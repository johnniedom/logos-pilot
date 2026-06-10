#include <logos_test.h>
#include "../src/pilot_impl.h"
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
