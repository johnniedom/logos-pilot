#include <logos_test.h>
#include "../src/pilot_impl.h"
#include "../src/pilot_spend_rail.h"
#include <sqlite3.h>
#include <string>
#include <cstdio>
#include <cstdlib>

// --- helpers -------------------------------------------------------------
// A fresh data dir per test so a leftover pilot.db can't carry stale rows in.
static std::string pilotTestDir(const std::string& name) {
    std::string base = "/tmp";
    if (const char* t = std::getenv("TMPDIR")) base = t;
    std::string dir = base + "/pilot_test_" + name;
    std::remove((dir + "/pilot.db").c_str());
    std::remove((dir + "/pilot.db-wal").c_str());
    std::remove((dir + "/pilot.db-shm").c_str());
    return dir;
}

// Force a spend request into a given state/expiry to simulate the passage of
// time (createSpendRequest always stamps a future deadline, so we age it here).
static void ageRequest(const std::string& dir, const std::string& id,
                       const std::string& state, const std::string& expiresAt) {
    sqlite3* db = nullptr;
    sqlite3_open((dir + "/pilot.db").c_str(), &db);
    std::string sql = "UPDATE spend_requests SET state='" + state +
        "', expires_at='" + expiresAt + "' WHERE id='" + id + "';";
    sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(db);
}

static std::string queryState(const std::string& dir, const std::string& id) {
    sqlite3* db = nullptr;
    sqlite3_open((dir + "/pilot.db").c_str(), &db);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "SELECT state FROM spend_requests WHERE id = ?;", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    std::string state;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        state = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return state;
}

// --- tests ---------------------------------------------------------------

// A request whose 60-minute approval window has passed must NOT still be
// presented to the owner as awaiting approval.
LOGOS_TEST(expired_request_not_listed_as_pending) {
    std::string dir = pilotTestDir("expiry_pending");
    PilotImpl impl;
    impl.initialize(dir);   // opens db_ (returns false without a wallet, which is fine here)

    std::string id = impl.createSpendRequest("recipient_npk", 250, "test job");
    LOGOS_ASSERT_FALSE(id.empty());

    ageRequest(dir, id, "NOTIFIED", "1000");   // long-expired, still "pending"

    std::string pending = impl.getPendingSpends();
    LOGOS_ASSERT_CONTAINS(pending, "\"pending\":[]");
    LOGOS_ASSERT_EQ(queryState(dir, id), std::string("EXPIRED"));
}

// ===================== Wave 3: L7 crash-stranded EXECUTING reconciliation ============

// A clean run always drives EXECUTING->terminal synchronously, so any spend still EXECUTING at
// startup was crash-stranded. reconcileExecutingSpends() moves it to the terminal TX_UNKNOWN, and
// a second pass is a no-op (idempotent; never double-surfaced).
LOGOS_TEST(executing_spend_reconciled_to_tx_unknown) {
    std::string dir = pilotTestDir("reconcile_exec");
    PilotImpl impl;
    impl.initialize(dir);

    std::string id = impl.createSpendRequest("recipient_npk", 25, "stranded job");
    LOGOS_ASSERT_FALSE(id.empty());
    ageRequest(dir, id, "EXECUTING", "9999999999");   // crash-stranded mid-transfer

    impl.reconcileExecutingSpends();
    LOGOS_ASSERT_EQ(queryState(dir, id), std::string("TX_UNKNOWN"));

    // Idempotent: the second pass finds no EXECUTING rows and leaves the terminal state alone.
    impl.reconcileExecutingSpends();
    LOGOS_ASSERT_EQ(queryState(dir, id), std::string("TX_UNKNOWN"));
}

// Schema/migration pin (S3): spend_requests carries a tx_hash column that DEFAULTs to '' for a
// freshly created request (createSpendRequest's explicit column list lets the DEFAULT apply).
LOGOS_TEST(spend_requests_have_tx_hash_column_defaulting_empty) {
    std::string dir = pilotTestDir("txhash_col");
    PilotImpl impl;
    impl.initialize(dir);

    std::string id = impl.createSpendRequest("recipient_npk", 10, "job");
    LOGOS_ASSERT_FALSE(id.empty());

    sqlite3* db = nullptr;
    sqlite3_open((dir + "/pilot.db").c_str(), &db);
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, "SELECT tx_hash FROM spend_requests WHERE id = ?;", -1, &stmt, nullptr);
    LOGOS_ASSERT_EQ(rc, SQLITE_OK);   // the column exists (schema + idempotent migration)
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    std::string txh = "SENTINEL";
    if (sqlite3_step(stmt) == SQLITE_ROW)
        txh = sqlite3_column_text(stmt, 0)
            ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)) : std::string();
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    LOGOS_ASSERT_EQ(txh, std::string(""));
}

// ===================== Public spend rail: the recipient's FORM picks the wallet call =========
// walletSend used to know only two forms (keys JSON -> transfer_private, bare id ->
// transfer_private_owned), both spending from the agent's PRIVATE account. On the public
// testnet that account cannot be filled without a client-side proof this class of box cannot
// finish, while the faucet money sits in a PUBLIC account the wallet owns. A "public:" prefix
// selects transfer_public from that account; the prefix is required because a public id and a
// private id are indistinguishable by shape.

