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

// L5: an LLM whose complete() RE-ENTERS processInboundRequest with a tasks/cancel for the
// in-flight agent-ask task, forcing a re-entrant terminal transition DURING dispatch. The
// agent-ask path's late 'completed' then races the cancel's 'canceled'; the monotonic UPDATE
// must keep the FIRST terminal. isConfigured()==true so agent-ask runs the real dispatch path.
class ReentrantCancelLLM : public LLMProvider {
public:
    ReentrantCancelLLM(PilotImpl* impl, std::string taskId)
        : impl_(impl), taskId_(std::move(taskId)) {}
    std::string complete(const std::string&, const std::vector<LLMMessage>&) override {
        // Direct FSM call (empty authenticatedNpk -> ownership binding skipped); the task is
        // still 'working', so the cancel succeeds and drives it to the 'canceled' terminal.
        impl_->processInboundRequest(
            "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/cancel\",\"id\":\"rpc-rc\",\"params\":{\"id\":\""
            + taskId_ + "\"}}", "", false);
        return "ANSWER: reentrant";
    }
    std::string model() const override { return "reentrant-1"; }
    std::string providerName() const override { return "reentrant"; }
    bool isConfigured() const override { return true; }
private:
    PilotImpl* impl_;
    std::string taskId_;
};

// L6: an LLM whose complete() RE-ENTERS processInboundRequest with a SECOND inbound agent-ask
// while this (outer) agent-ask is in-flight. The concurrency bound must refuse the nested call
// BEFORE it nests a second blocking complete() on the single delivery thread, so the nested task
// ends 'failed' (busy) and the outer still completes. The one-shot guard keeps a regressed (un-
// bounded) build from recursing forever — it would instead complete the nested task, which the
// test catches. isConfigured()==true so agent-ask runs the real dispatch path.
class ReentrantAskLLM : public LLMProvider {
public:
    explicit ReentrantAskLLM(PilotImpl* impl) : impl_(impl) {}
    std::string complete(const std::string&, const std::vector<LLMMessage>&) override {
        if (!nested_) {
            nested_ = true;
            impl_->processInboundRequest(
                "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-ask-nested\",\"params\":{"
                "\"id\":\"t-ask-nested\",\"metadata\":{\"skill\":\"agent-ask\"},"
                "\"message\":{\"prompt\":\"nested\"}},"
                "\"_logos\":{\"sender_npk\":\"peer\",\"reply_topic\":\"/r\"}}", "", false);
        }
        return "ANSWER: outer";
    }
    std::string model() const override { return "reentrant-ask-1"; }
    std::string providerName() const override { return "reentrant-ask"; }
    bool isConfigured() const override { return true; }
private:
    PilotImpl* impl_;
    bool nested_ = false;
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

// Read a single config value by key (L1: enc.pub / enc.priv at-rest assertions). Opens its own
// connection like the other readers; empty string when the key is absent.
static std::string configVal(const std::string& dir, const std::string& key) {
    sqlite3* db = nullptr;
    sqlite3_open((dir + "/pilot.db").c_str(), &db);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT value FROM config WHERE key=?;", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    std::string v;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
        v = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    sqlite3_close(db);
    return v;
}

// Force a spend_request into a specific state (drives executeSpend's idempotency guard).
static void forceSpendState(const std::string& dir, const std::string& id, const std::string& state) {
    execSql(dir, "UPDATE spend_requests SET state='" + state + "' WHERE id='" + id + "';");
}

// Seed a SIGNED, identity-bound discovered Agent Card so discoveredPayoutFor(agentAddress)
// resolves a payout. Under H1 discoveredPayoutFor refuses to pay a card whose _logos.payout !=
// _logos.npk (a card could otherwise redirect funds to a third account), so this helper binds
// payout = npk. matchedCardLogos only honors a card that verifyCardStatus()=='valid' (signed by
// its bound identity key AND consistent with the TOFU pin), so the card is signed EXACTLY as
// PilotImpl::agentCard() signs: over the canonical compact bytes of the card WITHOUT the
// signature field. Returns the keypair so a caller can assert the spend never targets the
// signing key.
static ECIESKeypair seedDiscoveredCard(const std::string& dir, const std::string& npk) {
    ECIESKeypair kp = generateECIESKeypair();
    QJsonObject logos;
    logos["npk"] = QString::fromStdString(npk);
    logos["payout"] = QString::fromStdString(npk);   // H1: payout bound to the card identity
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
    return kp;
}

// Sign a tasks/* REQUEST envelope (H2) exactly as the production signA2AEnvelope path does:
// set _logos.signing_key, drop any prior _logos.signature, sign the canonical compact bytes
// (signing_key present, signature absent), then attach _logos.signature. The doer's
// verifyInboundRequest reproduces these bytes and verifies them against signing_key. Mirrors
// makeSignedReply (test_a2a_outbound.cpp), for request rather than reply envelopes.
static std::string signRequest(QJsonObject env, const ECIESKeypair& kp) {
    QJsonObject logos = env["_logos"].toObject();
    logos["signing_key"] = QString::fromStdString(kp.publicKeyHex);
    logos.remove("signature");
    env["_logos"] = logos;
    std::string canonical = QJsonDocument(env).toJson(QJsonDocument::Compact).toStdString();
    std::vector<uint8_t> bytes(canonical.begin(), canonical.end());
    logos["signature"] = QString::fromStdString(signMessage(bytes, kp.privateKeyHex));
    env["_logos"] = logos;
    return QJsonDocument(env).toJson(QJsonDocument::Compact).toStdString();
}

// Pin sender_npk -> signing_key in the DEDICATED pinned_request_identities table (H2) — the
// SEPARATE namespace verifyInboundRequest uses, never the card pin (pinned_identities). CREATEs
// the table first so a fresh doer DB still pins (folds PM1-P3). A later request presenting that
// npk under a different signing_key is then dropped as a pin mismatch.
static void pinRequestIdentity(const std::string& dir, const std::string& npk,
                               const std::string& signingKey) {
    sqlite3* db = nullptr;
    sqlite3_open((dir + "/pilot.db").c_str(), &db);
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS pinned_request_identities "
        "(npk TEXT PRIMARY KEY, signing_key TEXT NOT NULL, first_seen TEXT NOT NULL);",
        nullptr, nullptr, nullptr);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO pinned_request_identities (npk, signing_key, first_seen) "
        "VALUES (?, ?, '1');", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, npk.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, signingKey.c_str(), -1, SQLITE_TRANSIENT);
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

// The requester-side pay-on-completion loop persists pending outbound tasks so the
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
    LOGOS_ASSERT_CONTAINS(impl.processInboundRequest("not json", "", false), "-32700");
}

LOGOS_TEST(inbound_rejects_unknown_method) {
    std::string dir = inboxDir("badmethod");
    PilotImpl impl; impl.initialize(dir);
    std::string r = impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/destroy\",\"id\":\"x1\"}", "", false);
    LOGOS_ASSERT_CONTAINS(r, "-32601");
    LOGOS_ASSERT_CONTAINS(r, "x1");
}

LOGOS_TEST(inbound_ping_completes) {
    std::string dir = inboxDir("ping");
    PilotImpl impl; impl.initialize(dir);
    std::string r = impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-ping\",\"params\":{"
        "\"id\":\"t-ping\",\"metadata\":{\"skill\":\"ping\"},\"message\":{}},"
        "\"_logos\":{\"sender_npk\":\"peer\",\"reply_topic\":\"/r\"}}", "", false);
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
        "\"_logos\":{\"sender_npk\":\"peer\",\"reply_topic\":\"/r\"}}", "", false);
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
        "\"_logos\":{\"sender_npk\":\"peer\",\"reply_topic\":\"/r\"}}", "", false);

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
        "\"_logos\":{\"sender_npk\":\"peer\",\"sender_ecies\":\"peerecies\",\"reply_topic\":\"/r\"}}", "", false);

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
        "\"_logos\":{\"sender_npk\":\"peer\",\"sender_ecies\":\"pe\",\"reply_topic\":\"/r\"}}", "", false);

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
        "\"_logos\":{\"sender_npk\":\"peer\",\"sender_ecies\":\"pe\",\"reply_topic\":\"/r\"}}", "", false);

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
        "\"_logos\":{\"sender_npk\":\"peer\",\"sender_ecies\":\"pe\",\"reply_topic\":\"/r\"}}", "", false);

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
        "\"_logos\":{\"sender_npk\":\"peer\",\"sender_ecies\":\"pe\",\"reply_topic\":\"/r\"}}", "", false);

    LOGOS_ASSERT_EQ(taskCol(dir, "t-ask2", "state"), std::string("failed"));
    LOGOS_ASSERT_CONTAINS(taskCol(dir, "t-ask2", "result_json"), "error");
    // Failed without ever asking the owner (it is a SAFE skill, just unconfigured) and no spend.
    LOGOS_ASSERT_TRUE(taskCol(dir, "t-ask2", "spend_request_id").empty());
}

