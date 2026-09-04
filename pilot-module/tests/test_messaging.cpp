#include <logos_test.h>
#include "../src/pilot_impl.h"
#include "../src/pilot_crypto.h"
#include <sqlite3.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QByteArray>

// The RECEIVE side of messaging and storage sharing (2026-09-04).
//
// Until this file existed, the sending half was the only half: messagingSend and storageShare
// encrypted a payload to the peer's key and published it to the peer's inbox topic, where the
// peer decrypted it and DROPPED it, because handleInboundA2A only accepted signed JSON-RPC
// requests. The two-agent test asserted "sent" for both and never asked whether anything was
// received; nothing ever was. Groups had a create and a join but no way to send to one and no
// way to read one. These tests drive the same public entry points the transport uses
// (handleInboundA2A for the inbox, handleInboundMessage for a topic) and read back what the
// agent kept.

static std::string msgDir(const std::string& name) {
    std::string base = "/tmp";
    if (const char* t = std::getenv("TMPDIR")) base = t;
    std::string dir = base + "/pilot_msg_" + name;
    std::remove((dir + "/pilot.db").c_str());
    std::remove((dir + "/pilot.db-wal").c_str());
    std::remove((dir + "/pilot.db-shm").c_str());
    return dir;
}

static void execSql(const std::string& dir, const std::string& sql) {
    sqlite3* db = nullptr;
    sqlite3_open((dir + "/pilot.db").c_str(), &db);
    sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(db);
}

// One column of one row: SELECT <column> FROM <table> WHERE <whereCol> = <whereVal>.
static std::string col(const std::string& dir, const char* table, const char* column,
                       const char* whereCol, const std::string& whereVal) {
    sqlite3* db = nullptr;
    sqlite3_open((dir + "/pilot.db").c_str(), &db);
    std::string sql = std::string("SELECT ") + column + " FROM " + table + " WHERE " + whereCol + "=?;";
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr);
    sqlite3_bind_text(st, 1, whereVal.c_str(), -1, SQLITE_TRANSIENT);
    std::string v;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
        v = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    sqlite3_close(db);
    return v;
}

// Seed the agent's A2A ECIES keypair so initialize()/loadIdentity() restores it. The no-wallet
// harness leaves it intact, so the agent holds the private half it needs to decrypt its inbox.
// (Same seam test_a2a_inbox.cpp uses.)
static void seedIdentity(const std::string& dir, const ECIESKeypair& kp) {
    { PilotImpl boot; boot.initialize(dir); }   // create the schema (no wallet -> returns false)
    execSql(dir, "INSERT OR REPLACE INTO agent_identity (id, npk, account_id, created_at) "
                 "VALUES (1,'agentnpk','acct','0');");
    execSql(dir, "INSERT OR REPLACE INTO config (key,value) VALUES ('ecies.pub','"
                 + kp.publicKeyHex + "');");
    execSql(dir, "INSERT OR REPLACE INTO config (key,value) VALUES ('ecies.priv','"
                 + kp.privateKeyHex + "');");
}

// What a peer's messagingSend / storageShare / messagingCreateGroup puts on the wire: the
// JSON object, ECIES-sealed to the recipient's advertised key.
static std::string sealTo(const std::string& pubHex, const QJsonObject& obj) {
    std::string plain = QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString();
    std::vector<uint8_t> bytes(plain.begin(), plain.end());
    return eciesSerialize(eciesEncrypt(pubHex, bytes));
}

static QJsonArray inboxOf(PilotImpl& impl) {
    QJsonDocument d = QJsonDocument::fromJson(QByteArray::fromStdString(impl.messagingInbox()));
    return d.isObject() ? d.object().value("messages").toArray() : QJsonArray();
}

static bool hasTopic(const std::vector<std::string>& topics, const std::string& t) {
    for (const auto& x : topics) if (x == t) return true;
    return false;
}

// A 32-byte AES-256 group key as 64 hex chars (the key half of the file-key format).
static std::string freshGroupKeyHex() {
    return aesKeyToHex(generateFileKey()).substr(0, 64);
}

LOGOS_TEST(messaging_inbox_reports_not_initialized_before_init) {
    PilotImpl impl;
    LOGOS_ASSERT_CONTAINS(impl.messagingInbox(), "not initialized");
}

