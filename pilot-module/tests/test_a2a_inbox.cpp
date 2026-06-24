#include <logos_test.h>
#include "../src/pilot_impl.h"
#include "../src/pilot_a2a.h"
#include "../src/pilot_crypto.h"
#include "../src/pilot_llm.h"
#include <sqlite3.h>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <QString>
#include <QJsonObject>
#include <QJsonDocument>
#include <QByteArray>

// Deterministic, network-free LLM for the SAFE agent.ask service. isConfigured()==true so
// agentAsk runs the real dispatch path; complete() echoes the prompt so the result is
// predictable (and obviously NOT an error object), letting the inbound dispatcher reach
// 'completed' without any owner gate or network call.
class FakeLLM : public LLMProvider {
public:
    std::string complete(const std::string&, const std::vector<LLMMessage>& msgs) override {
        return "ANSWER: " + (msgs.empty() ? std::string() : msgs.back().content);
    }
    std::string model() const override { return "fake-1"; }
    std::string providerName() const override { return "fake"; }
    bool isConfigured() const override { return true; }
};

// Inbound A2A task server. The pure state machine (processInboundRequest) is also
// proven standalone in /tmp/a2a/test_inbox.cpp; this runs it through the real
// PilotImpl + on-disk SQLite in the module harness.

static std::string inboxDir(const std::string& name) {
    std::string base = "/tmp";
    if (const char* t = std::getenv("TMPDIR")) base = t;
    std::string dir = base + "/pilot_inbox_" + name;
    std::remove((dir + "/pilot.db").c_str());
    std::remove((dir + "/pilot.db-wal").c_str());
    std::remove((dir + "/pilot.db-shm").c_str());
    return dir;
}

// Count spend_requests rows for a given recipient. Used to prove program.deploy
// never opens a transfer to the literal "program_deploy" recipient (the old fake).
static int spendCountForRecipient(const std::string& dir, const std::string& recipient) {
    sqlite3* db = nullptr;
    sqlite3_open((dir + "/pilot.db").c_str(), &db);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM spend_requests WHERE recipient=?;", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, recipient.c_str(), -1, SQLITE_TRANSIENT);
    int n = 0;
    if (sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return n;
}

static std::string taskCol(const std::string& dir, const std::string& id, const char* column) {
    sqlite3* db = nullptr;
    sqlite3_open((dir + "/pilot.db").c_str(), &db);
    std::string sql = std::string("SELECT ") + column + " FROM inbound_tasks WHERE id=?;";
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr);
    sqlite3_bind_text(st, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    std::string v;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
        v = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    sqlite3_close(db);
    return v;
}

// State of a spend_requests row by id. Used to prove the inbound auto-approve path
// actually drives the spend through the FSM (terminal) rather than parking it HELD.
static std::string spendStateById(const std::string& dir, const std::string& id) {
    sqlite3* db = nullptr;
    sqlite3_open((dir + "/pilot.db").c_str(), &db);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT state FROM spend_requests WHERE id=?;", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    std::string v;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
        v = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    sqlite3_close(db);
    return v;
}

// --- outbound (requester-side) helpers ---------------------------------------------

static std::string outboundCol(const std::string& dir, const std::string& id, const char* column) {
    sqlite3* db = nullptr;
    sqlite3_open((dir + "/pilot.db").c_str(), &db);
    std::string sql = std::string("SELECT ") + column + " FROM outbound_tasks WHERE id=?;";
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr);
    sqlite3_bind_text(st, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    std::string v;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
        v = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    sqlite3_close(db);
    return v;
}

static void execSql(const std::string& dir, const std::string& sql) {
    sqlite3* db = nullptr;
    sqlite3_open((dir + "/pilot.db").c_str(), &db);
    sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(db);
}

// Force a spend_request into a specific state (drives executeSpend's idempotency guard).
static void forceSpendState(const std::string& dir, const std::string& id, const std::string& state) {
    execSql(dir, "UPDATE spend_requests SET state='" + state + "' WHERE id='" + id + "';");
}

// Seed a SIGNED, identity-bound discovered Agent Card so discoveredPayoutFor(agentAddress)
// resolves a payout. matchedCardLogos now only honors a card that verifyCardStatus()=='valid'
// (signed by its bound identity key AND consistent with the TOFU pin), so the card must be
// signed EXACTLY as PilotImpl::agentCard() signs: over the canonical compact bytes of the
// card WITHOUT the signature field. The card matches on _logos.npk reduced through the same
// encryption-key function the requester routes by; a plain npk matches a plain agentAddress.
static void seedDiscoveredCard(const std::string& dir, const std::string& npk,
                               const std::string& payout) {
    ECIESKeypair kp = generateECIESKeypair();
    QJsonObject logos;
    logos["npk"] = QString::fromStdString(npk);
    logos["payout"] = QString::fromStdString(payout);
    logos["signing_key"] = QString::fromStdString(kp.publicKeyHex);
    QJsonObject card;
    card["_logos"] = logos;

    std::string canonical = QJsonDocument(card).toJson(QJsonDocument::Compact).toStdString();
    std::vector<uint8_t> bytes(canonical.begin(), canonical.end());
    QJsonObject sig;
    sig["alg"] = QString("ES256K");
    sig["publicKey"] = QString::fromStdString(kp.publicKeyHex);
    sig["value"] = QString::fromStdString(signMessage(bytes, kp.privateKeyHex));
    card["signature"] = sig;
    std::string cardStr = QJsonDocument(card).toJson(QJsonDocument::Compact).toStdString();

    sqlite3* db = nullptr;
    sqlite3_open((dir + "/pilot.db").c_str(), &db);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO discovered_agents (npk, card_json, topic, last_seen) "
        "VALUES (?, ?, 't', '0');", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, npk.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, cardStr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    sqlite3_close(db);
}

// Seed a PENDING outbound task (state 'submitted') as agentTask would persist before send.
static void seedOutbound(const std::string& dir, const std::string& id, const std::string& agent,
                         const std::string& skill, int64_t price) {
    execSql(dir,
        "INSERT OR REPLACE INTO outbound_tasks "
        "(id, agent_address, skill, price, reply_topic, state, payout, spend_request_id, created_at, updated_at) "
        "VALUES ('" + id + "','" + agent + "','" + skill + "'," + std::to_string(price) +
        ",'/pilot/1/reply-" + id + "/proto','submitted','',NULL,'0','0');");
}

// Seed an outbound task caught mid-settle ('settling') linked to spendId (recovery input).
static void seedSettling(const std::string& dir, const std::string& id, const std::string& spendId) {
    execSql(dir,
        "INSERT OR REPLACE INTO outbound_tasks "
        "(id, agent_address, skill, price, reply_topic, state, payout, spend_request_id, created_at, updated_at) "
        "VALUES ('" + id + "','doerAgent','storage-upload',5,'/pilot/1/reply-" + id +
        "/proto','settling','doerPayout','" + spendId + "','0','0');");
}

// Seed a spend_request directly in a given state (recovery input).
static void seedSpend(const std::string& dir, const std::string& id, const std::string& state) {
    execSql(dir,
        "INSERT OR REPLACE INTO spend_requests "
        "(id, recipient, amount, reason, state, created_at, updated_at, expires_at) "
        "VALUES ('" + id + "','doerPayout',5,'r','" + state + "','0','0','0');");
}

LOGOS_TEST(inbound_tasks_table_created) {
    std::string dir = inboxDir("schema");
    PilotImpl impl;
    impl.initialize(dir);   // returns false without a wallet; db_ is still opened
    sqlite3* db = nullptr;
    sqlite3_open((dir + "/pilot.db").c_str(), &db);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT name FROM sqlite_master WHERE type='table' AND name='inbound_tasks';", -1, &st, nullptr);
    bool found = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    sqlite3_close(db);
    LOGOS_ASSERT_TRUE(found);
}

// The requester-side pay-on-acceptance loop persists pending outbound tasks so the
// reply consumer can settle exactly once. The table must exist after initialize().
LOGOS_TEST(outbound_tasks_table_created) {
    std::string dir = inboxDir("outbound_schema");
    PilotImpl impl;
    impl.initialize(dir);
    sqlite3* db = nullptr;
    sqlite3_open((dir + "/pilot.db").c_str(), &db);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT name FROM sqlite_master WHERE type='table' AND name='outbound_tasks';", -1, &st, nullptr);
    bool found = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    sqlite3_close(db);
    LOGOS_ASSERT_TRUE(found);
}

LOGOS_TEST(inbound_rejects_invalid_json) {
    std::string dir = inboxDir("badjson");
    PilotImpl impl; impl.initialize(dir);
    LOGOS_ASSERT_CONTAINS(impl.processInboundRequest("not json"), "-32700");
}

LOGOS_TEST(inbound_rejects_unknown_method) {
    std::string dir = inboxDir("badmethod");
    PilotImpl impl; impl.initialize(dir);
    std::string r = impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/destroy\",\"id\":\"x1\"}");
    LOGOS_ASSERT_CONTAINS(r, "-32601");
    LOGOS_ASSERT_CONTAINS(r, "x1");
}