// H3: the SAFE auto-run path (agent-ask) is billable LLM work. Post-H2 a caller must be an
// authenticated, TOFU-pinned npk, so we rate-limit per sender: the first kA2AServiceMaxPerWindow
// (10) succeed, the next is refused WITHOUT running the LLM, and a DIFFERENT sender is unaffected.
LOGOS_TEST(inbound_service_rate_limited_after_burst) {
    std::string dir = inboxDir("svc_ratelimit");
    PilotImpl impl; impl.initialize(dir);
    pilotSetLLMProvider(impl, std::make_unique<FakeLLM>());
    auto askReq = [](const std::string& id, const std::string& sender) {
        return "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"" + id + "\",\"params\":{"
               "\"id\":\"" + id + "\",\"metadata\":{\"skill\":\"agent-ask\"},"
               "\"message\":{\"prompt\":\"hi\"}},"
               "\"_logos\":{\"sender_npk\":\"" + sender + "\",\"sender_ecies\":\"pe\",\"reply_topic\":\"/r\"}}";
    };
    for (int i = 0; i < 10; ++i) {
        std::string id = "rl-" + std::to_string(i);
        impl.processInboundRequest(askReq(id, "peer"), "peer", false);
        LOGOS_ASSERT_EQ(taskCol(dir, id, "state"), std::string("completed"));
    }
    // The 11th from the same authenticated sender within the window is rate-limited (no LLM run).
    std::string r = impl.processInboundRequest(askReq("rl-10", "peer"), "peer", false);
    LOGOS_ASSERT_CONTAINS(r, "rate limit");
    LOGOS_ASSERT_EQ(taskCol(dir, "rl-10", "state"), std::string("failed"));
    LOGOS_ASSERT_TRUE(taskCol(dir, "rl-10", "result_json").find("answer") == std::string::npos);
    // A DIFFERENT authenticated sender has its own quota and still completes.
    impl.processInboundRequest(askReq("rl-other", "peer2"), "peer2", false);
    LOGOS_ASSERT_EQ(taskCol(dir, "rl-other", "state"), std::string("completed"));
}

// H3: an oversized serviced request is refused BEFORE the billable LLM runs, so a single huge
// prompt cannot inflate cost. The size cap applies regardless of authentication.
LOGOS_TEST(inbound_service_prompt_too_large_is_rejected) {
    std::string dir = inboxDir("svc_toobig");
    PilotImpl impl; impl.initialize(dir);
    pilotSetLLMProvider(impl, std::make_unique<FakeLLM>());
    std::string big(9000, 'x');   // argsStr exceeds kA2AMaxServiceBytes (8192)
    std::string req =
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-big\",\"params\":{"
        "\"id\":\"t-big\",\"metadata\":{\"skill\":\"agent-ask\"},"
        "\"message\":{\"prompt\":\"" + big + "\"}},"
        "\"_logos\":{\"sender_npk\":\"peer\",\"sender_ecies\":\"pe\",\"reply_topic\":\"/r\"}}";
    std::string r = impl.processInboundRequest(req, "", false);
    LOGOS_ASSERT_CONTAINS(r, "size limit");
    LOGOS_ASSERT_EQ(taskCol(dir, "t-big", "state"), std::string("failed"));
    LOGOS_ASSERT_TRUE(taskCol(dir, "t-big", "result_json").find("answer") == std::string::npos);
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
        "\"_logos\":{\"sender_npk\":\"peer\",\"reply_topic\":\"/r\"}}", "", false);

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

    std::string r1 = impl.processInboundRequest(req, "", false);
    std::string r2 = impl.processInboundRequest(req, "", false);   // redelivery (at-least-once)

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
        "\"_logos\":{\"sender_npk\":\"peer\",\"reply_topic\":\"/r\"}}", "", false);

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
        "\"_logos\":{\"sender_npk\":\"peer\",\"reply_topic\":\"/r\"}}", "", false);
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
        "\"_logos\":{\"sender_npk\":\"peer\",\"reply_topic\":\"/r\"}}", "", false);
    std::string r = impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/cancel\",\"id\":\"rpc2\",\"params\":{\"id\":\"t-c\"}}", "", false);
    LOGOS_ASSERT_CONTAINS(r, "canceled");
    LOGOS_ASSERT_EQ(taskCol(dir, "t-c", "state"), std::string("canceled"));
}

LOGOS_TEST(inbound_cancel_of_completed_refused) {
    std::string dir = inboxDir("cancel2");
    PilotImpl impl; impl.initialize(dir);
    impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-d\",\"params\":{"
        "\"id\":\"t-d\",\"metadata\":{\"skill\":\"ping\"},\"message\":{}},"
        "\"_logos\":{\"sender_npk\":\"peer\",\"reply_topic\":\"/r\"}}", "", false);
    std::string r = impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/cancel\",\"id\":\"rpc3\",\"params\":{\"id\":\"t-d\"}}", "", false);
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
        "\"_logos\":{\"sender_npk\":\"peer\",\"reply_topic\":\"/r\"}}", "", false);
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
        "\"_logos\":{\"sender_npk\":\"peer\",\"reply_topic\":\"/r\"}}", "", false);
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
            "\"_logos\":{\"sender_npk\":\"peer\",\"reply_topic\":\"/r\"}}", "", false);
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
// lez_core and surfaces an error when that runtime is absent — it must
// NEVER create a "program_deploy" spend request or an owner-approval envelope.
LOGOS_TEST(program_deploy_never_creates_program_deploy_spend) {
    std::string dir = inboxDir("deploy_nospend");
    PilotImpl impl; impl.initialize(dir);   // no wallet wired -> lez_core absent

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
    ECIESKeypair kp = seedDiscoveredCard(dir, "doerAgent");   // H1: payout == npk == doerAgent
    seedOutbound(dir, "ob1", "doerAgent", "storage-upload", 5);

    impl.settleOutboundReply("ob1", "completed");
    impl.settleOutboundReply("ob1", "completed");   // duplicate reply must NOT pay twice

    LOGOS_ASSERT_EQ(spendCountForRecipient(dir, "doerAgent"), 1);    // exactly one spend, to the payout
    LOGOS_ASSERT_EQ(spendCountForRecipient(dir, kp.publicKeyHex), 0);   // never the signing key (M5)
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
    seedDiscoveredCard(dir, "doerAgent");   // H1: payout == npk == doerAgent
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
    seedDiscoveredCard(dir, "doerAgent");   // H1: payout == npk == doerAgent
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
    LOGOS_ASSERT_EQ(spendCountForRecipient(dir, "doerAgent"), 1);   // the HELD-then-EXPIRED row only
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

    // Requester builds a tasks/send, SIGNS it with its A2A keypair (H2), then encrypts the
    // signed envelope to the doer's PUBLISHED ECIES key. handleInboundA2A now REQUIRES the
    // signature (verifyInboundRequest) before it will dispatch.
    ECIESKeypair peer = generateECIESKeypair();
    QJsonObject metadata; metadata["skill"] = QString("ping");
    QJsonObject params;
    params["id"] = QString("t-rt");
    params["metadata"] = metadata;
    params["message"] = QJsonObject();
    QJsonObject logos;
    logos["sender_npk"] = QString("peer");
    logos["sender_ecies"] = QString::fromStdString(peer.publicKeyHex);
    logos["reply_topic"] = QString("/pilot/1/reply-t-rt/proto");
    QJsonObject env;
    env["jsonrpc"] = QString("2.0");
    env["method"] = QString("tasks/send");
    env["id"] = QString("t-rt");
    env["params"] = params;
    env["_logos"] = logos;
    std::string req = signRequest(env, peer);
    std::vector<uint8_t> plain(req.begin(), req.end());
    std::string payload = eciesSerialize(eciesEncrypt(kp.publicKeyHex, plain));

    doer.handleInboundA2A(payload);   // decrypt with agentEciesPriv_ -> verify -> dispatch

    // Decrypt + signature verification succeeded AND the task dispatched through the state
    // machine: ping auto-completes, and the requester's sender_ecies was persisted for the reply.
    LOGOS_ASSERT_EQ(taskCol(dir, "t-rt", "state"), std::string("completed"));
    LOGOS_ASSERT_EQ(taskCol(dir, "t-rt", "sender_ecies"), std::string(peer.publicKeyHex));
}

