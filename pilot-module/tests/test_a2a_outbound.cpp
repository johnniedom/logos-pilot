#include <logos_test.h>
#include "../src/pilot_impl.h"
#include "../src/pilot_a2a.h"
#include "../src/pilot_crypto.h"
#include <sqlite3.h>
#include <string>
#include <vector>
#include <set>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <QString>
#include <QJsonObject>
#include <QJsonDocument>
#include <QByteArray>

// Requester-side pay-on-acceptance loop (ASKER PAYS THE DOER). settleOutboundReply is the
// transport-free FSM seam — handleA2AReply decrypts a peer's reply with agentEciesPriv_
// then calls this; these tests drive it directly against real on-disk SQLite.
//
// HONEST HARNESS NOTE: a priced, payable reply runs the spending FSM directly
// (createSpendRequest -> executeSpend): it CREATES one spend_request targeting the doer's
// declared payout, then — with no wallet wired in this harness — the transfer honestly
// fails (TX_FAILED) so the task resolves to 'pay-failed'. The live two-agent run exercises
// the 'paid' (below threshold) and 'awaiting-approval' (above threshold) terminals and the
// ECIES reply leg end to end. What IS proven here without a wallet: the settle TRIGGER
// (B2 — 'completed' now fires), the payout TARGET (M5 — the one spend targets the card
// payout, never the messaging address; refuses to spend at all with no payout),
// no-pay-on-failed, and settle-AT-MOST-ONCE idempotency (the duplicate reply opens no
// second spend).

static std::string outDir(const std::string& name) {
    std::string base = "/tmp";
    if (const char* t = std::getenv("TMPDIR")) base = t;
    std::string dir = base + "/pilot_outbound_" + name;
    std::remove((dir + "/pilot.db").c_str());
    std::remove((dir + "/pilot.db-wal").c_str());
    std::remove((dir + "/pilot.db-shm").c_str());
    return dir;
}

static sqlite3* openDb(const std::string& dir) {
    sqlite3* db = nullptr;
    sqlite3_open((dir + "/pilot.db").c_str(), &db);
    return db;
}

// Insert a pending outbound task exactly as agentTask would (state 'submitted').
static void seedOutbound(const std::string& dir, const std::string& id,
                         const std::string& agentAddress, const std::string& skill,
                         int64_t price, const std::string& state = "submitted") {
    sqlite3* db = openDb(dir);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO outbound_tasks "
        "(id, agent_address, skill, price, reply_topic, state, payout, created_at, updated_at) "
        "VALUES (?, ?, ?, ?, '/r', ?, '', '1', '1');", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, agentAddress.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, skill.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, price);
    sqlite3_bind_text(st, 5, state.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    sqlite3_close(db);
}

// Store a card_json string in discovered_agents keyed on `npk`.
static void storeCard(const std::string& dir, const std::string& npk, const std::string& cardStr) {
    sqlite3* db = openDb(dir);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO discovered_agents (npk, card_json, topic, last_seen) "
        "VALUES (?, ?, '/pilot/1/discovery/proto', '1');", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, npk.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, cardStr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    sqlite3_close(db);
}

// Build + store a SIGNED, identity-bound Agent Card (verifyCardStatus -> 'valid') whose
// _logos.npk is `npk` and _logos.payout is `payout`, signed by `kp` published as
// _logos.signing_key. Mirrors PilotImpl::agentCard() EXACTLY: the signature is over the
// canonical compact bytes of the card WITHOUT the signature field, so the requester's
// verifyCardStatus reproduces the same bytes and the TOFU pin binds npk -> kp on first
// contact. Returns the keypair so a caller can assert against / diverge from it.
static ECIESKeypair seedCardKp(const std::string& dir, const std::string& npk,
                               const std::string& payout, const ECIESKeypair& kp) {
    QJsonObject pricing; pricing["storage-upload"] = 10;
    QJsonObject logos;
    logos["npk"] = QString::fromStdString(npk);
    logos["payout"] = QString::fromStdString(payout);
    logos["signing_key"] = QString::fromStdString(kp.publicKeyHex);
    logos["pricing"] = pricing;
    QJsonObject card;
    card["name"] = QString("Peer");
    card["_logos"] = logos;

    std::string canonical = QJsonDocument(card).toJson(QJsonDocument::Compact).toStdString();
    std::vector<uint8_t> bytes(canonical.begin(), canonical.end());
    QJsonObject sig;
    sig["alg"] = QString("ES256K");
    sig["publicKey"] = QString::fromStdString(kp.publicKeyHex);
    sig["value"] = QString::fromStdString(signMessage(bytes, kp.privateKeyHex));
    card["signature"] = sig;

    storeCard(dir, npk, QJsonDocument(card).toJson(QJsonDocument::Compact).toStdString());
    return kp;
}