LOGOS_TEST(inbound_ping_completes) {
    std::string dir = inboxDir("ping");
    PilotImpl impl; impl.initialize(dir);
    std::string r = impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-ping\",\"params\":{"
        "\"id\":\"t-ping\",\"metadata\":{\"skill\":\"ping\"},\"message\":{}},"
        "\"_logos\":{\"sender_npk\":\"peer\",\"reply_topic\":\"/r\"}}");
    LOGOS_ASSERT_CONTAINS(r, "completed");
    LOGOS_ASSERT_CONTAINS(r, "pong");
    LOGOS_ASSERT_EQ(taskCol(dir, "t-ping", "state"), std::string("completed"));
    LOGOS_ASSERT_EQ(taskCol(dir, "t-ping", "sender_npk"), std::string("peer"));
}

// ABOVE-THRESHOLD inbound costly task -> owner-approval gate (unchanged behavior). The
// per-tx limit is lowered to 10 so the 40-LEZ task clearly exceeds it; ambiguity must
// always route to the owner, never to autonomous execution.
LOGOS_TEST(inbound_costly_requires_owner_approval) {
    std::string dir = inboxDir("costly");
    PilotImpl impl; impl.initialize(dir);
    impl.setSpendingLimits(10, 500, 86400);   // 40-LEZ task is above the 10-LEZ per-tx cap
    std::string r = impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-pay\",\"params\":{"
        "\"id\":\"t-pay\",\"metadata\":{\"skill\":\"wallet-send\"},"
        "\"message\":{\"recipient\":\"deadbeef\",\"amount\":40,\"reason\":\"job\"}},"
        "\"_logos\":{\"sender_npk\":\"peer\",\"reply_topic\":\"/r\"}}");
    LOGOS_ASSERT_CONTAINS(r, "input-required");
    LOGOS_ASSERT_EQ(taskCol(dir, "t-pay", "state"), std::string("input-required"));
    std::string sid = taskCol(dir, "t-pay", "spend_request_id");
    LOGOS_ASSERT_FALSE(sid.empty());
    LOGOS_ASSERT_EQ(spendStateById(dir, sid), std::string("HELD"));   // parked for the owner
}

// FIX 3(c): an INBOUND wallet-send is owner-gated EVEN BELOW THRESHOLD. A requester is not
// the owner and is not authenticated as such, so we must NEVER auto-move our own funds at a
// stranger's request — regardless of amount. With default limits (per-tx 100, per-period
// 500) a 40-LEZ task fits both budgets, yet the agent must still HOLD it for the owner
// (input-required + spend HELD) rather than auto-execute. (Below-threshold autonomy stays on
// the OUTBOUND asker-pays-doer leg, not here.)
LOGOS_TEST(inbound_wallet_send_owner_gated_even_below_threshold) {
    std::string dir = inboxDir("auto");
    PilotImpl impl; impl.initialize(dir);   // default limits: per-tx 100, per-period 500
    std::string r = impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-auto\",\"params\":{"
        "\"id\":\"t-auto\",\"metadata\":{\"skill\":\"wallet-send\"},"
        "\"message\":{\"recipient\":\"deadbeef\",\"amount\":40,\"reason\":\"job\"}},"
        "\"_logos\":{\"sender_npk\":\"peer\",\"reply_topic\":\"/r\"}}");

    // Owner gate WAS taken despite the amount being below both budgets.
    LOGOS_ASSERT_CONTAINS(r, "input-required");
    LOGOS_ASSERT_EQ(taskCol(dir, "t-auto", "state"), std::string("input-required"));

    // The spend is parked HELD for the owner — never auto-executed.
    std::string sid = taskCol(dir, "t-auto", "spend_request_id");
    LOGOS_ASSERT_FALSE(sid.empty());
    LOGOS_ASSERT_EQ(spendStateById(dir, sid), std::string("HELD"));
}