// The same leg entered where a STORE-POLLED message enters: handleInboundMessage is the one
// funnel the live delivery event and the pull path (agentPoll/pollStore) share, so a payload
// on the doer's own inbox topic must reach handleInboundA2A and dispatch exactly as the live
// event would — and a payload on a topic we do not own must be ignored quietly.
LOGOS_TEST(inbound_message_on_inbox_topic_routes_to_a2a) {
    std::string dir = inboxDir("inbound_message_route");
    ECIESKeypair kp = generateECIESKeypair();
    { PilotImpl boot; boot.initialize(dir); }
    seedEciesIdentity(dir, kp);
    PilotImpl doer; doer.initialize(dir);

    ECIESKeypair peer = generateECIESKeypair();
    QJsonObject metadata; metadata["skill"] = QString("ping");
    QJsonObject params;
    params["id"] = QString("t-route");
    params["metadata"] = metadata;
    params["message"] = QJsonObject();
    QJsonObject logos;
    logos["sender_npk"] = QString("peer");
    logos["sender_ecies"] = QString::fromStdString(peer.publicKeyHex);
    logos["reply_topic"] = QString("/pilot/1/reply-t-route/proto");
    QJsonObject env;
    env["jsonrpc"] = QString("2.0");
    env["method"] = QString("tasks/send");
    env["id"] = QString("t-route");
    env["params"] = params;
    env["_logos"] = logos;
    std::string req = signRequest(env, peer);
    std::vector<uint8_t> plain(req.begin(), req.end());
    std::string payload = eciesSerialize(eciesEncrypt(kp.publicKeyHex, plain));

    // Wrong topic first: nothing may dispatch, so the task must not exist yet.
    doer.handleInboundMessage("/pilot/1/inbox-someone-else/proto", payload);
    LOGOS_ASSERT_EQ(taskCol(dir, "t-route", "state"), std::string(""));

    // Our own inbox topic: routed to handleInboundA2A, decrypted, verified, dispatched.
    doer.handleInboundMessage("/pilot/1/inbox-" + kp.publicKeyHex + "/proto", payload);
    LOGOS_ASSERT_EQ(taskCol(dir, "t-route", "state"), std::string("completed"));
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

// ===================== Wave 1 hardening: H2 inbound authentication ===================

// H2: handleInboundA2A now AUTHENTICATES every request before dispatch. An UNSIGNED request
// (no _logos.signature) is dropped — ambiguity defaults to inaction — so no task row exists.
LOGOS_TEST(inbound_unsigned_request_is_dropped) {
    std::string dir = inboxDir("unsigned_req");
    ECIESKeypair kp = generateECIESKeypair();
    { PilotImpl boot; boot.initialize(dir); }
    seedEciesIdentity(dir, kp);
    PilotImpl doer; doer.initialize(dir);

    std::string req =
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-uns\",\"params\":{"
        "\"id\":\"t-uns\",\"metadata\":{\"skill\":\"ping\"},\"message\":{}},"
        "\"_logos\":{\"sender_npk\":\"peer\",\"sender_ecies\":\"peerEcies\",\"reply_topic\":\"/r\"}}";
    std::vector<uint8_t> plain(req.begin(), req.end());
    doer.handleInboundA2A(eciesSerialize(eciesEncrypt(kp.publicKeyHex, plain)));

    LOGOS_ASSERT_TRUE(taskCol(dir, "t-uns", "state").empty());   // never dispatched
}

// H2: a request whose _logos.signing_key claims the victim's key but whose signature was made
// with the attacker's key fails verifySignature, so it is dropped before dispatch.
LOGOS_TEST(inbound_forged_signature_request_is_dropped) {
    std::string dir = inboxDir("forged_req");
    ECIESKeypair kp = generateECIESKeypair();
    { PilotImpl boot; boot.initialize(dir); }
    seedEciesIdentity(dir, kp);
    PilotImpl doer; doer.initialize(dir);

    ECIESKeypair victim = generateECIESKeypair();
    ECIESKeypair attacker = generateECIESKeypair();
    QJsonObject metadata; metadata["skill"] = QString("ping");
    QJsonObject params;
    params["id"] = QString("t-forge");
    params["metadata"] = metadata;
    params["message"] = QJsonObject();
    QJsonObject logos;
    logos["sender_npk"] = QString("peer");
    logos["sender_ecies"] = QString::fromStdString(victim.publicKeyHex);
    logos["reply_topic"] = QString("/r");
    logos["signing_key"] = QString::fromStdString(victim.publicKeyHex);   // claims the victim key
    QJsonObject env;
    env["jsonrpc"] = QString("2.0");
    env["method"] = QString("tasks/send");
    env["id"] = QString("t-forge");
    env["params"] = params;
    env["_logos"] = logos;
    // Sign the canonical bytes with the ATTACKER key -> signature does not verify vs signing_key.
    std::string canonical = QJsonDocument(env).toJson(QJsonDocument::Compact).toStdString();
    std::vector<uint8_t> cbytes(canonical.begin(), canonical.end());
    logos["signature"] = QString::fromStdString(signMessage(cbytes, attacker.privateKeyHex));
    env["_logos"] = logos;
    std::string req = QJsonDocument(env).toJson(QJsonDocument::Compact).toStdString();

    std::vector<uint8_t> plain(req.begin(), req.end());
    doer.handleInboundA2A(eciesSerialize(eciesEncrypt(kp.publicKeyHex, plain)));

    LOGOS_ASSERT_TRUE(taskCol(dir, "t-forge", "state").empty());   // forged signature -> dropped
}

// H2 (TOFU): once "peer" is pinned to a first-contact key K1, a self-consistent request signed
// by a DIFFERENT key K2 is a pin mismatch and is dropped (first-seen key wins).
LOGOS_TEST(inbound_pin_mismatch_request_is_dropped) {
    std::string dir = inboxDir("pin_mismatch");
    ECIESKeypair kp = generateECIESKeypair();
    { PilotImpl boot; boot.initialize(dir); }
    seedEciesIdentity(dir, kp);
    ECIESKeypair k1 = generateECIESKeypair();
    pinRequestIdentity(dir, "peer", k1.publicKeyHex);   // genuine first contact already on record
    PilotImpl doer; doer.initialize(dir);

    ECIESKeypair k2 = generateECIESKeypair();
    QJsonObject metadata; metadata["skill"] = QString("ping");
    QJsonObject params;
    params["id"] = QString("t-pin");
    params["metadata"] = metadata;
    params["message"] = QJsonObject();
    QJsonObject logos;
    logos["sender_npk"] = QString("peer");
    logos["sender_ecies"] = QString::fromStdString(k2.publicKeyHex);
    logos["reply_topic"] = QString("/r");
    QJsonObject env;
    env["jsonrpc"] = QString("2.0");
    env["method"] = QString("tasks/send");
    env["id"] = QString("t-pin");
    env["params"] = params;
    env["_logos"] = logos;
    std::string req = signRequest(env, k2);   // self-consistent, but signed by the WRONG key

    std::vector<uint8_t> plain(req.begin(), req.end());
    doer.handleInboundA2A(eciesSerialize(eciesEncrypt(kp.publicKeyHex, plain)));

    LOGOS_ASSERT_TRUE(taskCol(dir, "t-pin", "state").empty());   // K2 dropped; K1 still wins
}

// P4 / PM2#2 regression: an inbound request pins into a SEPARATE table (pinned_request_identities)
// and can NEVER poison the card/payout pin (pinned_identities). An attacker pins
// pinned_request_identities["V"] = attackerKey via a signed request claiming sender_npk "V";
// V's genuine Agent Card (signed by V's OWN key) must still verify 'valid' and resolve V's payout.
LOGOS_TEST(inbound_request_pin_does_not_poison_card_pin) {
    std::string dir = inboxDir("pin_isolation");
    ECIESKeypair kp = generateECIESKeypair();
    { PilotImpl boot; boot.initialize(dir); }
    seedEciesIdentity(dir, kp);
    PilotImpl doer; doer.initialize(dir);

    // Attacker's signed request claiming to be "V" -> pins pinned_request_identities[V]=attacker.
    ECIESKeypair attacker = generateECIESKeypair();
    QJsonObject metadata; metadata["skill"] = QString("ping");
    QJsonObject params;
    params["id"] = QString("t-poison");
    params["metadata"] = metadata;
    params["message"] = QJsonObject();
    QJsonObject logos;
    logos["sender_npk"] = QString("V");
    logos["sender_ecies"] = QString::fromStdString(attacker.publicKeyHex);
    logos["reply_topic"] = QString("/r");
    QJsonObject env;
    env["jsonrpc"] = QString("2.0");
    env["method"] = QString("tasks/send");
    env["id"] = QString("t-poison");
    env["params"] = params;
    env["_logos"] = logos;
    std::string req = signRequest(env, attacker);
    std::vector<uint8_t> plain(req.begin(), req.end());
    doer.handleInboundA2A(eciesSerialize(eciesEncrypt(kp.publicKeyHex, plain)));
    LOGOS_ASSERT_EQ(taskCol(dir, "t-poison", "state"), std::string("completed"));   // accepted + pinned

    // V publishes its GENUINE card (payout == npk == V), signed by V's OWN key.
    seedDiscoveredCard(dir, "V");

    // discoveredPayoutFor is private, so drive V's payout resolution through the PUBLIC settle
    // seam: a 'completed' reply for a task addressed to V resolves V's payout and opens exactly
    // one spend to V. A poisoned card pin would verify 'invalid' and open NO spend.
    seedOutbound(dir, "obV", "V", "storage-upload", 5);
    doer.settleOutboundReply("obV", "completed");
    LOGOS_ASSERT_EQ(spendCountForRecipient(dir, "V"), 1);   // genuine payout resolved

    // Direct check: V's genuine card still verifies 'valid' (request pin lives in a separate
    // table and cannot corrupt the card pin).
    sqlite3* vdb = nullptr;
    sqlite3_open((dir + "/pilot.db").c_str(), &vdb);
    sqlite3_stmt* cst = nullptr;
    sqlite3_prepare_v2(vdb, "SELECT card_json FROM discovered_agents WHERE npk='V';", -1, &cst, nullptr);
    std::string cardStr;
    if (sqlite3_step(cst) == SQLITE_ROW && sqlite3_column_text(cst, 0))
        cardStr = reinterpret_cast<const char*>(sqlite3_column_text(cst, 0));
    sqlite3_finalize(cst);
    QJsonObject vCard = QJsonDocument::fromJson(QByteArray::fromStdString(cardStr)).object();
    LOGOS_ASSERT_EQ(verifyCardStatus(vCard, vdb).toStdString(), std::string("valid"));
    sqlite3_close(vdb);
}

// H2 ownership binding: only the original requester (the task's stored sender_npk) may cancel.
// A transport-authenticated non-owner gets -32003 and the task is untouched.
LOGOS_TEST(inbound_cancel_rejects_non_owner) {
    std::string dir = inboxDir("cancel_owner");
    PilotImpl impl; impl.initialize(dir);
    impl.setSpendingLimits(1, 1000000, 86400);   // force the owner gate -> task stays cancelable
    impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-own\",\"params\":{"
        "\"id\":\"t-own\",\"metadata\":{\"skill\":\"wallet-send\"},"
        "\"message\":{\"recipient\":\"d\",\"amount\":5}},"
        "\"_logos\":{\"sender_npk\":\"alice\",\"reply_topic\":\"/r\"}}",
        "alice", false);

    // bob is not the original requester -> unauthorized; task untouched.
    std::string rb = impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/cancel\",\"id\":\"rpcB\",\"params\":{\"id\":\"t-own\"}}",
        "bob", false);
    LOGOS_ASSERT_CONTAINS(rb, "-32003");
    LOGOS_ASSERT_EQ(taskCol(dir, "t-own", "state"), std::string("input-required"));

    // alice (the requester) can cancel.
    std::string ra = impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/cancel\",\"id\":\"rpcA\",\"params\":{\"id\":\"t-own\"}}",
        "alice", false);
    LOGOS_ASSERT_CONTAINS(ra, "canceled");
    LOGOS_ASSERT_EQ(taskCol(dir, "t-own", "state"), std::string("canceled"));
}