// Convenience: seed a valid signed card under a FRESH key (first-contact pins it).
static void seedCard(const std::string& dir, const std::string& npk, const std::string& payout) {
    seedCardKp(dir, npk, payout, generateECIESKeypair());
}

// Seed an UNSIGNED card: verifyCardStatus -> 'unsigned', so it must NOT drive a payout.
static void seedUnsignedCard(const std::string& dir, const std::string& npk, const std::string& payout) {
    std::string card = "{\"name\":\"Peer\",\"_logos\":{\"npk\":\"" + npk +
        "\",\"payout\":\"" + payout + "\",\"pricing\":{\"storage-upload\":10}}}";
    storeCard(dir, npk, card);
}

// Pin npk -> signingKey directly (simulates a genuine first contact already on record), so
// a later card presenting that npk under a different key verifies 'invalid'.
static void pinIdentity(const std::string& dir, const std::string& npk, const std::string& signingKey) {
    sqlite3* db = openDb(dir);
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS pinned_identities "
        "(npk TEXT PRIMARY KEY, signing_key TEXT NOT NULL, first_seen TEXT NOT NULL);",
        nullptr, nullptr, nullptr);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO pinned_identities (npk, signing_key, first_seen) VALUES (?, ?, '1');",
        -1, &st, nullptr);
    sqlite3_bind_text(st, 1, npk.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, signingKey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    sqlite3_close(db);
}

// Build a doer's A2A reply envelope for `taskId`/`state`, signed EXACTLY as
// PilotImpl::replyToPeer signs it (FIX 1): _logos.signing_key is set, the canonical compact
// bytes (the envelope WITHOUT _logos.signature) are signed with kp's PRIVATE key, and the
// resulting _logos.signature is attached. With sign=false the reply carries NO signature —
// a forgeable, unauthenticated reply any observer could craft. The asker's verifyAndSettleReply
// reproduces these canonical bytes by removing _logos.signature and verifies the signature
// against the doer's PINNED signing_key (resolved from its discovered card), never the
// reply-supplied key.
static std::string makeSignedReply(const std::string& taskId, const std::string& state,
                                   const ECIESKeypair& kp, bool sign) {
    QJsonObject status; status["state"] = QString::fromStdString(state);
    QJsonObject task; task["id"] = QString::fromStdString(taskId); task["status"] = status;
    QJsonObject env;
    env["jsonrpc"] = QString("2.0");
    env["id"] = QString::fromStdString(taskId);
    env["result"] = task;   // result.status.state path (as a tasks/send reply carries it)
    if (sign) {
        QJsonObject logos;
        logos["signing_key"] = QString::fromStdString(kp.publicKeyHex);
        env["_logos"] = logos;
        std::string canonical = QJsonDocument(env).toJson(QJsonDocument::Compact).toStdString();
        std::vector<uint8_t> bytes(canonical.begin(), canonical.end());
        logos["signature"] = QString::fromStdString(signMessage(bytes, kp.privateKeyHex));
        env["_logos"] = logos;
    }
    return QJsonDocument(env).toJson(QJsonDocument::Compact).toStdString();
}