// SAFE SERVICE skill (asker pays the doer): the doer AUTO-RUNS the real skill for a stranger
// and is PAID for it. agent-ask is pure compute (LLM Q&A) — no local files, no messaging
// identity, no funds — so it auto-completes with NO owner gate. The doer creates NO spend
// request and NEVER transfers money to the requester (senderNpk). It also stores the
// requester's ECIES key for encrypted replies (the M4 reply-encryption contract).
LOGOS_TEST(inbound_service_skill_dispatches_no_pay_to_requester) {
    std::string dir = inboxDir("service");
    PilotImpl impl; impl.initialize(dir);   // registry is built in the ctor (no wallet needed)
    pilotSetLLMProvider(impl,std::make_unique<FakeLLM>());   // deterministic, network-free
    std::string r = impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-svc\",\"params\":{"
        "\"id\":\"t-svc\",\"metadata\":{\"skill\":\"agent-ask\"},"
        "\"message\":{\"prompt\":\"what is 2+2\"}},"
        "\"_logos\":{\"sender_npk\":\"peer\",\"sender_ecies\":\"peerecies\",\"reply_topic\":\"/r\"}}");

    // The real skill ran autonomously and the task completed honestly — no owner gate.
    LOGOS_ASSERT_CONTAINS(r, "completed");
    LOGOS_ASSERT_EQ(taskCol(dir, "t-svc", "state"), std::string("completed"));
    LOGOS_ASSERT_TRUE(r.find("input-required") == std::string::npos);

    // No money flows to the requester, and a SERVICE task opens NO spend request.
    LOGOS_ASSERT_EQ(spendCountForRecipient(dir, "peer"), 0);
    LOGOS_ASSERT_EQ(spendCountForRecipient(dir, "peerecies"), 0);
    LOGOS_ASSERT_TRUE(taskCol(dir, "t-svc", "spend_request_id").empty());

    // The requester's ECIES key is persisted so replies encrypt to it (one A2A keypair).
    LOGOS_ASSERT_EQ(taskCol(dir, "t-svc", "sender_ecies"), std::string("peerecies"));
}

// FIX 2: a STRANGER's storage-upload is OWNER-GATED, never auto-run. storage-upload reads a
// local path (exfil risk), so an A2A peer requesting it must NOT cause any file read. The task
// parks at input-required (awaiting owner approval) with NO result and NO dispatch; because the
// requester pays US, there is also NO spend request. (ambiguity / privilege -> owner gate.)
LOGOS_TEST(inbound_storage_upload_owner_gated_no_file_read) {
    std::string dir = inboxDir("risky_upload");
    PilotImpl impl; impl.initialize(dir);
    std::string r = impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-up\",\"params\":{"
        "\"id\":\"t-up\",\"metadata\":{\"skill\":\"storage-upload\"},"
        "\"message\":{\"path\":\"/etc/passwd\",\"label\":\"x\"}},"
        "\"_logos\":{\"sender_npk\":\"peer\",\"sender_ecies\":\"pe\",\"reply_topic\":\"/r\"}}");

    // Owner gate taken: the peer is told input-required, never completed.
    LOGOS_ASSERT_CONTAINS(r, "input-required");
    LOGOS_ASSERT_TRUE(r.find("completed") == std::string::npos);
    LOGOS_ASSERT_EQ(taskCol(dir, "t-up", "state"), std::string("input-required"));

    // The skill NEVER ran: no result was recorded (no file was read / no CID produced)...
    LOGOS_ASSERT_TRUE(taskCol(dir, "t-up", "result_json").empty());
    // ...and an asker-pays-doer service opens NO spend request.
    LOGOS_ASSERT_TRUE(taskCol(dir, "t-up", "spend_request_id").empty());
}

// FIX 2: a STRANGER's messaging-send is OWNER-GATED, never auto-run. messaging-send would
// emit a message under THIS agent's identity (open-relay / impersonation), so a peer
// requesting it must NOT cause any send. The task parks at input-required with no dispatch.
LOGOS_TEST(inbound_messaging_send_owner_gated_no_send) {
    std::string dir = inboxDir("risky_msg");
    PilotImpl impl; impl.initialize(dir);
    std::string r = impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-msg\",\"params\":{"
        "\"id\":\"t-msg\",\"metadata\":{\"skill\":\"messaging-send\"},"
        "\"message\":{\"recipient\":\"victim\",\"message\":\"spam\"}},"
        "\"_logos\":{\"sender_npk\":\"peer\",\"sender_ecies\":\"pe\",\"reply_topic\":\"/r\"}}");

    LOGOS_ASSERT_CONTAINS(r, "input-required");
    LOGOS_ASSERT_TRUE(r.find("completed") == std::string::npos);
    LOGOS_ASSERT_EQ(taskCol(dir, "t-msg", "state"), std::string("input-required"));
    LOGOS_ASSERT_TRUE(taskCol(dir, "t-msg", "result_json").empty());   // nothing was sent
    LOGOS_ASSERT_TRUE(taskCol(dir, "t-msg", "spend_request_id").empty());
}