// H2 ownership binding: only the original requester may read a task's result via sendSubscribe,
// so a third party cannot exfiltrate the paid-for answer. A non-owner gets -32003.
LOGOS_TEST(inbound_sendSubscribe_rejects_non_owner) {
    std::string dir = inboxDir("subscribe_owner");
    PilotImpl impl; impl.initialize(dir);
    impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-sub\",\"params\":{"
        "\"id\":\"t-sub\",\"metadata\":{\"skill\":\"ping\"},\"message\":{}},"
        "\"_logos\":{\"sender_npk\":\"alice\",\"reply_topic\":\"/r\"}}",
        "alice", false);

    std::string rb = impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/sendSubscribe\",\"id\":\"rpcB\",\"params\":{\"id\":\"t-sub\"}}",
        "bob", false);
    LOGOS_ASSERT_CONTAINS(rb, "-32003");

    // alice gets the task state back (ping auto-completed).
    std::string ra = impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/sendSubscribe\",\"id\":\"rpcA\",\"params\":{\"id\":\"t-sub\"}}",
        "alice", false);
    LOGOS_ASSERT_CONTAINS(ra, "completed");
}

// ===================== Wave 1 hardening: M4 wallet-send owner prompt =================

// M4: the inbound wallet-send owner-approval prompt now carries the payee recipient and reason.
LOGOS_TEST(inbound_wallet_send_owner_notification_includes_recipient) {
    std::string msg = a2aWalletSendApprovalMessage(
        "wallet-send", "peerNpk", 40, "payee123", "rent", "spend-7");
    LOGOS_ASSERT_CONTAINS(msg, "Skill: wallet-send");
    LOGOS_ASSERT_CONTAINS(msg, "From: peerNpk");
    LOGOS_ASSERT_CONTAINS(msg, "Amount: 40 LEZ");
    LOGOS_ASSERT_CONTAINS(msg, "To: payee123");
    LOGOS_ASSERT_CONTAINS(msg, "Reason: rent");
    LOGOS_ASSERT_CONTAINS(msg, "/approve spend-7");
}

// M4: an empty reason is omitted entirely (no dangling "Reason:" line).
LOGOS_TEST(inbound_wallet_send_owner_notification_omits_empty_reason) {
    std::string msg = a2aWalletSendApprovalMessage(
        "wallet-send", "peerNpk", 40, "payeeX", "", "spend-9");
    LOGOS_ASSERT_CONTAINS(msg, "To: payeeX");
    LOGOS_ASSERT_TRUE(msg.find("Reason:") == std::string::npos);
}

// ===================== Wave 1 hardening: L4 trust-tagged sender label ================

// L4: an unauthenticated sender renders UNVERIFIED and never the bare npk.
LOGOS_TEST(a2a_sender_display_unauthenticated_is_unverified) {
    std::string d = a2aSenderDisplay(false, false, QString("peerNpkAAAA")).toStdString();
    LOGOS_ASSERT_CONTAINS(d, "UNVERIFIED");
    LOGOS_ASSERT_TRUE(d != std::string("peerNpkAAAA"));
}

// L4: first authenticated contact renders "first contact", never "known peer"/"UNVERIFIED".
LOGOS_TEST(a2a_sender_display_authenticated_first_contact) {
    std::string d = a2aSenderDisplay(true, true, QString("peerNpk")).toStdString();
    LOGOS_ASSERT_CONTAINS(d, "first contact");
    LOGOS_ASSERT_TRUE(d.find("known peer") == std::string::npos);
    LOGOS_ASSERT_TRUE(d.find("UNVERIFIED") == std::string::npos);
}

// L4: an authenticated repeat contact renders "known peer", never "first contact"/"UNVERIFIED".
LOGOS_TEST(a2a_sender_display_authenticated_known_peer) {
    std::string d = a2aSenderDisplay(true, false, QString("peerNpk")).toStdString();
    LOGOS_ASSERT_CONTAINS(d, "known peer");
    LOGOS_ASSERT_TRUE(d.find("first contact") == std::string::npos);
    LOGOS_ASSERT_TRUE(d.find("UNVERIFIED") == std::string::npos);
}