LOGOS_TEST(storage_peer_info_and_connect_report_not_initialized_before_init) {
    PilotImpl impl;
    LOGOS_ASSERT_CONTAINS(impl.storagePeerInfo(), "not initialized");
    LOGOS_ASSERT_CONTAINS(impl.storageConnect("16Uiu2peer", "[\"/ip4/127.0.0.1/tcp/8070\"]"),
                          "not initialized");
}

LOGOS_TEST(inbound_direct_message_lands_in_the_inbox) {
    std::string dir = msgDir("direct");
    ECIESKeypair kp = generateECIESKeypair();
    seedIdentity(dir, kp);
    PilotImpl agent; agent.initialize(dir);

    QJsonObject msg;
    msg["type"] = QString("direct");
    msg["from"] = QString("peer-a-key");
    msg["message"] = QString("hello from a");
    agent.handleInboundA2A(sealTo(kp.publicKeyHex, msg));

    QJsonArray inbox = inboxOf(agent);
    LOGOS_ASSERT_EQ(inbox.size(), 1);
    QJsonObject m = inbox[0].toObject();
    LOGOS_ASSERT_EQ(m["kind"].toString().toStdString(), std::string("direct"));
    LOGOS_ASSERT_EQ(m["from"].toString().toStdString(), std::string("peer-a-key"));
    LOGOS_ASSERT_EQ(m["message"].toString().toStdString(), std::string("hello from a"));
}

LOGOS_TEST(inbound_direct_message_without_a_type_field_is_still_a_message) {
    // The pre-2026-09-04 messagingSend put {"from","message"} on the wire with no "type";
    // a peer on that build must still be heard.
    std::string dir = msgDir("direct_legacy");
    ECIESKeypair kp = generateECIESKeypair();
    seedIdentity(dir, kp);
    PilotImpl agent; agent.initialize(dir);

    QJsonObject msg;
    msg["from"] = QString("old-peer");
    msg["message"] = QString("legacy hello");
    agent.handleInboundA2A(sealTo(kp.publicKeyHex, msg));

    QJsonArray inbox = inboxOf(agent);
    LOGOS_ASSERT_EQ(inbox.size(), 1);
    LOGOS_ASSERT_EQ(inbox[0].toObject()["message"].toString().toStdString(), std::string("legacy hello"));
}

LOGOS_TEST(inbound_file_share_records_the_key_so_download_can_find_it) {
    std::string dir = msgDir("share");
    ECIESKeypair kp = generateECIESKeypair();
    seedIdentity(dir, kp);
    PilotImpl agent; agent.initialize(dir);

    QJsonObject share;
    share["type"] = QString("file_share");
    share["cid"] = QString("zDvShared1");
    share["key"] = QString("aa:bb:cc");
    share["from"] = QString("peer-a-key");
    share["label"] = QString("report");
    agent.handleInboundA2A(sealTo(kp.publicKeyHex, share));

    // storageDownload looks the key up in stored_files by CID — that row must now exist.
    LOGOS_ASSERT_EQ(col(dir, "stored_files", "file_key_encrypted", "cid", "zDvShared1"), std::string("aa:bb:cc"));
    std::string list = agent.storageList();
    LOGOS_ASSERT_CONTAINS(list, "zDvShared1");
    LOGOS_ASSERT_CONTAINS(list, "peer-a-key");
    LOGOS_ASSERT_CONTAINS(list, "report");
    // And the owner can see that a share arrived.
    QJsonArray inbox = inboxOf(agent);
    LOGOS_ASSERT_EQ(inbox.size(), 1);
    LOGOS_ASSERT_EQ(inbox[0].toObject()["kind"].toString().toStdString(), std::string("file_share"));
    LOGOS_ASSERT_CONTAINS(inbox[0].toObject()["message"].toString().toStdString(), "zDvShared1");
}

LOGOS_TEST(inbound_file_share_never_overwrites_the_key_of_a_file_we_uploaded) {
    // Anyone can send a share. It must not be able to replace the key of a file WE stored,
    // or a stranger could brick our own downloads.
    std::string dir = msgDir("share_no_overwrite");
    ECIESKeypair kp = generateECIESKeypair();
    seedIdentity(dir, kp);
    execSql(dir, "INSERT INTO stored_files (cid, label, file_key_encrypted, timestamp, size_bytes) "
                 "VALUES ('zOwn', 'mine', '11:22:33', '0', 0);");
    PilotImpl agent; agent.initialize(dir);

    QJsonObject share;
    share["type"] = QString("file_share");
    share["cid"] = QString("zOwn");
    share["key"] = QString("99:99:99");
    share["from"] = QString("stranger");
    agent.handleInboundA2A(sealTo(kp.publicKeyHex, share));

    LOGOS_ASSERT_EQ(col(dir, "stored_files", "file_key_encrypted", "cid", "zOwn"), std::string("11:22:33"));
    LOGOS_ASSERT_EQ(col(dir, "stored_files", "label", "cid", "zOwn"), std::string("mine"));
}