// FIX 2: the SAFE agent-ask service auto-completes for a stranger with NO owner gate and NO
// spend when an LLM is configured — this is the skill the autonomous-pay demo exercises.
LOGOS_TEST(inbound_agent_ask_autonomous_completes) {
    std::string dir = inboxDir("agentask_ok");
    PilotImpl impl; impl.initialize(dir);
    pilotSetLLMProvider(impl,std::make_unique<FakeLLM>());
    std::string r = impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-ask\",\"params\":{"
        "\"id\":\"t-ask\",\"metadata\":{\"skill\":\"agent-ask\"},"
        "\"message\":{\"prompt\":\"hello there\"}},"
        "\"_logos\":{\"sender_npk\":\"peer\",\"sender_ecies\":\"pe\",\"reply_topic\":\"/r\"}}");

    LOGOS_ASSERT_CONTAINS(r, "completed");
    LOGOS_ASSERT_EQ(taskCol(dir, "t-ask", "state"), std::string("completed"));
    LOGOS_ASSERT_CONTAINS(taskCol(dir, "t-ask", "result_json"), "answer");
    // Autonomous: no owner gate, no spend.
    LOGOS_ASSERT_TRUE(r.find("input-required") == std::string::npos);
    LOGOS_ASSERT_TRUE(taskCol(dir, "t-ask", "spend_request_id").empty());
}

// FIX 2 honesty: agent-ask still runs autonomously (no owner gate) but FAILS honestly when no
// LLM is configured — we never fabricate an answer to claim a 'completed' task and get paid for
// unproven work. The default no-wallet harness installs the NoOpProvider (not configured).
LOGOS_TEST(inbound_agent_ask_honest_error_without_llm) {
    std::string dir = inboxDir("agentask_nollm");
    PilotImpl impl; impl.initialize(dir);
    pilotSetLLMProvider(impl,nullptr);           // force NoOpProvider -> isConfigured()==false
    std::string r = impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-ask2\",\"params\":{"
        "\"id\":\"t-ask2\",\"metadata\":{\"skill\":\"agent-ask\"},"
        "\"message\":{\"prompt\":\"hello\"}},"
        "\"_logos\":{\"sender_npk\":\"peer\",\"sender_ecies\":\"pe\",\"reply_topic\":\"/r\"}}");

    LOGOS_ASSERT_EQ(taskCol(dir, "t-ask2", "state"), std::string("failed"));
    LOGOS_ASSERT_CONTAINS(taskCol(dir, "t-ask2", "result_json"), "error");
    // Failed without ever asking the owner (it is a SAFE skill, just unconfigured) and no spend.
    LOGOS_ASSERT_TRUE(taskCol(dir, "t-ask2", "spend_request_id").empty());
}

// FIX 3 HONEST SUCCESS CONTRACT: the inbound 'completed vs failed' verdict (a2aResultIsSuccess)
// pays the doer ONLY on an EXPLICIT, positive success signal. A serviced skill that reports it
// did NOT do the work — {"joined":false}, {"success":false}, status:failed — must mark the task
// 'failed' so the asker pays NOTHING; an opaque/ambiguous result (bare string, empty object/
// array, unparseable) is likewise 'failed'. Only a non-empty object with no negative signal, or
// a non-empty array, is 'completed'. Previously {"joined":false} and any non-empty string were
// paid as 'completed' — this pins the closed hole.
LOGOS_TEST(a2a_result_honest_success_contract) {
    // Explicit failure signals -> NOT success (asker pays nothing).
    LOGOS_ASSERT_FALSE(a2aResultIsSuccess("{\"joined\":false}"));
    LOGOS_ASSERT_FALSE(a2aResultIsSuccess("{\"success\":false}"));
    LOGOS_ASSERT_FALSE(a2aResultIsSuccess("{\"ok\":false}"));
    LOGOS_ASSERT_FALSE(a2aResultIsSuccess("{\"status\":\"failed\"}"));
    LOGOS_ASSERT_FALSE(a2aResultIsSuccess("{\"status\":\"error\"}"));
    LOGOS_ASSERT_FALSE(a2aResultIsSuccess("{\"error\":\"boom\"}"));

    // Opaque / ambiguous shapes -> NOT success (ambiguity is never paid).
    LOGOS_ASSERT_FALSE(a2aResultIsSuccess("done"));            // bare/unparseable string
    LOGOS_ASSERT_FALSE(a2aResultIsSuccess("\"done\""));        // JSON scalar string
    LOGOS_ASSERT_FALSE(a2aResultIsSuccess(""));                // empty
    LOGOS_ASSERT_FALSE(a2aResultIsSuccess("{}"));              // empty object
    LOGOS_ASSERT_FALSE(a2aResultIsSuccess("[]"));              // empty array

    // Genuine, positive results -> success (the doer actually produced something).
    LOGOS_ASSERT_TRUE(a2aResultIsSuccess("{\"answer\":\"4\"}"));
    LOGOS_ASSERT_TRUE(a2aResultIsSuccess("{\"joined\":true}"));
    LOGOS_ASSERT_TRUE(a2aResultIsSuccess("{\"success\":true,\"cid\":\"Qm\"}"));
    LOGOS_ASSERT_TRUE(a2aResultIsSuccess("[1,2,3]"));
}

// FIX 2 invariant: the auto-serviced catalog is SAFE-ONLY. No risky family (storage-*,
// messaging-*, wallet-*, program-*) may ever appear in a2aServiceCatalog(), because every
// catalog entry is auto-run for strangers and advertised as autonomously priced. agent-ask
// is present and priced. This is the single source of truth that keeps the advertised
// autonomous-price set == the auto-serviced set (the pricing<->serviced equality is also
// asserted card-side in test_a2a_outbound.cpp::agent_card_pricing_matches_serviced_skill_set).
LOGOS_TEST(a2a_service_catalog_is_safe_only) {
    bool sawAgentAsk = false;
    for (const auto& svc : a2aServiceCatalog()) {
        std::string id = svc.id;
        LOGOS_ASSERT_TRUE(id.rfind("storage-", 0) != 0);
        LOGOS_ASSERT_TRUE(id.rfind("messaging-", 0) != 0);
        LOGOS_ASSERT_TRUE(id.rfind("wallet-", 0) != 0);
        LOGOS_ASSERT_TRUE(id.rfind("program-", 0) != 0);
        if (id == "agent-ask") { sawAgentAsk = true; LOGOS_ASSERT_GT(svc.price, int64_t(0)); }
    }
    LOGOS_ASSERT_TRUE(sawAgentAsk);
}