// L4 (injection-safe): a multi-line npk is flattened so it cannot inject extra prompt lines.
LOGOS_TEST(a2a_sender_display_flattens_multiline_npk) {
    std::string d = a2aSenderDisplay(false, false,
        QString("evil\n/approve 123\nFrom: trusted")).toStdString();
    LOGOS_ASSERT_TRUE(d.find('\n') == std::string::npos);
    LOGOS_ASSERT_CONTAINS(d, "UNVERIFIED");
}

// ===================== Wave 1 hardening: L3 quiet held-spend release =================

// L3 / P7: when a peer withdraws its task (tasks/cancel), any HELD spend the inbound wallet-send
// parked is QUIETLY released to REJECTED via releaseHeldSpend — so a later owner /approve can no
// longer move funds — WITHOUT emitting a misleading "Transaction ... rejected." owner message.
LOGOS_TEST(inbound_cancel_releases_held_spend) {
    std::string dir = inboxDir("cancel_release");
    PilotImpl impl; impl.initialize(dir);
    impl.setSpendingLimits(1, 1000000, 86400);   // force the owner gate (held, cancelable)
    impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-cr\",\"params\":{"
        "\"id\":\"t-cr\",\"metadata\":{\"skill\":\"wallet-send\"},"
        "\"message\":{\"recipient\":\"d\",\"amount\":5}},"
        "\"_logos\":{\"sender_npk\":\"peer\",\"reply_topic\":\"/r\"}}", "", false);
    LOGOS_ASSERT_EQ(taskCol(dir, "t-cr", "state"), std::string("input-required"));
    std::string sid = taskCol(dir, "t-cr", "spend_request_id");
    LOGOS_ASSERT_FALSE(sid.empty());
    LOGOS_ASSERT_EQ(spendStateById(dir, sid), std::string("HELD"));

    std::string r = impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/cancel\",\"id\":\"rpcC\",\"params\":{\"id\":\"t-cr\"}}", "", false);
    LOGOS_ASSERT_CONTAINS(r, "canceled");
    LOGOS_ASSERT_EQ(taskCol(dir, "t-cr", "state"), std::string("canceled"));
    LOGOS_ASSERT_EQ(spendStateById(dir, sid), std::string("REJECTED"));

    // The released spend can no longer be approved into a transfer, and stays REJECTED.
    LOGOS_ASSERT_FALSE(impl.approveSpend(sid));
    LOGOS_ASSERT_EQ(spendStateById(dir, sid), std::string("REJECTED"));

    // P7 (quiet release): the cancel acknowledgement is the plain 'canceled' reply and carries
    // NO "Transaction ... rejected." text; the task went to 'canceled', NEVER the 'failed' state
    // rejectSpend's resumeInboundTask would drive. (The no-wallet harness has no wired owner
    // channel — logosAPI_ is null / ownerChannelId_ empty -> sendToOwner is a no-op with no
    // observable sink — so the reply + task state are the observable proof the release was quiet.)
    LOGOS_ASSERT_TRUE(r.find("rejected") == std::string::npos);
    LOGOS_ASSERT_TRUE(r.find("Transaction") == std::string::npos);
    LOGOS_ASSERT_TRUE(taskCol(dir, "t-cr", "state") != std::string("failed"));
}

// ===================== Wave 1 hardening: L5 monotonic terminal state =================

// L5: a re-entrant terminal during dispatch never gets clobbered. The injected LLM cancels the
// in-flight agent-ask task mid-complete(); the late 'completed' loses the monotonic UPDATE, so
// the task ends 'canceled' and the reply reports 'canceled', never 'completed'.
LOGOS_TEST(inbound_terminal_state_monotonic_under_reentrant_cancel) {
    std::string dir = inboxDir("reentrant_cancel");
    PilotImpl impl; impl.initialize(dir);
    pilotSetLLMProvider(impl, std::make_unique<ReentrantCancelLLM>(&impl, "t-rc"));
    std::string r = impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-rc\",\"params\":{"
        "\"id\":\"t-rc\",\"metadata\":{\"skill\":\"agent-ask\"},"
        "\"message\":{\"prompt\":\"hello\"}},"
        "\"_logos\":{\"sender_npk\":\"peer\",\"reply_topic\":\"/r\"}}", "", false);

    LOGOS_ASSERT_EQ(taskCol(dir, "t-rc", "state"), std::string("canceled"));
    LOGOS_ASSERT_CONTAINS(r, "canceled");
    LOGOS_ASSERT_TRUE(r.find("completed") == std::string::npos);
}

// ===================== Wave 2 hardening: M3 resource caps / eviction =================

// Generic single-value COUNT(*) read against the test DB (no per-table helper exists for
// discovered_agents / pinned_identities). Opens its own connection like the other readers.
static int countRows(const std::string& dir, const std::string& sql) {
    sqlite3* db = nullptr;
    sqlite3_open((dir + "/pilot.db").c_str(), &db);
    sqlite3_stmt* st = nullptr;
    int n = 0;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return n;
}

// M3 (params-size cap): a request whose FULL message envelope (params_json) exceeds
// kA2AMaxParamsBytes (16384) is rejected with -32005 BEFORE any row is stored. The inner flat
// args (parts[0].text) are tiny — only a huge SIBLING field bloats the envelope — so this trips
// the new envelope gate, not the H3 serviced-args (8192) gate, and stores NOTHING.
LOGOS_TEST(inbound_oversized_envelope_is_rejected_and_not_stored) {
    std::string dir = inboxDir("oversized_envelope");
    PilotImpl impl; impl.initialize(dir);
    std::string huge(20000, 'x');   // sibling key bloats params_json past 16384
    std::string req =
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-ovr\",\"params\":{"
        "\"id\":\"t-ovr\",\"metadata\":{\"skill\":\"ping\"},"
        "\"message\":{\"role\":\"user\",\"parts\":[{\"type\":\"text\",\"text\":{\"prompt\":\"hi\"}}],"
        "\"junk\":\"" + huge + "\"}},"
        "\"_logos\":{\"sender_npk\":\"peer\",\"reply_topic\":\"/r\"}}";
    std::string r = impl.processInboundRequest(req, "", false);
    LOGOS_ASSERT_CONTAINS(r, "size limit");
    LOGOS_ASSERT_CONTAINS(r, "-32005");
    // No row was stored at all (gate runs before the INSERT): both state and params_json empty.
    LOGOS_ASSERT_TRUE(taskCol(dir, "t-ovr", "state").empty());
    LOGOS_ASSERT_TRUE(taskCol(dir, "t-ovr", "params_json").empty());
}

// M3 (global per-sender flood gate): the cap spans ALL skills, not just the serviced sublimit.
// The first kA2AInboundMaxPerWindow (30) tasks/send from one authenticated sender complete; the
// 31st is refused with -32006 (state 'failed') without running; a DIFFERENT sender is unaffected.
LOGOS_TEST(inbound_any_skill_rate_limited_per_sender) {
    std::string dir = inboxDir("global_flood");
    PilotImpl impl; impl.initialize(dir);
    auto pingReq = [](const std::string& id, const std::string& sender) {
        return "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"" + id + "\",\"params\":{"
               "\"id\":\"" + id + "\",\"metadata\":{\"skill\":\"ping\"},\"message\":{}},"
               "\"_logos\":{\"sender_npk\":\"" + sender + "\",\"reply_topic\":\"/r\"}}";
    };
    for (int i = 0; i < 30; ++i) {
        std::string id = "fl-" + std::to_string(i);
        impl.processInboundRequest(pingReq(id, "peer"), "peer", false);
        LOGOS_ASSERT_EQ(taskCol(dir, id, "state"), std::string("completed"));
    }
    // The 31st from the same authenticated sender within the window is flood-limited.
    std::string r = impl.processInboundRequest(pingReq("fl-30", "peer"), "peer", false);
    LOGOS_ASSERT_CONTAINS(r, "rate limit");
    LOGOS_ASSERT_CONTAINS(r, "-32006");
    LOGOS_ASSERT_EQ(taskCol(dir, "fl-30", "state"), std::string("failed"));
    // A DIFFERENT authenticated sender has its own quota and still completes.
    impl.processInboundRequest(pingReq("fl-other", "peer2"), "peer2", false);
    LOGOS_ASSERT_EQ(taskCol(dir, "fl-other", "state"), std::string("completed"));
}

