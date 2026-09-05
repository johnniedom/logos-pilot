#include <logos_test.h>
#include "../src/pilot_impl.h"
#include "../src/pilot_crypto.h"
#include <sqlite3.h>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdio>

LOGOS_TEST(echo_returns_input) {
    PilotImpl impl;
    LOGOS_ASSERT_EQ(impl.echo("hello"), std::string("echo: hello"));
}

LOGOS_TEST(echo_handles_empty_input) {
    PilotImpl impl;
    LOGOS_ASSERT_EQ(impl.echo(""), std::string("echo: "));
}

LOGOS_TEST(not_initialized_by_default) {
    PilotImpl impl;
    LOGOS_ASSERT_FALSE(impl.isInitialized());
}

LOGOS_TEST(npk_empty_before_init) {
    PilotImpl impl;
    LOGOS_ASSERT_EQ(impl.getAgentNpk(), std::string(""));
}

LOGOS_TEST(account_id_empty_before_init) {
    PilotImpl impl;
    LOGOS_ASSERT_EQ(impl.getAccountId(), std::string(""));
}

LOGOS_TEST(owner_channel_empty_before_init) {
    PilotImpl impl;
    LOGOS_ASSERT_EQ(impl.getOwnerChannelId(), std::string(""));
}

LOGOS_TEST(wallet_balance_returns_error_before_init) {
    PilotImpl impl;
    std::string result = impl.walletBalance();
    LOGOS_ASSERT_CONTAINS(result, "not initialized");
}

LOGOS_TEST(wallet_history_returns_error_before_init) {
    PilotImpl impl;
    std::string result = impl.walletHistory();
    LOGOS_ASSERT_CONTAINS(result, "not initialized");
}

LOGOS_TEST(chain_account_returns_error_before_init) {
    PilotImpl impl;
    LOGOS_ASSERT_CONTAINS(impl.chainAccount("HkwBGetZJytPq7fMnDNqUVYUug4fbWQwHTNkJtH4yh7x"), "not initialized");
}

LOGOS_TEST(account_id_form_hex_is_told_apart_from_base58) {
    // The testnet's own getAccount takes base58; the wallet takes 64 hex. chainAccount converts
    // base58 through the wallet, so the two forms must be told apart reliably.
    LOGOS_ASSERT_TRUE(pilotLooksLikeAccountHex("f8fc394c0e5440c4188236d1693076b0cfad04984cf67ca64e0e43a173144f63"));
    LOGOS_ASSERT_FALSE(pilotLooksLikeAccountHex("HkwBGetZJytPq7fMnDNqUVYUug4fbWQwHTNkJtH4yh7x"));
    LOGOS_ASSERT_FALSE(pilotLooksLikeAccountHex("f8fc394c"));
    LOGOS_ASSERT_FALSE(pilotLooksLikeAccountHex(std::string(64, 'g')));
    LOGOS_ASSERT_FALSE(pilotLooksLikeAccountHex(""));
}

LOGOS_TEST(little_endian_hex_amounts_decode_to_decimal) {
    // The wallet's account record carries amounts as 16-byte little-endian hex — the form
    // funding writes with u128LeHex. 0x96 = 150 (a fresh faucet claim), 0x95 = 149 after a
    // 1-LEZ spend: the first alerter run (2026-09-05) printed these raw.
    LOGOS_ASSERT_EQ(pilotLeHexToDecimal("96000000000000000000000000000000"), std::string("150"));
    LOGOS_ASSERT_EQ(pilotLeHexToDecimal("95000000000000000000000000000000"), std::string("149"));
    LOGOS_ASSERT_EQ(pilotLeHexToDecimal("00000000000000000000000000000000"), std::string("0"));
    LOGOS_ASSERT_EQ(pilotLeHexToDecimal("0100000000000000"), std::string("1"));       // 8-byte nonce form
    LOGOS_ASSERT_EQ(pilotLeHexToDecimal("e803000000000000"), std::string("1000"));
    LOGOS_ASSERT_EQ(pilotLeHexToDecimal("ffffffffffffffffffffffffffffffff"), std::string("340282366920938463463374607431768211455"));
    LOGOS_ASSERT_EQ(pilotLeHexToDecimal("abc"), std::string(""));       // odd length
    LOGOS_ASSERT_EQ(pilotLeHexToDecimal("zz"), std::string(""));        // not hex
    LOGOS_ASSERT_EQ(pilotLeHexToDecimal(""), std::string(""));
}