LOGOS_TEST(inbound_group_invite_is_recorded_and_join_adds_the_topic_to_the_poll_set) {
    std::string dir = msgDir("invite_join");
    ECIESKeypair kp = generateECIESKeypair();
    seedIdentity(dir, kp);
    PilotImpl agent; agent.initialize(dir);
    std::string key = freshGroupKeyHex();

    QJsonObject invite;
    invite["type"] = QString("group_invite");
    invite["group_id"] = QString("g1");
    invite["topic"] = QString("/pilot/1/group-g1/proto");
    invite["key"] = QString::fromStdString(key);
    invite["from"] = QString("peer-a-key");
    agent.handleInboundA2A(sealTo(kp.publicKeyHex, invite));

    QJsonArray inbox = inboxOf(agent);
    LOGOS_ASSERT_EQ(inbox.size(), 1);
    LOGOS_ASSERT_EQ(inbox[0].toObject()["kind"].toString().toStdString(), std::string("group_invite"));
    LOGOS_ASSERT_EQ(inbox[0].toObject()["group_id"].toString().toStdString(), std::string("g1"));
    LOGOS_ASSERT_EQ(col(dir, "messaging_groups", "key_hex", "group_id", "g1"), key);
    LOGOS_ASSERT_EQ(col(dir, "messaging_groups", "joined", "group_id", "g1"), std::string("0"));

    // Invited is not joined: the pull path must not read a group the owner never joined.
    LOGOS_ASSERT_FALSE(hasTopic(agent.agentPollTopics(), "/pilot/1/group-g1/proto"));

    LOGOS_ASSERT_FALSE(agent.messagingJoin("never-invited"));
    LOGOS_ASSERT_TRUE(agent.messagingJoin("g1"));
    LOGOS_ASSERT_EQ(col(dir, "messaging_groups", "joined", "group_id", "g1"), std::string("1"));
    LOGOS_ASSERT_TRUE(hasTopic(agent.agentPollTopics(), "/pilot/1/group-g1/proto"));
}

LOGOS_TEST(group_message_on_a_joined_group_decrypts_into_the_inbox) {
    std::string dir = msgDir("group_msg");
    ECIESKeypair kp = generateECIESKeypair();
    seedIdentity(dir, kp);
    PilotImpl agent; agent.initialize(dir);
    std::string key = freshGroupKeyHex();

    QJsonObject invite;
    invite["type"] = QString("group_invite");
    invite["group_id"] = QString("g1");
    invite["topic"] = QString("/pilot/1/group-g1/proto");
    invite["key"] = QString::fromStdString(key);
    invite["from"] = QString("peer-a-key");
    agent.handleInboundA2A(sealTo(kp.publicKeyHex, invite));
    LOGOS_ASSERT_TRUE(agent.messagingJoin("g1"));

    // Enter where the store poll and the live event both enter: by topic.
    std::string sealed = pilotSealGroupMessage(key, "peer-a-key", "hi group");
    agent.handleInboundMessage("/pilot/1/group-g1/proto", sealed);

    QJsonArray inbox = inboxOf(agent);
    LOGOS_ASSERT_EQ(inbox.size(), 2);   // newest first: the message, then the invite
    QJsonObject m = inbox[0].toObject();
    LOGOS_ASSERT_EQ(m["kind"].toString().toStdString(), std::string("group"));
    LOGOS_ASSERT_EQ(m["group_id"].toString().toStdString(), std::string("g1"));
    LOGOS_ASSERT_EQ(m["from"].toString().toStdString(), std::string("peer-a-key"));
    LOGOS_ASSERT_EQ(m["message"].toString().toStdString(), std::string("hi group"));

    // A message on a group we were never invited to is dropped quietly.
    agent.handleInboundMessage("/pilot/1/group-g9/proto", sealed);
    LOGOS_ASSERT_EQ(inboxOf(agent).size(), 2);
    // A message on a group we know but sealed under a different key is dropped too.
    agent.handleInboundMessage("/pilot/1/group-g1/proto",
                               pilotSealGroupMessage(freshGroupKeyHex(), "x", "forged"));
    LOGOS_ASSERT_EQ(inboxOf(agent).size(), 2);
}