// M3 (TTL sweep): a2aEvictOldInboundTasks drops TERMINAL rows older than kA2AInboundTaskTTLSec
// (86400) while leaving in-flight rows and fresh terminal rows untouched.
LOGOS_TEST(inbound_terminal_tasks_evicted_by_ttl_inflight_survives) {
    std::string dir = inboxDir("ttl_evict");
    PilotImpl impl; impl.initialize(dir);   // create schema
    auto seedTask = [&](const std::string& id, const std::string& state, const std::string& createdAt) {
        execSql(dir,
            "INSERT OR REPLACE INTO inbound_tasks "
            "(id,sender_npk,reply_topic,skill,params_json,state,created_at,updated_at) VALUES ('"
            + id + "','peer','/r','ping','{}','" + state + "','" + createdAt + "','" + createdAt + "');");
    };
    seedTask("old-done",   "completed",      "1000");          // terminal + old   -> swept
    seedTask("old-wait",   "input-required", "1000");          // in-flight + old  -> survives
    seedTask("fresh-done", "completed",      "5000000000");    // terminal + fresh -> survives

    sqlite3* db = nullptr;
    sqlite3_open((dir + "/pilot.db").c_str(), &db);
    a2aEvictOldInboundTasks(db, 5000000000L);   // now == fresh-done; far past old-* + TTL
    sqlite3_close(db);

    LOGOS_ASSERT_TRUE(taskCol(dir, "old-done", "state").empty());
    LOGOS_ASSERT_EQ(taskCol(dir, "old-wait", "state"), std::string("input-required"));
    LOGOS_ASSERT_EQ(taskCol(dir, "fresh-done", "state"), std::string("completed"));
}

// M3 [FIX-C/FIX-F] (row-cap backstop): with the TTL sweep inert (all rows fresh), the row cap
// keeps only the newest kA2AInboundTaskMaxRows (5000) terminal rows. Proves the subquery
// `id NOT IN (SELECT ... LIMIT k)` DELETE actually runs on stock SQLite (a bare DELETE...LIMIT
// would silently no-op). The constant is file-local in pilot_a2a_inbox.cpp, so 5000 is mirrored
// here.
LOGOS_TEST(inbound_rowcap_evicts_terminal_beyond_max) {
    std::string dir = inboxDir("rowcap_evict");
    PilotImpl impl; impl.initialize(dir);
    // Seed 5005 fresh terminal rows with strictly ascending created_at (rc-1 oldest .. rc-5005 newest).
    execSql(dir,
        "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x < 5005) "
        "INSERT INTO inbound_tasks (id,sender_npk,reply_topic,skill,params_json,state,created_at,updated_at) "
        "SELECT 'rc-'||x,'peer','/r','ping','{}','completed',CAST(1000000000+x AS TEXT),'0' FROM c;");

    sqlite3* db = nullptr;
    sqlite3_open((dir + "/pilot.db").c_str(), &db);
    a2aEvictOldInboundTasks(db, 1000005105L);   // now > every created_at but within TTL -> only row-cap fires
    sqlite3_close(db);

    LOGOS_ASSERT_EQ(countRows(dir, "SELECT COUNT(*) FROM inbound_tasks WHERE state='completed';"), 5000);
    LOGOS_ASSERT_TRUE(taskCol(dir, "rc-1", "state").empty());     // 5 oldest evicted by the cap
    LOGOS_ASSERT_TRUE(taskCol(dir, "rc-5", "state").empty());
    LOGOS_ASSERT_EQ(taskCol(dir, "rc-6", "state"), std::string("completed"));      // just inside the cap
    LOGOS_ASSERT_EQ(taskCol(dir, "rc-5005", "state"), std::string("completed"));   // newest survives
}

// M3 [FIX-B/FIX-E] (discovery-cache trim): a2aEvictDiscoveryCache LRU-trims ONLY discovered_agents
// to kA2ADiscoveredAgentsMax (1000) by last_seen. It NEVER touches the TOFU pin tables, and it
// NEVER evicts a card backing a non-terminal outbound_task — even when that card's last_seen is
// stale and outside the cap. A genuinely stale, non-pinned, non-inflight card is evicted. The cap
// is file-local in pilot_a2a.cpp, so 1005 fresher fillers are seeded to force the trim.
LOGOS_TEST(discovery_cache_trim_never_evicts_pins_or_inflight) {
    std::string dir = inboxDir("disc_trim");
    PilotImpl impl; impl.initialize(dir);   // schema: discovered_agents, pinned_identities, outbound_tasks

    // cached: a discovered_agents card (stale) AND a TOFU pin row.
    execSql(dir, "INSERT OR REPLACE INTO discovered_agents (npk,card_json,topic,last_seen) VALUES ('cached','{}','t','1000');");
    execSql(dir, "INSERT OR REPLACE INTO pinned_identities (npk,signing_key,first_seen) VALUES ('cached','k','1');");
    // pinnedOnly: a pin row with NO card and no outbound task.
    execSql(dir, "INSERT OR REPLACE INTO pinned_identities (npk,signing_key,first_seen) VALUES ('pinnedOnly','k','1');");
    // inflight: a discovered_agents card with STALE last_seen + a non-terminal outbound task.
    execSql(dir, "INSERT OR REPLACE INTO discovered_agents (npk,card_json,topic,last_seen) VALUES ('inflight','{}','t','1001');");
    seedOutbound(dir, "ob-inflight", "inflight", "agent-ask", 5);   // state 'submitted'
    // staleVictim: stale, NOT pinned, NOT inflight -> must be evicted once outside the cap.
    execSql(dir, "INSERT OR REPLACE INTO discovered_agents (npk,card_json,topic,last_seen) VALUES ('staleVictim','{}','t','1002');");
    // > kA2ADiscoveredAgentsMax (1000) FRESHER cards push the stale rows outside the freshest-N window.
    execSql(dir,
        "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x < 1005) "
        "INSERT INTO discovered_agents (npk,card_json,topic,last_seen) "
        "SELECT 'fill-'||x,'{}','t',CAST(2000000000+x AS TEXT) FROM c;");

    sqlite3* db = nullptr;
    sqlite3_open((dir + "/pilot.db").c_str(), &db);
    a2aEvictDiscoveryCache(db);
    sqlite3_close(db);

    // [FIX-B] both pin rows survive — the cache trim never touches pinned_identities.
    LOGOS_ASSERT_EQ(countRows(dir, "SELECT COUNT(*) FROM pinned_identities WHERE npk='cached';"), 1);
    LOGOS_ASSERT_EQ(countRows(dir, "SELECT COUNT(*) FROM pinned_identities WHERE npk='pinnedOnly';"), 1);
    // [FIX-E] the in-flight card survives despite a stale last_seen.
    LOGOS_ASSERT_EQ(countRows(dir, "SELECT COUNT(*) FROM discovered_agents WHERE npk='inflight';"), 1);
    // A genuinely stale, non-pinned, non-inflight card outside the cap is evicted.
    LOGOS_ASSERT_EQ(countRows(dir, "SELECT COUNT(*) FROM discovered_agents WHERE npk='staleVictim';"), 0);
}

// ===================== Wave 3: L8 atomic money-state transitions =====================

// L8 (outbound recover step 0): an unlinked 'settling' row (spend_request_id='') never moved
// money, so the self-heal re-arms it to 'submitted' on restart — still unlinked, no spend created.
// (A LINKED settling row is left to step (2)/L7 instead.)
LOGOS_TEST(outbound_recover_self_heals_settling_without_spend) {
    std::string dir = inboxDir("recover_selfheal");
    {
        PilotImpl impl; impl.initialize(dir);
        execSql(dir,
            "INSERT OR REPLACE INTO outbound_tasks "
            "(id, agent_address, skill, price, reply_topic, state, payout, spend_request_id, created_at, updated_at) "
            "VALUES ('obSH','doerAgent','storage-upload',5,'/r','settling','','','0','0');");
    }
    PilotImpl impl2; impl2.initialize(dir);   // initialize() -> outboundTasksRecover() step (0)

    LOGOS_ASSERT_EQ(outboundCol(dir, "obSH", "state"), std::string("submitted"));   // re-armed
    LOGOS_ASSERT_TRUE(outboundCol(dir, "obSH", "spend_request_id").empty());        // still unlinked
    LOGOS_ASSERT_EQ(countRows(dir, "SELECT COUNT(*) FROM spend_requests;"), 0);     // no money moved
}