// AGENT-SPENDING skill: wallet-send still goes through the spending FSM gate, and the
// spend targets the payee named in the task params — NEVER the requester (senderNpk).
LOGOS_TEST(inbound_wallet_send_pays_params_recipient_not_requester) {
    std::string dir = inboxDir("walletrecipient");
    PilotImpl impl; impl.initialize(dir);
    impl.setSpendingLimits(10, 500, 86400);   // 40-LEZ task -> above per-tx cap -> owner gate
    std::string r = impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-w\",\"params\":{"
        "\"id\":\"t-w\",\"metadata\":{\"skill\":\"wallet-send\"},"
        "\"message\":{\"recipient\":\"payee123\",\"amount\":40}},"
        "\"_logos\":{\"sender_npk\":\"peer\",\"reply_topic\":\"/r\"}}");

    // The threshold gate fired (held for owner), proving wallet-send is agent-spending.
    LOGOS_ASSERT_CONTAINS(r, "input-required");
    LOGOS_ASSERT_EQ(taskCol(dir, "t-w", "state"), std::string("input-required"));

    // The spend targets the params payee, never the requester.
    LOGOS_ASSERT_EQ(spendCountForRecipient(dir, "payee123"), 1);
    LOGOS_ASSERT_EQ(spendCountForRecipient(dir, "peer"), 0);
}

// FIX 3(a): IDEMPOTENCY. Waku is at-least-once, so a redelivered identical tasks/send must
// produce EXACTLY ONE spend/dispatch — never a second transfer (double-spend) or a second
// skill run. We deliver the same wallet-send twice; the second delivery must hit the cache
// (task already linked to a spend_request) and create no second spend_request row.
LOGOS_TEST(inbound_redelivered_send_is_idempotent) {
    std::string dir = inboxDir("idem");
    PilotImpl impl; impl.initialize(dir);
    const char* req =
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-idem\",\"params\":{"
        "\"id\":\"t-idem\",\"metadata\":{\"skill\":\"wallet-send\"},"
        "\"message\":{\"recipient\":\"dupRecipient\",\"amount\":40}},"
        "\"_logos\":{\"sender_npk\":\"peer\",\"reply_topic\":\"/r\"}}";

    std::string r1 = impl.processInboundRequest(req);
    std::string r2 = impl.processInboundRequest(req);   // redelivery (at-least-once)

    // Exactly ONE spend was opened across both deliveries — no double-spend, no re-dispatch.
    LOGOS_ASSERT_EQ(spendCountForRecipient(dir, "dupRecipient"), 1);
    // Both replies report the same (input-required) task state; the second is the cached one.
    LOGOS_ASSERT_CONTAINS(r1, "input-required");
    LOGOS_ASSERT_CONTAINS(r2, "input-required");
    LOGOS_ASSERT_EQ(taskCol(dir, "t-idem", "state"), std::string("input-required"));
}

// FIX 3(b): PARAMS EXTRACTION. The transport wraps the real args as an A2A message envelope
// {role, parts:[{type:text,text:<args>}]}, but parameterized handlers read FLAT top-level
// keys. The inbox must unwrap parts[0].text and feed THAT to the handler. We prove it on the
// only parameterized inbound handler with a wallet-free, db-observable effect: wallet-send
// reads recipient/amount via the same extraction the service-skill dispatch uses. A
// parts-wrapped envelope must yield a spend to the FLAT recipient; without extraction the
// recipient would be empty and NO spend would be created.
LOGOS_TEST(inbound_extracts_flat_args_from_parts_envelope) {
    std::string dir = inboxDir("flatargs");
    PilotImpl impl; impl.initialize(dir);
    std::string r = impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-flat\",\"params\":{"
        "\"id\":\"t-flat\",\"metadata\":{\"skill\":\"wallet-send\"},"
        "\"message\":{\"role\":\"user\",\"parts\":[{\"type\":\"text\","
        "\"text\":{\"recipient\":\"payeeFlat\",\"amount\":40}}]}},"
        "\"_logos\":{\"sender_npk\":\"peer\",\"reply_topic\":\"/r\"}}");

    // The flat args were unwrapped from parts[0].text: the spend targets the inner recipient,
    // and the handler did NOT fail on a missing recipient key.
    LOGOS_ASSERT_TRUE(r.find("missing recipient") == std::string::npos);
    LOGOS_ASSERT_EQ(spendCountForRecipient(dir, "payeeFlat"), 1);
    LOGOS_ASSERT_EQ(taskCol(dir, "t-flat", "state"), std::string("input-required"));
}

// FIX 3(b) regression: a SAFE SERVICE skill delivered inside the parts envelope still
// dispatches and completes. agent-ask reads its prompt from the unwrapped parts[0].text, so a
// parts-wrapped {prompt:...} must drive a clean run — proving the unwrap doesn't break the
// common path.
LOGOS_TEST(inbound_service_skill_dispatches_through_parts_envelope) {
    std::string dir = inboxDir("svc_parts");
    PilotImpl impl; impl.initialize(dir);
    pilotSetLLMProvider(impl,std::make_unique<FakeLLM>());
    std::string r = impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-svcp\",\"params\":{"
        "\"id\":\"t-svcp\",\"metadata\":{\"skill\":\"agent-ask\"},"
        "\"message\":{\"role\":\"user\",\"parts\":[{\"type\":\"text\","
        "\"text\":{\"prompt\":\"hi\"}}]}},"
        "\"_logos\":{\"sender_npk\":\"peer\",\"reply_topic\":\"/r\"}}");
    LOGOS_ASSERT_CONTAINS(r, "completed");
    LOGOS_ASSERT_EQ(taskCol(dir, "t-svcp", "state"), std::string("completed"));
}

LOGOS_TEST(inbound_cancel_marks_task_canceled) {
    std::string dir = inboxDir("cancel");
    PilotImpl impl; impl.initialize(dir);
    impl.setSpendingLimits(1, 1000000, 86400);   // force the owner gate (held, cancelable)
    impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-c\",\"params\":{"
        "\"id\":\"t-c\",\"metadata\":{\"skill\":\"wallet-send\"},"
        "\"message\":{\"recipient\":\"d\",\"amount\":5}},"
        "\"_logos\":{\"sender_npk\":\"peer\",\"reply_topic\":\"/r\"}}");
    std::string r = impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/cancel\",\"id\":\"rpc2\",\"params\":{\"id\":\"t-c\"}}");
    LOGOS_ASSERT_CONTAINS(r, "canceled");
    LOGOS_ASSERT_EQ(taskCol(dir, "t-c", "state"), std::string("canceled"));
}