LOGOS_TEST(group_envelope_roundtrips_and_a_wrong_key_fails) {
    std::string k1 = freshGroupKeyHex(), k2 = freshGroupKeyHex();
    std::string sealed = pilotSealGroupMessage(k1, "me", "secret text");
    LOGOS_ASSERT_TRUE(sealed.find("secret text") == std::string::npos);   // not plaintext
    std::string from, msg;
    LOGOS_ASSERT_TRUE(pilotOpenGroupMessage(k1, sealed, from, msg));
    LOGOS_ASSERT_EQ(from, std::string("me"));
    LOGOS_ASSERT_EQ(msg, std::string("secret text"));
    LOGOS_ASSERT_FALSE(pilotOpenGroupMessage(k2, sealed, from, msg));
    LOGOS_ASSERT_FALSE(pilotOpenGroupMessage(k1, "not an envelope", from, msg));
    // Fresh IV per message: the same text never seals to the same bytes.
    LOGOS_ASSERT_TRUE(pilotSealGroupMessage(k1, "me", "secret text") != sealed);
    // A key that is not 32 bytes of hex seals nothing.
    LOGOS_ASSERT_EQ(pilotSealGroupMessage("abc", "me", "x"), std::string(""));
}

LOGOS_TEST(members_parse_as_a_json_array_or_the_legacy_comma_list) {
    std::vector<std::string> a = pilotParseMembers("[\"aa\", \"bb\"]");
    LOGOS_ASSERT_EQ(a.size(), static_cast<size_t>(2));
    LOGOS_ASSERT_EQ(a[0], std::string("aa"));
    LOGOS_ASSERT_EQ(a[1], std::string("bb"));
    std::vector<std::string> b = pilotParseMembers("aa, bb");
    LOGOS_ASSERT_EQ(b.size(), static_cast<size_t>(2));
    LOGOS_ASSERT_EQ(b[1], std::string("bb"));
    LOGOS_ASSERT_EQ(pilotParseMembers("[]").size(), static_cast<size_t>(0));
    LOGOS_ASSERT_EQ(pilotParseMembers("").size(), static_cast<size_t>(0));
    // A member given as a card object resolves to the card's encryption key.
    std::vector<std::string> c = pilotParseMembers(
        "[{\"name\":\"peer\",\"_logos\":{\"enc_key\":\"02abc\"}}]");
    LOGOS_ASSERT_EQ(c.size(), static_cast<size_t>(1));
    LOGOS_ASSERT_EQ(c[0], std::string("02abc"));
}

LOGOS_TEST(recipient_key_prefers_the_cards_enc_key) {
    // The inbox is named after the agent's ENCRYPTION key (a2aSelfEncKey, advertised as
    // _logos.enc_key). Sending to the wallet viewing key reaches nobody — which is exactly
    // what the two-agent test did for two months.
    LOGOS_ASSERT_EQ(pilotRecipientKey("02deadbeef"), std::string("02deadbeef"));
    LOGOS_ASSERT_EQ(pilotRecipientKey("{\"_logos\":{\"enc_key\":\"02enc\",\"signing_key\":\"02sig\"}}"),
                    std::string("02enc"));
    LOGOS_ASSERT_EQ(pilotRecipientKey("{\"viewing_public_key\":\"02view\"}"), std::string("02view"));
}

LOGOS_TEST(junk_application_payload_is_dropped_without_a_trace_in_the_inbox) {
    std::string dir = msgDir("junk");
    ECIESKeypair kp = generateECIESKeypair();
    seedIdentity(dir, kp);
    PilotImpl agent; agent.initialize(dir);

    QJsonObject junk;
    junk["hello"] = QString("world");
    agent.handleInboundA2A(sealTo(kp.publicKeyHex, junk));
    QJsonObject badInvite;    // an invite without a usable key must not create a group
    badInvite["type"] = QString("group_invite");
    badInvite["group_id"] = QString("g-bad");
    badInvite["key"] = QString("short");
    agent.handleInboundA2A(sealTo(kp.publicKeyHex, badInvite));

    LOGOS_ASSERT_EQ(inboxOf(agent).size(), 0);
    LOGOS_ASSERT_EQ(col(dir, "messaging_groups", "topic", "group_id", "g-bad"), std::string(""));
}