// L8 (inbound wallet-send txn): the create + link + input-required + HELD writes land together as
// one committed unit, so a peer's wallet-send is never observed half-applied.
LOGOS_TEST(inbound_wallet_send_create_link_hold_are_atomic) {
    std::string dir = inboxDir("walletsend_atomic");
    PilotImpl impl; impl.initialize(dir);
    std::string r = impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-atom\",\"params\":{"
        "\"id\":\"t-atom\",\"metadata\":{\"skill\":\"wallet-send\"},"
        "\"message\":{\"recipient\":\"payeeAtom\",\"amount\":40,\"reason\":\"job\"}},"
        "\"_logos\":{\"sender_npk\":\"peer\",\"reply_topic\":\"/r\"}}", "", false);

    LOGOS_ASSERT_CONTAINS(r, "input-required");
    LOGOS_ASSERT_EQ(taskCol(dir, "t-atom", "state"), std::string("input-required"));   // input-required
    std::string sid = taskCol(dir, "t-atom", "spend_request_id");
    LOGOS_ASSERT_FALSE(sid.empty());                                                   // linked
    LOGOS_ASSERT_EQ(spendCountForRecipient(dir, "payeeAtom"), 1);                      // created (one)
    LOGOS_ASSERT_EQ(spendStateById(dir, sid), std::string("HELD"));                    // HELD
}

// ===================== Wave 3: I2 capabilities returns card without broadcast ========

// I2: an inbound 'capabilities' read returns the FULL signed Agent Card via buildCard() — the peer
// still gets the complete card; only the discovery-topic broadcast (which lives solely in
// agentCard()) is dropped. We seed an identity so buildCard() emits a real card.
LOGOS_TEST(inbound_capabilities_returns_card_without_broadcast) {
    std::string dir = inboxDir("capabilities_card");
    { PilotImpl boot; boot.initialize(dir); }   // create schema (no wallet -> returns false)
    execSql(dir, "INSERT OR REPLACE INTO agent_identity (id,npk,account_id,created_at) "
                 "VALUES (1,'agentnpk','acct','0');");
    ECIESKeypair kp = generateECIESKeypair();
    execSql(dir, "INSERT OR REPLACE INTO config (key,value) VALUES ('ecies.pub','" + kp.publicKeyHex + "');");
    execSql(dir, "INSERT OR REPLACE INTO config (key,value) VALUES ('ecies.priv','" + kp.privateKeyHex + "');");
    PilotImpl impl; impl.initialize(dir);        // loadIdentity() restores npk + ECIES key

    std::string r = impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-cap\",\"params\":{"
        "\"id\":\"t-cap\",\"metadata\":{\"skill\":\"capabilities\"},\"message\":{}},"
        "\"_logos\":{\"sender_npk\":\"peer\",\"reply_topic\":\"/r\"}}", "", false);

    LOGOS_ASSERT_CONTAINS(r, "completed");
    LOGOS_ASSERT_EQ(taskCol(dir, "t-cap", "state"), std::string("completed"));
    std::string card = taskCol(dir, "t-cap", "result_json");
    LOGOS_ASSERT_CONTAINS(card, "agentnpk");                          // full card, real identity
    LOGOS_ASSERT_CONTAINS(card, "signature");                        // signed
    LOGOS_ASSERT_TRUE(card.find("not initialized") == std::string::npos);
}

// I2: the empty-npk not-initialized stub is preserved on the inbound capabilities leg — buildCard()
// returns the same error stub the old agentCard() did before an identity exists.
LOGOS_TEST(inbound_capabilities_uninitialized_returns_error_not_card) {
    std::string dir = inboxDir("capabilities_uninit");
    PilotImpl impl; impl.initialize(dir);   // no identity seeded -> agentNpk_ stays empty
    std::string r = impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-capx\",\"params\":{"
        "\"id\":\"t-capx\",\"metadata\":{\"skill\":\"capabilities\"},\"message\":{}},"
        "\"_logos\":{\"sender_npk\":\"peer\",\"reply_topic\":\"/r\"}}", "", false);

    std::string card = taskCol(dir, "t-capx", "result_json");
    LOGOS_ASSERT_CONTAINS(card, "not initialized");                  // stub preserved
    LOGOS_ASSERT_TRUE(card.find("signature") == std::string::npos);  // no real card emitted
}

// ===================== Wave 3: L6 in-flight LLM bound (no nested event loop) =========

// L6: an inbound agent-ask FLOOD can never nest QEventLoops on the single delivery thread. The
// injected LLM re-enters with a SECOND agent-ask DURING the first's complete(); the concurrency
// bound refuses the nested one ('failed' + busy) before it can nest another blocking call, while
// the outer agent-ask still completes.
LOGOS_TEST(inbound_agent_ask_concurrency_bounded_no_nested_eventloop) {
    std::string dir = inboxDir("agentask_concurrency");
    PilotImpl impl; impl.initialize(dir);
    pilotSetLLMProvider(impl, std::make_unique<ReentrantAskLLM>(&impl));

    std::string r = impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-ask-outer\",\"params\":{"
        "\"id\":\"t-ask-outer\",\"metadata\":{\"skill\":\"agent-ask\"},"
        "\"message\":{\"prompt\":\"outer\"}},"
        "\"_logos\":{\"sender_npk\":\"peer\",\"reply_topic\":\"/r\"}}", "", false);

    // The OUTER agent-ask owns the single in-flight slot and completes.
    LOGOS_ASSERT_CONTAINS(r, "completed");
    LOGOS_ASSERT_EQ(taskCol(dir, "t-ask-outer", "state"), std::string("completed"));
    // The NESTED agent-ask (fired from inside the outer's blocking complete()) was refused by the
    // bound BEFORE nesting a second event loop: it failed with a 'busy' error.
    LOGOS_ASSERT_EQ(taskCol(dir, "t-ask-nested", "state"), std::string("failed"));
    LOGOS_ASSERT_CONTAINS(taskCol(dir, "t-ask-nested", "result_json"), "busy");
}

// ===================== Wave 2 hardening: L1 key separation (enc vs signing) ===========

// L1: the Agent Card advertises an INDEPENDENT encryption key (_logos.enc_key) alongside the
// unchanged signing identity (_logos.signing_key). The inbox topic + url are keyed on the enc
// key, not the signing key, and the card still verifies 'valid' (the enc_key is signed over).
LOGOS_TEST(card_advertises_independent_enc_key) {
    std::string dir = inboxDir("card_enc");
    ECIESKeypair kp = generateECIESKeypair();
    { PilotImpl boot; boot.initialize(dir); }   // create schema (no wallet -> returns false)
    seedEciesIdentity(dir, kp);
    PilotImpl impl; impl.initialize(dir);        // loadIdentity restores npk + ECIES key, backfills enc

    QJsonObject card = QJsonDocument::fromJson(QByteArray::fromStdString(impl.agentCard())).object();
    QJsonObject logos = card["_logos"].toObject();
    std::string signingKey = logos["signing_key"].toString().toStdString();
    std::string encKey = logos["enc_key"].toString().toStdString();

    LOGOS_ASSERT_EQ(signingKey, kp.publicKeyHex);             // signing identity is UNCHANGED (TOFU survives)
    LOGOS_ASSERT_FALSE(encKey.empty());                       // enc key is advertised
    LOGOS_ASSERT_TRUE(encKey != signingKey);                  // and is independent of the signing key

    std::string inbox = logos["inbox_topic"].toString().toStdString();
    LOGOS_ASSERT_CONTAINS(inbox, encKey);                     // inbox keyed on the enc key
    LOGOS_ASSERT_TRUE(inbox.find(signingKey) == std::string::npos);   // NOT the signing key
    LOGOS_ASSERT_CONTAINS(card["url"].toString().toStdString(), encKey);
    LOGOS_ASSERT_TRUE(verifyCardStatus(card) == QString("valid"));    // enc_key is signed over
}