LOGOS_TEST(inbound_cancel_of_completed_refused) {
    std::string dir = inboxDir("cancel2");
    PilotImpl impl; impl.initialize(dir);
    impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-d\",\"params\":{"
        "\"id\":\"t-d\",\"metadata\":{\"skill\":\"ping\"},\"message\":{}},"
        "\"_logos\":{\"sender_npk\":\"peer\",\"reply_topic\":\"/r\"}}");
    std::string r = impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/cancel\",\"id\":\"rpc3\",\"params\":{\"id\":\"t-d\"}}");
    LOGOS_ASSERT_CONTAINS(r, "-32002");
    LOGOS_ASSERT_EQ(taskCol(dir, "t-d", "state"), std::string("completed"));
}

LOGOS_TEST(inbound_task_fails_when_owner_rejects) {
    std::string dir = inboxDir("reject");
    PilotImpl impl; impl.initialize(dir);
    impl.setSpendingLimits(1, 1000000, 86400);   // force the owner gate so reject applies
    impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-rej\",\"params\":{"
        "\"id\":\"t-rej\",\"metadata\":{\"skill\":\"wallet-send\"},"
        "\"message\":{\"recipient\":\"d\",\"amount\":7}},"
        "\"_logos\":{\"sender_npk\":\"peer\",\"reply_topic\":\"/r\"}}");
    std::string sid = taskCol(dir, "t-rej", "spend_request_id");
    LOGOS_ASSERT_TRUE(impl.rejectSpend(sid));
    LOGOS_ASSERT_EQ(taskCol(dir, "t-rej", "state"), std::string("failed"));
}

LOGOS_TEST(inbound_task_fails_when_approval_expires) {
    std::string dir = inboxDir("expire");
    PilotImpl impl; impl.initialize(dir);
    impl.setSpendingLimits(1, 1000000, 86400);   // force the owner gate so expiry applies
    impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-exp\",\"params\":{"
        "\"id\":\"t-exp\",\"metadata\":{\"skill\":\"wallet-send\"},"
        "\"message\":{\"recipient\":\"d\",\"amount\":7}},"
        "\"_logos\":{\"sender_npk\":\"peer\",\"reply_topic\":\"/r\"}}");
    std::string sid = taskCol(dir, "t-exp", "spend_request_id");
    sqlite3* db = nullptr;
    sqlite3_open((dir + "/pilot.db").c_str(), &db);
    std::string sql = "UPDATE spend_requests SET expires_at='1000' WHERE id='" + sid + "';";
    sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(db);
    LOGOS_ASSERT_GT(impl.expireStaleSpends(), 0);
    LOGOS_ASSERT_EQ(taskCol(dir, "t-exp", "state"), std::string("failed"));
}

