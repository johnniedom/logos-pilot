#include <logos_test.h>
#include "../src/pilot_impl.h"
#include <sqlite3.h>
#include <string>
#include <cstdio>
#include <cstdlib>

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

LOGOS_TEST(inbound_costly_requires_owner_approval) {
    std::string dir = inboxDir("costly");
    PilotImpl impl; impl.initialize(dir);
    std::string r = impl.processInboundRequest(
        "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\",\"id\":\"t-pay\",\"params\":{"
        "\"id\":\"t-pay\",\"metadata\":{\"skill\":\"wallet-send\"},"
        "\"message\":{\"recipient\":\"deadbeef\",\"amount\":40,\"reason\":\"job\"}},"
        "\"_logos\":{\"sender_npk\":\"peer\",\"reply_topic\":\"/r\"}}");
    LOGOS_ASSERT_CONTAINS(r, "input-required");
    LOGOS_ASSERT_EQ(taskCol(dir, "t-pay", "state"), std::string("input-required"));
    LOGOS_ASSERT_FALSE(taskCol(dir, "t-pay", "spend_request_id").empty());
}

LOGOS_TEST(inbound_cancel_marks_task_canceled) {
    std::string dir = inboxDir("cancel");
    PilotImpl impl; impl.initialize(dir);
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