// L1 backfill: a pre-split DB (ecies.* only) generates+persists a fresh enc keypair on load, and a
// SECOND boot on the same dir advertises the SAME enc key (never re-rotates). The signing identity
// stays the seeded ecies key throughout.
LOGOS_TEST(identity_load_backfills_enc_keypair) {
    std::string dir = inboxDir("enc_backfill");
    ECIESKeypair kp = generateECIESKeypair();
    { PilotImpl boot; boot.initialize(dir); }
    seedEciesIdentity(dir, kp);

    std::string encKeyCard1;
    {
        PilotImpl impl; impl.initialize(dir);   // first load -> backfill enc keypair
        QJsonObject logos = QJsonDocument::fromJson(
            QByteArray::fromStdString(impl.agentCard())).object()["_logos"].toObject();
        encKeyCard1 = logos["enc_key"].toString().toStdString();
        LOGOS_ASSERT_EQ(logos["signing_key"].toString().toStdString(), kp.publicKeyHex);
    }
    std::string encPub = configVal(dir, "enc.pub");
    LOGOS_ASSERT_FALSE(encPub.empty());                       // enc.pub persisted to config
    LOGOS_ASSERT_FALSE(configVal(dir, "enc.priv").empty());   // enc.priv persisted (plaintext, no passphrase)
    LOGOS_ASSERT_EQ(encKeyCard1, encPub);                     // the card advertised the backfilled key
    LOGOS_ASSERT_TRUE(encKeyCard1 != kp.publicKeyHex);        // independent of the signing key

    // A second boot advertises the SAME enc key (no re-rotation) and the SAME signing key.
    PilotImpl impl2; impl2.initialize(dir);
    QJsonObject logos2 = QJsonDocument::fromJson(
        QByteArray::fromStdString(impl2.agentCard())).object()["_logos"].toObject();
    LOGOS_ASSERT_EQ(logos2["enc_key"].toString().toStdString(), encPub);
    LOGOS_ASSERT_EQ(logos2["signing_key"].toString().toStdString(), kp.publicKeyHex);
}

// (L1 routing prefer/fallback unit tests were removed with the pilotTestA2ARoutingKey seam — the
// QRO generator wrapped that PilotImpl&-taking declaration and failed to build. Routing is covered
// by the enc-key roundtrip test below + the manual two-agent Docker test.)

// L1 decrypt: a task encrypted to the doer's advertised _logos.enc_key (independent of the signing
// key) is decrypted by a2aTryDecrypt (enc key first) and dispatched through the state machine —
// the new<->new pay-loop request leg.
LOGOS_TEST(inbound_roundtrip_via_enc_key_decrypts_and_dispatches) {
    std::string dir = inboxDir("enc_roundtrip");
    ECIESKeypair kp = generateECIESKeypair();
    { PilotImpl boot; boot.initialize(dir); }
    seedEciesIdentity(dir, kp);
    PilotImpl doer; doer.initialize(dir);        // loadIdentity backfills a fresh enc keypair

    // The asker reads the doer's advertised enc_key and encrypts the SIGNED task to THAT key.
    QJsonObject dlogos = QJsonDocument::fromJson(
        QByteArray::fromStdString(doer.agentCard())).object()["_logos"].toObject();
    std::string encKey = dlogos["enc_key"].toString().toStdString();
    LOGOS_ASSERT_FALSE(encKey.empty());
    LOGOS_ASSERT_TRUE(encKey != kp.publicKeyHex);   // enc key is NOT the signing key

    ECIESKeypair peer = generateECIESKeypair();
    QJsonObject metadata; metadata["skill"] = QString("ping");
    QJsonObject params;
    params["id"] = QString("t-enc");
    params["metadata"] = metadata;
    params["message"] = QJsonObject();
    QJsonObject logos;
    logos["sender_npk"] = QString("peerEnc");
    logos["sender_ecies"] = QString::fromStdString(peer.publicKeyHex);
    logos["reply_topic"] = QString("/pilot/1/reply-t-enc/proto");
    QJsonObject env;
    env["jsonrpc"] = QString("2.0");
    env["method"] = QString("tasks/send");
    env["id"] = QString("t-enc");
    env["params"] = params;
    env["_logos"] = logos;
    std::string req = signRequest(env, peer);
    std::vector<uint8_t> plain(req.begin(), req.end());
    std::string payload = eciesSerialize(eciesEncrypt(encKey, plain));   // encrypt to the ENC key

    doer.handleInboundA2A(payload);   // a2aTryDecrypt: enc key first -> verify -> dispatch

    LOGOS_ASSERT_EQ(taskCol(dir, "t-enc", "state"), std::string("completed"));
    LOGOS_ASSERT_EQ(taskCol(dir, "t-enc", "sender_ecies"), std::string(peer.publicKeyHex));
}

// L1 x M2: the backfilled enc.priv is sealed at rest through the SAME path as ecies.priv — both are
// wrapped (enc:v1) when PILOT_KEY_PASSPHRASE is set. The env is cleared BEFORE any assert so a
// later seeded-plaintext test in this single-process runner is never re-wrapped.
LOGOS_TEST(migration_backfilled_enc_priv_wrapped_when_passphrase_set) {
    std::string dir = inboxDir("enc_wrapped");
    ECIESKeypair kp = generateECIESKeypair();
    { PilotImpl boot; boot.initialize(dir); }
    seedEciesIdentity(dir, kp);                  // legacy plaintext ecies.* only

    setenv("PILOT_KEY_PASSPHRASE", "operator-secret", 1);
    { PilotImpl impl; impl.initialize(dir); }    // backfills enc keypair, seals enc.priv + ecies.priv
    std::string encPriv = configVal(dir, "enc.priv");
    std::string eciesPriv = configVal(dir, "ecies.priv");
    unsetenv("PILOT_KEY_PASSPHRASE");            // BEFORE any assert

    LOGOS_ASSERT_TRUE(isWrappedSecret(encPriv));     // enc.priv sealed at rest
    LOGOS_ASSERT_TRUE(isWrappedSecret(eciesPriv));   // ecies.priv migrated/sealed at rest
}

// L1 [FIX-A] regression: a WRAPPED-but-unreadable enc.priv (passphrase absent on a later boot) is
// NEVER regenerated. The backfill guard tests the RAW STORED slots, so the recoverable enc key
// survives and the advertised enc_key is unchanged — not a fresh random key.
LOGOS_TEST(migration_wrapped_enc_priv_not_rotated_without_passphrase) {
    std::string dir = inboxDir("enc_not_rotated");
    ECIESKeypair kp = generateECIESKeypair();
    { PilotImpl boot; boot.initialize(dir); }
    seedEciesIdentity(dir, kp);

    std::string encPubBefore;
    setenv("PILOT_KEY_PASSPHRASE", "operator-secret", 1);
    { PilotImpl impl; impl.initialize(dir); }    // writes WRAPPED enc.* (recoverable)
    encPubBefore = configVal(dir, "enc.pub");
    unsetenv("PILOT_KEY_PASSPHRASE");            // BEFORE any assert

    // Second boot with NO passphrase: enc.priv is wrapped+unreadable, but the RAW slots are present,
    // so the guard does NOT fire and the key is not rotated.
    PilotImpl impl2; impl2.initialize(dir);
    std::string cardEncKey = QJsonDocument::fromJson(
        QByteArray::fromStdString(impl2.agentCard())).object()["_logos"].toObject()
        ["enc_key"].toString().toStdString();
    std::string encPubAfter = configVal(dir, "enc.pub");

    LOGOS_ASSERT_FALSE(encPubBefore.empty());
    LOGOS_ASSERT_EQ(encPubAfter, encPubBefore);    // enc.pub NOT overwritten (key not regenerated)
    LOGOS_ASSERT_EQ(cardEncKey, encPubBefore);     // advertised enc_key is the original, not a new random key
}

// The card must state the payment model the requester side actually runs. settleOutboundReply
// pays ONLY on the doer's terminal 'completed' (accepted / working / input-required never
// settle, failed / canceled / rejected never pay), so a card advertising "on-acceptance" told
// a peer to expect money at a moment it would not arrive.
LOGOS_TEST(card_advertises_pay_on_completion) {
    std::string dir = inboxDir("card_timing");
    ECIESKeypair kp = generateECIESKeypair();
    { PilotImpl boot; boot.initialize(dir); }   // create schema (no wallet -> returns false)
    seedEciesIdentity(dir, kp);
    PilotImpl impl; impl.initialize(dir);

    QJsonObject logos = QJsonDocument::fromJson(QByteArray::fromStdString(impl.agentCard()))
                            .object()["_logos"].toObject();
    LOGOS_ASSERT_EQ(logos["payment_timing"].toString().toStdString(), std::string("on-completion"));
}