LOGOS_TEST(spend_recipient_keys_json_selects_private_keys_rail) {
    std::string keys = "{\"nullifier_public_key\":\"ab\",\"viewing_public_key\":\"cd\"}";
    SpendTarget t = parseSpendRecipient(keys);
    LOGOS_ASSERT_EQ(static_cast<int>(t.rail), static_cast<int>(SpendRail::PrivateKeys));
    LOGOS_ASSERT_EQ(t.target, keys);
}

LOGOS_TEST(spend_recipient_bare_id_selects_private_owned_rail) {
    std::string id(64, 'a');
    SpendTarget t = parseSpendRecipient(id);
    LOGOS_ASSERT_EQ(static_cast<int>(t.rail), static_cast<int>(SpendRail::PrivateOwned));
    LOGOS_ASSERT_EQ(t.target, id);
}

LOGOS_TEST(spend_recipient_public_prefix_selects_public_rail_and_normalises_hex) {
    std::string hex = "F8FC394C0E5440C4188236D1693076B0CFAD04984CF67CA64E0E43A173144F63";
    SpendTarget t = parseSpendRecipient("public:" + hex);
    LOGOS_ASSERT_EQ(static_cast<int>(t.rail), static_cast<int>(SpendRail::Public));
    LOGOS_ASSERT_EQ(t.target,
        std::string("f8fc394c0e5440c4188236d1693076b0cfad04984cf67ca64e0e43a173144f63"));
}

LOGOS_TEST(spend_recipient_public_prefix_rejects_malformed_id) {
    // Too short, not hex, empty: none may reach the wallet as a transfer_public target.
    LOGOS_ASSERT_EQ(static_cast<int>(parseSpendRecipient("public:abc").rail),
                    static_cast<int>(SpendRail::Invalid));
    LOGOS_ASSERT_EQ(static_cast<int>(parseSpendRecipient("public:" + std::string(64, 'g')).rail),
                    static_cast<int>(SpendRail::Invalid));
    LOGOS_ASSERT_EQ(static_cast<int>(parseSpendRecipient("public:").rail),
                    static_cast<int>(SpendRail::Invalid));
}

// ===================== Say why a spend failed ===============================================
// A spend can fail before it reaches the wallet (module context not attached, recipient
// malformed) or inside it (the wallet's own error JSON). The reason used to vanish: the row
// went TX_FAILED and walletSend answered {"status":"failed"} with nothing to act on. It is
// persisted ON THE SPEND ROW rather than in a member so it survives a restart and is per
// request: an A2A payout and an owner spend can fail at the same time without one reason
// overwriting the other.

static std::string queryError(const std::string& dir, const std::string& id) {
    sqlite3* db = nullptr;
    sqlite3_open((dir + "/pilot.db").c_str(), &db);
    sqlite3_stmt* stmt = nullptr;
    std::string err = "<NO_ERROR_COLUMN>";
    if (sqlite3_prepare_v2(db, "SELECT error FROM spend_requests WHERE id = ?;",
                           -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            err = sqlite3_column_text(stmt, 0)
                ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)) : std::string();
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return err;
}

// Schema pin: a fresh request carries an empty error (column exists, DEFAULT '').
LOGOS_TEST(spend_requests_have_error_column_defaulting_empty) {
    std::string dir = pilotTestDir("error_col");
    PilotImpl impl;
    impl.initialize(dir);
    std::string id = impl.createSpendRequest("recipient_npk", 10, "job");
    LOGOS_ASSERT_FALSE(id.empty());
    LOGOS_ASSERT_EQ(queryError(dir, id), std::string(""));
}

// No wallet attached (exactly this unit-test situation, and a daemon whose lez_core host
// died): the row must say so, not just go TX_FAILED.
LOGOS_TEST(spend_without_wallet_records_why_it_failed) {
    std::string dir = pilotTestDir("error_no_wallet");
    PilotImpl impl;
    impl.initialize(dir);
    std::string id = impl.createSpendRequest(std::string(64, 'a'), 10, "job");
    LOGOS_ASSERT_FALSE(impl.executeSpend(id));
    LOGOS_ASSERT_EQ(queryState(dir, id), std::string("TX_FAILED"));
    LOGOS_ASSERT_EQ(queryError(dir, id), std::string("wallet module unavailable"));
}

// A malformed public recipient is rejected on its FORM, before the wallet is consulted: the
// reason names the recipient, not the wallet, so the same request would fail identically
// with a wallet attached, and no transfer_public is ever attempted with a bad target.
LOGOS_TEST(spend_to_malformed_public_recipient_fails_on_form_not_wallet) {
    std::string dir = pilotTestDir("error_bad_public");
    PilotImpl impl;
    impl.initialize(dir);
    std::string id = impl.createSpendRequest("public:abc", 10, "job");
    LOGOS_ASSERT_FALSE(impl.executeSpend(id));
    LOGOS_ASSERT_EQ(queryState(dir, id), std::string("TX_FAILED"));
    LOGOS_ASSERT_CONTAINS(queryError(dir, id), "malformed public recipient");
}