LOGOS_TEST(inbound_inflight_tasks_fail_on_restart) {
    std::string dir = inboxDir("restart");
    {
        PilotImpl impl; impl.initialize(dir);
        impl.processInboundRequest(
            "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-r\",\"params\":{"
            "\"id\":\"t-r\",\"metadata\":{\"skill\":\"ping\"},\"message\":{}},"
            "\"_logos\":{\"sender_npk\":\"peer\",\"reply_topic\":\"/r\"}}");
        sqlite3* db = nullptr;
        sqlite3_open((dir + "/pilot.db").c_str(), &db);
        sqlite3_exec(db, "UPDATE inbound_tasks SET state='working' WHERE id='t-r';", nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }
    PilotImpl impl2; impl2.initialize(dir);   // boot sweep fails in-flight tasks
    LOGOS_ASSERT_EQ(taskCol(dir, "t-r", "state"), std::string("failed"));
}

// program.deploy used to FAKE a deploy: it opened a 100-LEZ spend request to the
// literal recipient "program_deploy" and, on approval, transferred tokens there.
// The honest implementation attempts the real upstream deploy on
// logos_execution_zone and surfaces an error when that runtime is absent — it must
// NEVER create a "program_deploy" spend request or an owner-approval envelope.
LOGOS_TEST(program_deploy_never_creates_program_deploy_spend) {
    std::string dir = inboxDir("deploy_nospend");
    PilotImpl impl; impl.initialize(dir);   // no wallet wired -> logos_execution_zone absent

    std::string r = impl.programDeploy("/nonexistent/program.bin");

    // (a) the faked transfer to the magic recipient must not exist.
    LOGOS_ASSERT_EQ(spendCountForRecipient(dir, "program_deploy"), 0);
    // sanity: programDeploy must not have created ANY spend request from this path.
    LOGOS_ASSERT_EQ(spendCountForRecipient(dir, "/nonexistent/program.bin"), 0);

    // (b) honest error, never the old "held for owner approval" envelope.
    LOGOS_ASSERT_CONTAINS(r, "error");
    LOGOS_ASSERT_TRUE(r.find("\"held\"") == std::string::npos);
    LOGOS_ASSERT_TRUE(r.find("request_id") == std::string::npos);
    LOGOS_ASSERT_TRUE(r.find("requires owner approval") == std::string::npos);
}

// Even with a non-empty (but unwired) data dir, deploy stays honest: no tokens move,
// no approval is requested, and the caller gets a truthful error rather than a
// fabricated success.
LOGOS_TEST(program_deploy_returns_honest_error_when_upstream_absent) {
    std::string dir = inboxDir("deploy_honest");
    PilotImpl impl; impl.initialize(dir);

    std::string r = impl.programDeploy("/tmp/whatever.bin");

    LOGOS_ASSERT_CONTAINS(r, "error");
    LOGOS_ASSERT_TRUE(r.find("\"status\":\"completed\"") == std::string::npos);
    LOGOS_ASSERT_TRUE(r.find("\"held\"") == std::string::npos);
    LOGOS_ASSERT_EQ(spendCountForRecipient(dir, "program_deploy"), 0);
}

// -- REWORK 4: outbound settlement idempotency, terminal transitions, recovery -------

// (a) executeSpend is a NO-OP on a spend already in a terminal/in-flight execution state:
// it must never re-run a transfer for one that is COMPLETED/EXECUTING/TX_FAILED. We force
// each state and assert the row is UNCHANGED (a non-idempotent impl would drive COMPLETED
// to TX_FAILED here, since no wallet is wired).
LOGOS_TEST(execute_spend_idempotent_on_terminal_states) {
    std::string dir = inboxDir("exec_idem");
    PilotImpl impl; impl.initialize(dir);

    std::string c = impl.createSpendRequest("payee", 5, "r");
    forceSpendState(dir, c, "COMPLETED");
    LOGOS_ASSERT_TRUE(impl.executeSpend(c));                       // no-op success
    LOGOS_ASSERT_EQ(spendStateById(dir, c), std::string("COMPLETED"));

    std::string e = impl.createSpendRequest("payee", 5, "r");
    forceSpendState(dir, e, "EXECUTING");
    LOGOS_ASSERT_FALSE(impl.executeSpend(e));                      // mid-flight, left alone
    LOGOS_ASSERT_EQ(spendStateById(dir, e), std::string("EXECUTING"));

    std::string f = impl.createSpendRequest("payee", 5, "r");
    forceSpendState(dir, f, "TX_FAILED");
    LOGOS_ASSERT_FALSE(impl.executeSpend(f));                      // never retried into a 2nd transfer
    LOGOS_ASSERT_EQ(spendStateById(dir, f), std::string("TX_FAILED"));
}

// (b) Two accept/complete replies for ONE outbound task settle EXACTLY ONCE: the atomic
// submitted->settling claim means the duplicate reply creates no second spend_request and
// no second transfer. Price 5 is below the default per-tx cap (100), so it auto-pays; with
// no wallet the transfer honestly fails (TX_FAILED -> 'pay-failed'), but the POINT proven
// is single settlement (exactly one spend to the declared payout).
LOGOS_TEST(outbound_double_reply_settles_exactly_once) {
    std::string dir = inboxDir("outbound_once");
    PilotImpl impl; impl.initialize(dir);
    seedDiscoveredCard(dir, "doerAgent", "doerPayout");
    seedOutbound(dir, "ob1", "doerAgent", "storage-upload", 5);

    impl.settleOutboundReply("ob1", "completed");
    impl.settleOutboundReply("ob1", "completed");   // duplicate reply must NOT pay twice

    LOGOS_ASSERT_EQ(spendCountForRecipient(dir, "doerPayout"), 1);   // exactly one spend
    LOGOS_ASSERT_EQ(spendCountForRecipient(dir, "doerAgent"), 0);    // never the messaging id (M5)
    LOGOS_ASSERT_FALSE(outboundCol(dir, "ob1", "spend_request_id").empty());
    // Terminal under the no-wallet harness; 'paid' with a live wallet.
    LOGOS_ASSERT_EQ(outboundCol(dir, "ob1", "state"), std::string("pay-failed"));
}

// (c) An ABOVE-THRESHOLD outbound payment routes to the owner: the task parks in
// 'awaiting-approval' (spend HELD), then approveSpend advances it to a TERMINAL state so it
// never orphans (M6). No wallet -> the approved transfer honestly fails -> 'pay-failed';
// with a live wallet that terminal is 'paid'.
LOGOS_TEST(outbound_above_threshold_awaits_then_terminal_on_approve) {
    std::string dir = inboxDir("outbound_approve");
    PilotImpl impl; impl.initialize(dir);
    impl.setSpendingLimits(10, 500, 86400);         // price 40 > per-tx 10 -> owner gate
    seedDiscoveredCard(dir, "doerAgent", "doerPayout");
    seedOutbound(dir, "ob2", "doerAgent", "storage-upload", 40);

    impl.settleOutboundReply("ob2", "completed");
    LOGOS_ASSERT_EQ(outboundCol(dir, "ob2", "state"), std::string("awaiting-approval"));
    std::string sid = outboundCol(dir, "ob2", "spend_request_id");
    LOGOS_ASSERT_FALSE(sid.empty());
    LOGOS_ASSERT_EQ(spendStateById(dir, sid), std::string("HELD"));   // parked for the owner

    impl.approveSpend(sid);
    std::string fin = outboundCol(dir, "ob2", "state");
    LOGOS_ASSERT_TRUE(fin != std::string("awaiting-approval"));       // no orphan
    LOGOS_ASSERT_TRUE(fin != std::string("settling"));
    LOGOS_ASSERT_EQ(fin, std::string("pay-failed"));                 // 'paid' with a live wallet
}

// FIX 3 (M6 orphan): an ABOVE-THRESHOLD outbound payment parks in 'awaiting-approval' behind a
// HELD spend. If that spend EXPIRES unapproved, the money never moves — and the linked outbound
// task must ALSO reach a terminal state, exactly as approveSpend/rejectSpend drive it. Before
// this fix expireStaleSpends only failed the inbound side, orphaning the outbound row in
// 'awaiting-approval' forever. Now expiry drives it to 'pay-failed'.
LOGOS_TEST(outbound_above_threshold_expiry_drives_terminal) {
    std::string dir = inboxDir("outbound_expire");
    PilotImpl impl; impl.initialize(dir);
    impl.setSpendingLimits(10, 500, 86400);         // price 40 > per-tx 10 -> owner gate
    seedDiscoveredCard(dir, "doerAgent", "doerPayout");
    seedOutbound(dir, "ob3", "doerAgent", "storage-upload", 40);

    impl.settleOutboundReply("ob3", "completed");
    LOGOS_ASSERT_EQ(outboundCol(dir, "ob3", "state"), std::string("awaiting-approval"));
    std::string sid = outboundCol(dir, "ob3", "spend_request_id");
    LOGOS_ASSERT_FALSE(sid.empty());
    LOGOS_ASSERT_EQ(spendStateById(dir, sid), std::string("HELD"));

    // Force the approval window into the past, then run the expiry sweep.
    execSql(dir, "UPDATE spend_requests SET expires_at='1000' WHERE id='" + sid + "';");
    LOGOS_ASSERT_GT(impl.expireStaleSpends(), 0);

    LOGOS_ASSERT_EQ(spendStateById(dir, sid), std::string("EXPIRED"));      // spend cancelled
    LOGOS_ASSERT_EQ(outboundCol(dir, "ob3", "state"), std::string("pay-failed"));  // no orphan
    // The transfer never happened: NO spend ever paid out to the doer's payout id.
    LOGOS_ASSERT_EQ(spendCountForRecipient(dir, "doerPayout"), 1);   // the HELD-then-EXPIRED row only
}

// (M7) Restart recovery reconciles tasks caught mid-settle ('settling') against their
// linked spend_request: COMPLETED -> 'paid', terminal-failed -> 'pay-failed', still
// in-flight (HELD) -> LEFT for retry. outboundTasksRecover() runs from initialize().
LOGOS_TEST(outbound_recover_reconciles_settling_against_spend) {
    std::string dir = inboxDir("outbound_recover");
    {
        PilotImpl impl; impl.initialize(dir);
        seedSettling(dir, "obP", "spP"); seedSpend(dir, "spP", "COMPLETED");
        seedSettling(dir, "obF", "spF"); seedSpend(dir, "spF", "TX_FAILED");
        seedSettling(dir, "obH", "spH"); seedSpend(dir, "spH", "HELD");
    }
    PilotImpl impl2; impl2.initialize(dir);          // initialize() -> outboundTasksRecover()

    LOGOS_ASSERT_EQ(outboundCol(dir, "obP", "state"), std::string("paid"));        // COMPLETED -> paid
    LOGOS_ASSERT_EQ(outboundCol(dir, "obF", "state"), std::string("pay-failed"));  // TX_FAILED -> pay-failed
    LOGOS_ASSERT_EQ(outboundCol(dir, "obH", "state"), std::string("settling"));    // in flight -> left for retry
}

// -- FIX 1: A2A messaging identity unified on the ECIES key --------------------------

// Seed the doer's single A2A ECIES keypair into pilot.db so initialize()/loadIdentity
// restores it as agentEciesPub_/agentEciesPriv_ (the keys load from config 'ecies.*'). The
// no-wallet harness then leaves that keypair intact (createIdentity bails before it would
// regenerate one), so the doer holds the private half the round trip needs to decrypt.
static void seedEciesIdentity(const std::string& dir, const ECIESKeypair& kp) {
    execSql(dir, "INSERT OR REPLACE INTO agent_identity (id, npk, account_id, created_at) "
                 "VALUES (1,'agentnpk','acct','0');");
    execSql(dir, "INSERT OR REPLACE INTO config (key,value) VALUES ('ecies.pub','"
                 + kp.publicKeyHex + "');");
    execSql(dir, "INSERT OR REPLACE INTO config (key,value) VALUES ('ecies.priv','"
                 + kp.privateKeyHex + "');");
}

// The full request leg a real peer drives. A requester ECIES-encrypts a tasks/send to the
// doer's agentEciesPub_ — the key the doer's Agent Card advertises as BOTH the inbox id and
// _logos.signing_key — and sends it to the doer's inbox. The doer's transport entry
// handleInboundA2A must decrypt it with agentEciesPriv_ (the key it HOLDS) and dispatch it
// through the state machine. This is the leg that was BLOCKED before FIX 1: the requester
// encrypted to extractEncryptionKey(npk) (the wallet VIEWING key), which the doer cannot
// decrypt. Driven through handleInboundA2A (NOT processInboundRequest in isolation) so the
// decrypt seam is exercised end to end.
LOGOS_TEST(inbound_ecies_roundtrip_decrypts_and_dispatches) {
    std::string dir = inboxDir("ecies_roundtrip");
    ECIESKeypair kp = generateECIESKeypair();
    { PilotImpl boot; boot.initialize(dir); }   // create schema (no wallet -> returns false)
    seedEciesIdentity(dir, kp);
    PilotImpl doer; doer.initialize(dir);        // loadIdentity() restores the ECIES keypair

    // Requester builds a tasks/send and encrypts it to the doer's PUBLISHED ECIES key.
    std::string req =
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-rt\",\"params\":{"
        "\"id\":\"t-rt\",\"metadata\":{\"skill\":\"ping\"},\"message\":{}},"
        "\"_logos\":{\"sender_npk\":\"peer\",\"sender_ecies\":\"peerEcies\","
        "\"reply_topic\":\"/pilot/1/reply-t-rt/proto\"}}";
    std::vector<uint8_t> plain(req.begin(), req.end());
    std::string payload = eciesSerialize(eciesEncrypt(kp.publicKeyHex, plain));

    doer.handleInboundA2A(payload);   // decrypt with agentEciesPriv_ -> dispatch

    // Decrypt succeeded AND the task dispatched through the state machine: ping auto-
    // completes, and the requester's sender_ecies was persisted for the reply leg.
    LOGOS_ASSERT_EQ(taskCol(dir, "t-rt", "state"), std::string("completed"));
    LOGOS_ASSERT_EQ(taskCol(dir, "t-rt", "sender_ecies"), std::string("peerEcies"));
}

// Negative control: a payload encrypted to a DIFFERENT key cannot be decrypted with the
// doer's agentEciesPriv_, so handleInboundA2A drops it (ambiguity -> inaction) and creates
// NO task row. Proves the decrypt is real, not a pass-through that would dispatch anything.
LOGOS_TEST(inbound_ecies_wrong_key_is_dropped) {
    std::string dir = inboxDir("ecies_wrongkey");
    ECIESKeypair kp = generateECIESKeypair();
    { PilotImpl boot; boot.initialize(dir); }
    seedEciesIdentity(dir, kp);
    PilotImpl doer; doer.initialize(dir);

    ECIESKeypair other = generateECIESKeypair();   // NOT the doer's key
    std::string req =
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-bad\",\"params\":{"
        "\"id\":\"t-bad\",\"metadata\":{\"skill\":\"ping\"},\"message\":{}},"
        "\"_logos\":{\"sender_npk\":\"peer\",\"sender_ecies\":\"peerEcies\",\"reply_topic\":\"/r\"}}";
    std::vector<uint8_t> plain(req.begin(), req.end());
    std::string payload = eciesSerialize(eciesEncrypt(other.publicKeyHex, plain));

    doer.handleInboundA2A(payload);   // undecryptable with the doer's key -> dropped

    LOGOS_ASSERT_TRUE(taskCol(dir, "t-bad", "state").empty());   // no task created
}