static std::string outCol(const std::string& dir, const std::string& id, const char* col) {
    sqlite3* db = openDb(dir);
    std::string sql = std::string("SELECT ") + col + " FROM outbound_tasks WHERE id=?;";
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

static int spendCount(const std::string& dir) {
    sqlite3* db = openDb(dir);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM spend_requests;", -1, &st, nullptr);
    int n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return n;
}

static int spendCountForRecipient(const std::string& dir, const std::string& recipient) {
    sqlite3* db = openDb(dir);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM spend_requests WHERE recipient=?;", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, recipient.c_str(), -1, SQLITE_TRANSIENT);
    int n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return n;
}

static void execOut(const std::string& dir, const std::string& sql) {
    sqlite3* db = openDb(dir);
    sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(db);
}

static std::string spendState(const std::string& dir, const std::string& id) {
    sqlite3* db = openDb(dir);
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

// B2 + M5: a 'completed' reply (the doer's terminal SUCCESS for a service skill) now FIRES
// the settle path, and the price is targeted at the card's declared payout — NEVER the
// messaging address the task was routed to.
LOGOS_TEST(settle_completed_targets_payout_not_messaging_address) {
    std::string dir = outDir("payout_target");
    PilotImpl impl; impl.initialize(dir);
    seedOutbound(dir, "t1", "PEER_VK", "storage-upload", 5);
    // H1: the payout account is bound to the card identity (payout == npk == PEER_VK).
    ECIESKeypair kp = seedCardKp(dir, "PEER_VK", "PEER_VK", generateECIESKeypair());

    impl.settleOutboundReply("t1", "completed");

    // B2: the settle path fired (the row left 'submitted'); previously 'completed' was
    // ignored and the loop never paid.
    LOGOS_ASSERT_TRUE(outCol(dir, "t1", "state") != std::string("submitted"));
    // M5/H1: the resolved payout target is the card's declared, identity-bound payout account...
    LOGOS_ASSERT_EQ(outCol(dir, "t1", "payout"), std::string("PEER_VK"));
    // ...and never the card's signing/messaging ECIES key.
    LOGOS_ASSERT_EQ(spendCountForRecipient(dir, kp.publicKeyHex), 0);
    // Honest wallet-less terminal: pay-failed AFTER targeting the payout. Live + funded
    // wallet -> 'paid' (two-agent run).
    LOGOS_ASSERT_EQ(outCol(dir, "t1", "state"), std::string("pay-failed"));
}

// A 'failed' reply pays nothing: no settling claim, no payout target, no spend request.
LOGOS_TEST(settle_failed_reply_pays_nothing) {
    std::string dir = outDir("failed_nopay");
    PilotImpl impl; impl.initialize(dir);
    seedOutbound(dir, "t2", "PEER_VK", "storage-upload", 5);
    seedCard(dir, "PEER_VK", "PAYOUT_ACCT");

    impl.settleOutboundReply("t2", "failed");

    LOGOS_ASSERT_EQ(outCol(dir, "t2", "state"), std::string("failed"));
    LOGOS_ASSERT_EQ(outCol(dir, "t2", "payout"), std::string(""));
    LOGOS_ASSERT_EQ(spendCount(dir), 0);
}

// M5 honest refusal: a payable reply with NO payout on the discovered card must mark the
// task 'pay-failed' and transfer NOTHING — it must not fall back to the messaging address.
LOGOS_TEST(settle_no_payout_card_marks_pay_failed_no_transfer) {
    std::string dir = outDir("no_payout");
    PilotImpl impl; impl.initialize(dir);
    seedOutbound(dir, "t3", "PEER_VK", "storage-upload", 5);
    // No discovered card -> no payout on file.

    impl.settleOutboundReply("t3", "completed");

    LOGOS_ASSERT_EQ(outCol(dir, "t3", "state"), std::string("pay-failed"));
    LOGOS_ASSERT_EQ(outCol(dir, "t3", "payout"), std::string(""));
    LOGOS_ASSERT_EQ(spendCount(dir), 0);
    LOGOS_ASSERT_EQ(spendCountForRecipient(dir, "PEER_VK"), 0);
}

// No declared price -> nothing to settle (we never fabricate a price): 'accepted-nopay'.
LOGOS_TEST(settle_zero_price_is_accepted_nopay) {
    std::string dir = outDir("zero_price");
    PilotImpl impl; impl.initialize(dir);
    seedOutbound(dir, "t4", "PEER_VK", "ping", 0);
    seedCard(dir, "PEER_VK", "PAYOUT_ACCT");

    impl.settleOutboundReply("t4", "completed");

    LOGOS_ASSERT_EQ(outCol(dir, "t4", "state"), std::string("accepted-nopay"));
    LOGOS_ASSERT_EQ(spendCount(dir), 0);
}

// Settle AT MOST ONCE: a task already settled to a terminal 'paid' (as a live wallet
// would leave it) is NEVER re-claimed, no matter how many duplicate/late replies arrive —
// the atomic submitted->settling claim only fires from 'submitted'. A later 'failed'
// likewise cannot undo a paid task.
LOGOS_TEST(settle_after_paid_never_pays_again) {
    std::string dir = outDir("already_paid");
    PilotImpl impl; impl.initialize(dir);
    seedOutbound(dir, "t5", "PEER_VK", "storage-upload", 5, "paid");
    seedCard(dir, "PEER_VK", "PAYOUT_ACCT");

    impl.settleOutboundReply("t5", "completed");   // duplicate success reply
    LOGOS_ASSERT_EQ(outCol(dir, "t5", "state"), std::string("paid"));
    LOGOS_ASSERT_EQ(spendCount(dir), 0);

    impl.settleOutboundReply("t5", "failed");      // contradictory late reply
    LOGOS_ASSERT_EQ(outCol(dir, "t5", "state"), std::string("paid"));
    LOGOS_ASSERT_EQ(spendCount(dir), 0);
}

// Re-driving the same reply twice settles only once: the FIRST 'completed' opens exactly
// one spend; the second identical 'completed' finds the row already out of 'submitted' and
// is a no-op (no second spend attempt) — so the spend count stays at one.
LOGOS_TEST(settle_same_reply_twice_is_idempotent) {
    std::string dir = outDir("idempotent");
    PilotImpl impl; impl.initialize(dir);
    seedOutbound(dir, "t6", "PEER_VK", "storage-upload", 5);
    seedCard(dir, "PEER_VK", "PEER_VK");   // H1: payout bound to identity (payout == npk)

    impl.settleOutboundReply("t6", "completed");
    std::string state1 = outCol(dir, "t6", "state");
    std::string payout1 = outCol(dir, "t6", "payout");
    LOGOS_ASSERT_EQ(spendCount(dir), 1);   // first reply opened exactly one spend

    impl.settleOutboundReply("t6", "completed");
    LOGOS_ASSERT_EQ(outCol(dir, "t6", "state"), state1);
    LOGOS_ASSERT_EQ(outCol(dir, "t6", "payout"), payout1);
    LOGOS_ASSERT_EQ(spendCount(dir), 1);   // duplicate reply opened no second spend
}

// 'input-required' (the doer's owner-gated interim reply) pays nothing AND does not kill a
// still-settleable 'submitted' row — a later 'completed' must still claim and settle.
LOGOS_TEST(settle_input_required_does_not_pay_or_block) {
    std::string dir = outDir("input_then_done");
    PilotImpl impl; impl.initialize(dir);
    seedOutbound(dir, "t7", "PEER_VK", "storage-upload", 5);
    seedCard(dir, "PEER_VK", "PEER_VK");   // H1: payout == npk

    impl.settleOutboundReply("t7", "input-required");
    LOGOS_ASSERT_EQ(outCol(dir, "t7", "state"), std::string("submitted"));   // still settleable
    LOGOS_ASSERT_EQ(spendCount(dir), 0);

    impl.settleOutboundReply("t7", "completed");
    LOGOS_ASSERT_TRUE(outCol(dir, "t7", "state") != std::string("submitted"));
    LOGOS_ASSERT_EQ(outCol(dir, "t7", "payout"), std::string("PEER_VK"));
}

// FIX 2(a) TERMINAL-ONLY TRIGGER: a progress reply must NOT pay. 'accepted' and 'working'
// leave the row 'submitted' (no spend, no payout) so a LATER 'completed' can still claim;
// paying on 'accepted' would pay for work the doer has not finished (no-fakery).
LOGOS_TEST(settle_accepted_does_not_pay_then_completed_does) {
    std::string dir = outDir("accepted_then_done");
    PilotImpl impl; impl.initialize(dir);
    seedOutbound(dir, "t8", "PEER_VK", "storage-upload", 5);
    seedCard(dir, "PEER_VK", "PEER_VK");   // H1: payout == npk

    impl.settleOutboundReply("t8", "accepted");
    LOGOS_ASSERT_EQ(outCol(dir, "t8", "state"), std::string("submitted"));   // not settled
    LOGOS_ASSERT_EQ(outCol(dir, "t8", "payout"), std::string(""));
    LOGOS_ASSERT_EQ(spendCount(dir), 0);                                     // nothing paid

    impl.settleOutboundReply("t8", "working");
    LOGOS_ASSERT_EQ(outCol(dir, "t8", "state"), std::string("submitted"));   // still not settled
    LOGOS_ASSERT_EQ(spendCount(dir), 0);

    impl.settleOutboundReply("t8", "completed");                            // terminal success -> settle
    LOGOS_ASSERT_TRUE(outCol(dir, "t8", "state") != std::string("submitted"));
    LOGOS_ASSERT_EQ(outCol(dir, "t8", "payout"), std::string("PEER_VK"));
    LOGOS_ASSERT_EQ(spendCount(dir), 1);                                     // exactly one spend opened
}

// FIX 2(b) AUTHENTICATED PAYOUT: a 'completed' reply matched only by an UNSIGNED card pays
// NOTHING. The card is not authentic, so we mark 'pay-failed' and open no spend (mirrors
// the no-payout refusal) — an attacker cannot redirect a payout with an unsigned card.
LOGOS_TEST(settle_unsigned_card_refuses_to_pay) {
    std::string dir = outDir("unsigned_card");
    PilotImpl impl; impl.initialize(dir);
    seedOutbound(dir, "t9", "PEER_VK", "storage-upload", 5);
    seedUnsignedCard(dir, "PEER_VK", "PAYOUT_ACCT");

    impl.settleOutboundReply("t9", "completed");

    LOGOS_ASSERT_EQ(outCol(dir, "t9", "state"), std::string("pay-failed"));
    LOGOS_ASSERT_EQ(outCol(dir, "t9", "payout"), std::string(""));
    LOGOS_ASSERT_EQ(spendCount(dir), 0);
    LOGOS_ASSERT_EQ(spendCountForRecipient(dir, "PAYOUT_ACCT"), 0);
}

// FIX 2(c) UNPINNED-CHANGED CARD: an attacker publishes a self-signed card (internally
// 'valid') under the victim npk with ITS OWN signing_key + payout, but the npk was PINNED to
// the genuine key on first contact. verifyCardStatus rejects the swapped key as 'invalid',
// so the pay path opens no spend and never pays the attacker's payout.
LOGOS_TEST(settle_swapped_signing_key_card_refuses_to_pay) {
    std::string dir = outDir("swapped_key");
    PilotImpl impl; impl.initialize(dir);
    seedOutbound(dir, "t10", "PEER_VK", "storage-upload", 5);
    // Attacker's self-signed card under the victim npk, naming the attacker's payout.
    seedCardKp(dir, "PEER_VK", "ATTACKER_PAYOUT", generateECIESKeypair());
    // But PEER_VK was pinned to the GENUINE key (a different key) on first contact.
    pinIdentity(dir, "PEER_VK", generateECIESKeypair().publicKeyHex);

    impl.settleOutboundReply("t10", "completed");

    LOGOS_ASSERT_EQ(outCol(dir, "t10", "state"), std::string("pay-failed"));
    LOGOS_ASSERT_EQ(outCol(dir, "t10", "payout"), std::string(""));
    LOGOS_ASSERT_EQ(spendCount(dir), 0);
    LOGOS_ASSERT_EQ(spendCountForRecipient(dir, "ATTACKER_PAYOUT"), 0);
}

// FIX 4(a) SINGLE-SOURCE PRICING: the prices agentCard() advertises (_logos.pricing) are
// EXACTLY the serviced (non-free) skills in a2aServiceCatalog() — no priced-but-unsupported
// skill, no serviced-but-accidentally-free skill, no drift between the two tables that used
// to diverge. program-call/program-deploy are unsupported over A2A, so they must never be
// priced. We seed a minimal identity so agentCard() emits a full card (it bails when the npk
// is empty), then compare the published price KEYS to the catalog's non-free id set.
LOGOS_TEST(agent_card_pricing_matches_serviced_skill_set) {
    std::string dir = outDir("pricing_single_source");
    { PilotImpl boot; boot.initialize(dir); }   // create schema (no wallet -> returns false)
    execOut(dir, "INSERT OR REPLACE INTO agent_identity (id,npk,account_id,created_at) "
                 "VALUES (1,'agentnpk','acct','0');");
    ECIESKeypair kp = generateECIESKeypair();
    execOut(dir, "INSERT OR REPLACE INTO config (key,value) VALUES ('ecies.pub','" + kp.publicKeyHex + "');");
    execOut(dir, "INSERT OR REPLACE INTO config (key,value) VALUES ('ecies.priv','" + kp.privateKeyHex + "');");
    PilotImpl impl; impl.initialize(dir);        // loadIdentity() restores npk + ECIES key

    std::string cardStr = impl.agentCard();
    QJsonObject card = QJsonDocument::fromJson(QByteArray::fromStdString(cardStr)).object();
    QJsonObject pricing = card["_logos"].toObject()["pricing"].toObject();

    // Expected advertised set = catalog entries with a non-zero (non-free) price.
    std::set<std::string> expected;
    for (const auto& svc : a2aServiceCatalog())
        if (svc.price > 0) expected.insert(svc.id);

    std::set<std::string> advertised;
    for (auto it = pricing.begin(); it != pricing.end(); ++it)
        advertised.insert(it.key().toStdString());

    // The advertised price set is EXACTLY the serviced non-free skill set.
    LOGOS_ASSERT_TRUE(advertised == expected);
    // ...and the catalog is non-trivial (guards against an empty-vs-empty false pass).
    LOGOS_ASSERT_GT(static_cast<int>(expected.size()), 0);

    // Every advertised price equals the catalog price (one source of truth, no drift).
    for (const auto& svc : a2aServiceCatalog())
        if (svc.price > 0)
            LOGOS_ASSERT_EQ(static_cast<int64_t>(pricing[QString::fromUtf8(svc.id)].toDouble()), svc.price);

    // Unsupported skills are never priced.
    LOGOS_ASSERT_TRUE(!pricing.contains("program-call"));
    LOGOS_ASSERT_TRUE(!pricing.contains("program-deploy"));
}

// FIX 4(b) CONTRADICTORY TERMINAL RETRACTS A HELD PAYMENT: a peer that first replies
// 'completed' (parking an above-threshold payment in 'awaiting-approval' with the spend
// HELD for the owner) and THEN replies 'failed' must retract the pending payment. The held
// spend is rejected (REJECTED) and the outbound task moves to 'pay-failed' — never left
// dangling in 'awaiting-approval' against a spend the owner might still approve.
LOGOS_TEST(settle_failed_after_awaiting_approval_cancels_held_spend) {
    std::string dir = outDir("contradict_after_park");
    PilotImpl impl; impl.initialize(dir);
    impl.setSpendingLimits(10, 500, 86400);    // price 40 > per-tx 10 -> owner gate
    seedOutbound(dir, "tc", "PEER_VK", "storage-upload", 40);
    ECIESKeypair kp = seedCardKp(dir, "PEER_VK", "PEER_VK", generateECIESKeypair());   // H1: payout == npk

    impl.settleOutboundReply("tc", "completed");   // parks awaiting-approval, spend HELD
    LOGOS_ASSERT_EQ(outCol(dir, "tc", "state"), std::string("awaiting-approval"));
    std::string sid = outCol(dir, "tc", "spend_request_id");
    LOGOS_ASSERT_FALSE(sid.empty());
    LOGOS_ASSERT_EQ(spendState(dir, sid), std::string("HELD"));   // parked for the owner

    impl.settleOutboundReply("tc", "failed");      // contradictory terminal retracts payment
    LOGOS_ASSERT_EQ(outCol(dir, "tc", "state"), std::string("pay-failed"));   // no orphan
    LOGOS_ASSERT_EQ(spendState(dir, sid), std::string("REJECTED"));           // held spend canceled
    // The retracted payment never reached execution. Under H1 (payout == npk == PEER_VK) the
    // REJECTED row targets PEER_VK, so we assert against the card's signing key — which is NEVER
    // a payout recipient — to prove no transfer was ever opened to the doer's key.
    LOGOS_ASSERT_EQ(spendCountForRecipient(dir, kp.publicKeyHex), 0);
}

// FIX 1 (the BLOCKER) GENUINE REPLY SETTLES: a 'completed' reply SIGNED by the doer's pinned
// signing_key authenticates, so the asker settles it — the row leaves 'submitted', the spend
// targets the card payout, and exactly one spend opens. (Wallet-less harness -> 'pay-failed'
// after targeting; a live wallet -> 'paid'.) This is the legitimate path the forgery guard
// must NOT break.
LOGOS_TEST(verify_genuine_signed_reply_settles) {
    std::string dir = outDir("signed_genuine");
    PilotImpl impl; impl.initialize(dir);
    seedOutbound(dir, "ts1", "PEER_VK", "storage-upload", 5);
    // seedCardKp pins PEER_VK -> kp on first contact (verifyCardStatus inside matchedCardLogos),
    // so kp is the AUTHORITATIVE signing_key the reply is verified against.
    ECIESKeypair kp = seedCardKp(dir, "PEER_VK", "PEER_VK", generateECIESKeypair());   // H1: payout == npk

    impl.verifyAndSettleReply("ts1", makeSignedReply("ts1", "completed", kp, /*sign=*/true));

    LOGOS_ASSERT_TRUE(outCol(dir, "ts1", "state") != std::string("submitted"));   // settled
    LOGOS_ASSERT_EQ(outCol(dir, "ts1", "payout"), std::string("PEER_VK"));        // payout targeted (== npk)
    LOGOS_ASSERT_EQ(spendCount(dir), 1);                                          // exactly one spend
}

// FIX 1 FORGED (UNSIGNED) REPLY PAYS NOTHING: today's reply is only ECIES-encrypted to OUR
// public key, which anyone can do, so an UNSIGNED forged {"status":{"state":"completed"}}
// must NOT settle. With no _logos.signature the asker drops it: the row stays 'submitted', no
// payout is targeted, and no spend opens. (Same crafted 'completed' that pre-FIX-1 forced a
// payment.)
LOGOS_TEST(verify_unsigned_reply_does_not_settle) {
    std::string dir = outDir("unsigned_reply");
    PilotImpl impl; impl.initialize(dir);
    seedOutbound(dir, "ts2", "PEER_VK", "storage-upload", 5);
    seedCardKp(dir, "PEER_VK", "PAYOUT_ACCT", generateECIESKeypair());

    // Forged completion with NO signature — exactly what an eavesdropper could publish.
    impl.verifyAndSettleReply("ts2", makeSignedReply("ts2", "completed", generateECIESKeypair(), /*sign=*/false));

    LOGOS_ASSERT_EQ(outCol(dir, "ts2", "state"), std::string("submitted"));   // NOT settled
    LOGOS_ASSERT_EQ(outCol(dir, "ts2", "payout"), std::string(""));           // nothing targeted
    LOGOS_ASSERT_EQ(spendCount(dir), 0);                                      // nothing paid
}

// FIX 1 FORGED (WRONG-KEY) REPLY PAYS NOTHING: an attacker who knows the public reply topic
// and our public ECIES key crafts a 'completed' and SIGNS it with their OWN key (not the
// doer's pinned signing_key). The signature is internally well-formed but does NOT verify
// against the AUTHORITATIVE key bound to the task's agent_address, so the asker drops it: the
// row stays 'submitted', no payout, no spend. Only the genuine doer's key can authorize pay.
LOGOS_TEST(verify_wrong_key_signed_reply_does_not_settle) {
    std::string dir = outDir("wrongkey_reply");
    PilotImpl impl; impl.initialize(dir);
    seedOutbound(dir, "ts3", "PEER_VK", "storage-upload", 5);
    // The card pins PEER_VK to the GENUINE doer key (the authoritative signer).
    seedCardKp(dir, "PEER_VK", "PAYOUT_ACCT", generateECIESKeypair());

    // Attacker signs the forged completion with a DIFFERENT (non-pinned) key.
    ECIESKeypair attacker = generateECIESKeypair();
    impl.verifyAndSettleReply("ts3", makeSignedReply("ts3", "completed", attacker, /*sign=*/true));

    LOGOS_ASSERT_EQ(outCol(dir, "ts3", "state"), std::string("submitted"));   // forgery dropped
    LOGOS_ASSERT_EQ(outCol(dir, "ts3", "payout"), std::string(""));
    LOGOS_ASSERT_EQ(spendCount(dir), 0);
    LOGOS_ASSERT_EQ(spendCountForRecipient(dir, "PAYOUT_ACCT"), 0);
}

// H1 DIVERGENT-PAYOUT CARD REFUSES TO PAY: a card that is internally 'valid' AND pinned, but
// whose _logos.payout != _logos.npk, could redirect funds to a third account. discoveredPayoutFor
// now refuses such a card (returns ""), so a 'completed' reply settles to 'pay-failed' and opens
// NO spend — never the divergent payout. This is the dedicated negative test that pins the
// "a card cannot redirect payout" property in this wave.
LOGOS_TEST(settle_divergent_payout_card_refuses_to_pay) {
    std::string dir = outDir("divergent_payout");
    PilotImpl impl; impl.initialize(dir);
    seedOutbound(dir, "td", "PEER_VK", "storage-upload", 5);
    // Valid, first-contact-pinned card — but payout (ATTACKER_PAYOUT) is NOT bound to the card
    // identity (npk PEER_VK). H1 refuses to redirect funds to that unbound third account.
    seedCardKp(dir, "PEER_VK", "ATTACKER_PAYOUT", generateECIESKeypair());

    impl.settleOutboundReply("td", "completed");

    LOGOS_ASSERT_EQ(outCol(dir, "td", "state"), std::string("pay-failed"));
    LOGOS_ASSERT_EQ(outCol(dir, "td", "payout"), std::string(""));
    LOGOS_ASSERT_EQ(spendCount(dir), 0);
    LOGOS_ASSERT_EQ(spendCountForRecipient(dir, "ATTACKER_PAYOUT"), 0);
}

// ===================== Wave 3: L7 reconciliation drives the linked outbound =========

// L7: a spend crash-stranded in EXECUTING, linked to a 'settling' outbound task, is reconciled to
// TX_UNKNOWN and its hung outbound row is un-hung to 'pay-unresolved' (advanceLinkedOutboundTask
// only moves 'awaiting-approval'/'settling' rows, so this is the same monotonic driver the owner
// gate uses).
LOGOS_TEST(reconcile_executing_drives_linked_outbound_to_pay_unresolved) {
    std::string dir = outDir("reconcile_linked");
    PilotImpl impl; impl.initialize(dir);

    // A spend stranded mid-transfer (EXECUTING), and a 'settling' outbound task linked to it.
    std::string sid = impl.createSpendRequest("PAYEE", 5, "A2A pay");
    execOut(dir, "UPDATE spend_requests SET state='EXECUTING' WHERE id='" + sid + "';");
    seedOutbound(dir, "tr", "PEER_VK", "storage-upload", 5, "settling");
    execOut(dir, "UPDATE outbound_tasks SET spend_request_id='" + sid + "' WHERE id='tr';");

    impl.reconcileExecutingSpends();

    LOGOS_ASSERT_EQ(spendState(dir, sid), std::string("TX_UNKNOWN"));        // surfaced
    LOGOS_ASSERT_EQ(outCol(dir, "tr", "state"), std::string("pay-unresolved"));   // un-hung
}

// L8: a committed 'completed' settle ALWAYS carries a linked spend (claim+create+link land
// atomically), and exactly one spend exists — never a 'settling' row with an empty spend_request_id.
LOGOS_TEST(settle_completed_creates_and_links_spend_atomically) {
    std::string dir = outDir("settle_atomic");
    PilotImpl impl; impl.initialize(dir);
    seedOutbound(dir, "ta", "PEER_VK", "storage-upload", 5);
    seedCardKp(dir, "PEER_VK", "PEER_VK", generateECIESKeypair());   // H1: payout == npk

    impl.settleOutboundReply("ta", "completed");

    std::string sid = outCol(dir, "ta", "spend_request_id");
    LOGOS_ASSERT_FALSE(sid.empty());                       // the link is durable...
    LOGOS_ASSERT_EQ(spendCount(dir), 1);                   // ...and exactly one spend was created...
    LOGOS_ASSERT_FALSE(spendState(dir, sid).empty());      // ...and the linked spend row really exists
}

// ── routing: a bare address is not evidence of a messaging key ──────────────────
//
// With no card on file, a2aRoutingKeyFor used to return the address VERBATIM on the
// theory that the caller had passed an ECIES key directly. But a wallet viewing key
// is bare hex too, and test-two-agents-docker.sh passes exactly that. Routing to it
// encrypts the request to a key the peer cannot decrypt and publishes it to
// /pilot/1/inbox-<viewing key>/proto — a channel the peer never subscribes to. The
// send "succeeds", nothing arrives, and no error is ever raised: a silent dead-drop
// (observed 2026-07-26, and the reason a paid task could never settle).
//
// Only a card — discovered, or imported out-of-band — makes a peer routable.
LOGOS_TEST(routing_refuses_an_address_no_card_vouches_for) {
    std::string dir = outDir("routing_no_card");
    PilotImpl impl; impl.initialize(dir);
    sqlite3* db = openDb(dir);

    // A well-formed compressed secp256k1 public key, with no card on file. Shaped like a
    // key is not evidence of being the peer's key — a wallet viewing key looks identical.
    LOGOS_ASSERT_TRUE(a2aResolveRoutingKey(db,
        "02a36ce18bf4221d22f28ee9ee2d5c4e7e5161fabf0995b29eb5cee1ed3e98951d").empty());
    // A wallet npk blob was already refused; keep it refused.
    LOGOS_ASSERT_TRUE(a2aResolveRoutingKey(db,
        "{\"nullifier_public_key\":\"aa\",\"viewing_public_key\":\"bb\"}").empty());
    sqlite3_close(db);
}

// A card on file DOES make the peer routable — the fix must refuse the unvouched-for,
// not everything. seedCardKp publishes kp.publicKeyHex as the card's _logos.signing_key.
LOGOS_TEST(routing_resolves_the_key_from_a_card) {
    std::string dir = outDir("routing_with_card");
    PilotImpl impl; impl.initialize(dir);
    ECIESKeypair kp = seedCardKp(dir, "PEER_VK", "PEER_VK", generateECIESKeypair());
    sqlite3* db = openDb(dir);

    LOGOS_ASSERT_EQ(a2aResolveRoutingKey(db, "PEER_VK"), kp.publicKeyHex);
    sqlite3_close(db);
}