LOGOS_TEST(program_call_arguments_parse_strictly) {
    // program.call sends exactly the words the owner supplies; a malformed list is refused,
    // never rounded or guessed.
    bool ok = false;
    std::vector<uint32_t> w = pilotParseWordList("[1, 2, 4294967295]", &ok);
    LOGOS_ASSERT_TRUE(ok);
    LOGOS_ASSERT_EQ(w.size(), static_cast<size_t>(3));
    LOGOS_ASSERT_EQ(w[2], static_cast<uint32_t>(4294967295u));
    w = pilotParseWordList("7, 8", &ok);
    LOGOS_ASSERT_TRUE(ok);
    LOGOS_ASSERT_EQ(w.size(), static_cast<size_t>(2));
    LOGOS_ASSERT_EQ(w[1], static_cast<uint32_t>(8));
    w = pilotParseWordList("[]", &ok);
    LOGOS_ASSERT_TRUE(ok);
    LOGOS_ASSERT_EQ(w.size(), static_cast<size_t>(0));
    pilotParseWordList("[-1]", &ok);          LOGOS_ASSERT_FALSE(ok);
    pilotParseWordList("[4294967296]", &ok);  LOGOS_ASSERT_FALSE(ok);   // one past u32
    pilotParseWordList("abc", &ok);           LOGOS_ASSERT_FALSE(ok);
    pilotParseWordList("[1.5]", &ok);         LOGOS_ASSERT_FALSE(ok);

    std::vector<bool> f = pilotParseBoolList("[true, false]", &ok);
    LOGOS_ASSERT_TRUE(ok);
    LOGOS_ASSERT_EQ(f.size(), static_cast<size_t>(2));
    LOGOS_ASSERT_TRUE(f[0]); LOGOS_ASSERT_FALSE(f[1]);
    f = pilotParseBoolList("1,0,true", &ok);
    LOGOS_ASSERT_TRUE(ok);
    LOGOS_ASSERT_EQ(f.size(), static_cast<size_t>(3));
    pilotParseBoolList("[1, \"maybe\"]", &ok); LOGOS_ASSERT_FALSE(ok);
}

LOGOS_TEST(program_skills_report_not_initialized_before_init) {
    PilotImpl impl;
    LOGOS_ASSERT_CONTAINS(impl.programQuery("HkwBGetZJytPq7fMnDNqUVYUug4fbWQwHTNkJtH4yh7x", "{}"), "not initialized");
    LOGOS_ASSERT_CONTAINS(impl.programCall("00", "[1]", "{}"), "not initialized");
    LOGOS_ASSERT_CONTAINS(impl.programDeploy("builtin:token"), "not initialized");
}

LOGOS_TEST(wallet_send_returns_error_before_init) {
    PilotImpl impl;
    std::string result = impl.walletSend("recipient", 100, "test");
    LOGOS_ASSERT_CONTAINS(result, "not initialized");
}

LOGOS_TEST(storage_list_returns_error_before_init) {
    PilotImpl impl;
    std::string result = impl.storageList();
    LOGOS_ASSERT_CONTAINS(result, "not initialized");
}

LOGOS_TEST(storage_upload_returns_error_before_init) {
    PilotImpl impl;
    std::string result = impl.storageUpload("/tmp/test.txt", "test");
    LOGOS_ASSERT_CONTAINS(result, "not initialized");
}

LOGOS_TEST(messaging_send_returns_error_before_init) {
    PilotImpl impl;
    std::string result = impl.messagingSend("recipient", "hello");
    LOGOS_ASSERT_CONTAINS(result, "not initialized");
}

LOGOS_TEST(agent_card_returns_error_before_init) {
    PilotImpl impl;
    std::string result = impl.agentCard();
    LOGOS_ASSERT_CONTAINS(result, "not initialized");
}

LOGOS_TEST(agent_discover_returns_error_before_init) {
    PilotImpl impl;
    std::string result = impl.agentDiscover("");
    LOGOS_ASSERT_CONTAINS(result, "not initialized");
}

LOGOS_TEST(meta_skills_lists_all_skills) {
    PilotImpl impl;
    std::string result = impl.metaSkills();
    LOGOS_ASSERT_CONTAINS(result, "wallet.balance");
    LOGOS_ASSERT_CONTAINS(result, "wallet.send");
    LOGOS_ASSERT_CONTAINS(result, "storage.upload");
    LOGOS_ASSERT_CONTAINS(result, "storage.download");
    LOGOS_ASSERT_CONTAINS(result, "messaging.send");
    LOGOS_ASSERT_CONTAINS(result, "agent.card");
    LOGOS_ASSERT_CONTAINS(result, "agent.ask");
    LOGOS_ASSERT_CONTAINS(result, "agent.discover");
    LOGOS_ASSERT_CONTAINS(result, "program.query");
    LOGOS_ASSERT_CONTAINS(result, "meta.skills");
    LOGOS_ASSERT_CONTAINS(result, "meta.status");
    LOGOS_ASSERT_CONTAINS(result, "agent.import_card");
    LOGOS_ASSERT_CONTAINS(result, "\"count\":23");
}

LOGOS_TEST(meta_status_shows_not_initialized) {
    PilotImpl impl;
    std::string result = impl.metaStatus();
    LOGOS_ASSERT_CONTAINS(result, "\"initialized\":false");
}

LOGOS_TEST(establish_owner_channel_fails_without_api) {
    PilotImpl impl;
    LOGOS_ASSERT_FALSE(impl.establishOwnerChannel());
}

LOGOS_TEST(send_to_owner_fails_without_channel) {
    PilotImpl impl;
    LOGOS_ASSERT_FALSE(impl.sendToOwner("test message"));
}

LOGOS_TEST(messaging_join_fails_without_api) {
    PilotImpl impl;
    LOGOS_ASSERT_FALSE(impl.messagingJoin("group123"));
}

LOGOS_TEST(agent_cancel_fails_without_api) {
    PilotImpl impl;
    LOGOS_ASSERT_FALSE(impl.agentCancel("addr", "task123"));
}

// --- M2: identity key-at-rest migration (legacy plaintext -> wrapped on load) ---

static std::string pilotMigDir(const std::string& name) {
    std::string base = "/tmp";
    if (const char* t = std::getenv("TMPDIR")) base = t;
    std::string dir = base + "/pilot_idmig_" + name;
    std::remove((dir + "/pilot.db").c_str());
    std::remove((dir + "/pilot.db-wal").c_str());
    std::remove((dir + "/pilot.db-shm").c_str());
    return dir;
}

static void pilotMigExec(const std::string& dir, const std::string& sql) {
    sqlite3* db = nullptr;
    sqlite3_open((dir + "/pilot.db").c_str(), &db);
    sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(db);
}

static std::string pilotMigConfig(const std::string& dir, const std::string& key) {
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

// A pre-M2 DB stores ecies.priv as raw hex. When the operator now sets
// PILOT_KEY_PASSPHRASE, loadIdentity must re-wrap the stored value in place (it is no
// longer plaintext on disk and is fully recoverable) WITHOUT regenerating the identity.
// PILOT_KEY_PASSPHRASE is cleared BEFORE any assert so a later test in this single-process
// runner never re-wraps its own seeded plaintext keys.
LOGOS_TEST(identity_at_rest_migrates_plaintext_to_wrapped) {
    std::string dir = pilotMigDir("plain2wrapped");

    // 1. Build the schema (initialize returns false with no wallet, but db_ is opened).
    { PilotImpl seed; seed.initialize(dir); }

    // 2. Seed a legacy plaintext identity: an agent_identity row (so loadIdentity runs the
    //    config loop) + a raw-hex ecies.priv (the pre-M2 on-disk form) + ecies.pub.
    ECIESKeypair kp = generateECIESKeypair();
    pilotMigExec(dir,
        "INSERT OR REPLACE INTO agent_identity (id, npk, account_id, created_at) "
        "VALUES (1, 'npk-mig', 'acct-mig', '0');");
    pilotMigExec(dir,
        "INSERT OR REPLACE INTO config (key, value) VALUES ('ecies.pub', '" +
        kp.publicKeyHex + "');");
    pilotMigExec(dir,
        "INSERT OR REPLACE INTO config (key, value) VALUES ('ecies.priv', '" +
        kp.privateKeyHex + "');");

    // 3. Load with a passphrase set -> loadIdentity migrates the stored key in place.
    setenv("PILOT_KEY_PASSPHRASE", "operator-secret", 1);
    { PilotImpl impl; impl.initialize(dir); }

    // Capture the on-disk result, then CLEAR the env BEFORE any assert.
    std::string stored = pilotMigConfig(dir, "ecies.priv");
    std::string storedPub = pilotMigConfig(dir, "ecies.pub");
    unsetenv("PILOT_KEY_PASSPHRASE");

    bool wrapped = isWrappedSecret(stored);
    bool differs = (stored != kp.privateKeyHex);
    std::string recovered = wrapped ? unwrapSecret(stored, "operator-secret") : std::string();

    LOGOS_ASSERT_TRUE(wrapped);                    // ecies.priv is no longer plaintext on disk
    LOGOS_ASSERT_TRUE(differs);
    LOGOS_ASSERT_EQ(recovered, kp.privateKeyHex);  // the identity key is fully recoverable
    LOGOS_ASSERT_EQ(storedPub, kp.publicKeyHex);   // identity never regenerated (pub untouched)
}
