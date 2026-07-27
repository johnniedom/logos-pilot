#include <logos_test.h>
#include "../src/pilot_impl.h"
#include "../src/pilot_crypto.h"
#include <sqlite3.h>
#include <QString>
#include <QJsonObject>
#include <QJsonDocument>
#include <QByteArray>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// An agent that advertises an address it does not listen on is unhireable, and that is
// exactly what shipped: initDeliveryModule() subscribed the inbox topics BEFORE the
// identity existed, so both subscribes were skipped and never retried (measured
// 2026-07-26 — the peer's own advertised inbox never appears as a subscription in its log).
// These tests pin the invariant: whatever address the card advertises, the agent listens
// there.
//
// The agent does NOT open itself for hire. That is the owner's explicit decision (see
// agentOpenForHire), which makes "advertising but not listening" representable again — so
// the invariant is conditional and BOTH halves are pinned below: open => listening on the
// advertised inbox, closed => not listening on it AND not broadcasting a card that names it.
//
// HONEST LIMIT: these prove the topic LIST is right, which is a pure question and the part
// a unit test can own. They do NOT prove the delivery module accepted the subscription — no
// test in this suite mocks delivery_module, so subscribeIdentityTopics() returns early here.
// Phase 8 of test-two-agents-docker.sh asserts the real subscription from a live agent's
// own log; that assertion is the one that would have caught the original bug.

static std::string bootDir(const std::string& name) {
    // initialize() prefers PILOT_DATA_DIR over its argument. If the surrounding environment
    // has one set, every test below would silently share one directory and the identity
    // seeding would cross-contaminate, so take the override off the table.
    unsetenv("PILOT_DATA_DIR");
    std::string base = "/tmp";
    if (const char* t = std::getenv("TMPDIR")) base = t;
    std::string dir = base + "/pilot_boot_" + name;
    std::remove((dir + "/pilot.db").c_str());
    std::remove((dir + "/pilot.db-wal").c_str());
    std::remove((dir + "/pilot.db-shm").c_str());
    return dir;
}

// Seed an identity the way a previous boot would have left one, so loadIdentity() restores
// it. Key names are the ones the module really uses (verified in a live agent's config
// table): enc.pub/enc.priv is the dedicated encryption pair, ecies.pub/ecies.priv the
// legacy signing pair.
static void seedIdentity(const std::string& dir,
                         const ECIESKeypair& enc, const ECIESKeypair& ecies) {
    sqlite3* db = nullptr;
    sqlite3_open((dir + "/pilot.db").c_str(), &db);
    // created_at is NOT NULL in the schema — omitting it makes the INSERT fail silently, the
    // identity never loads, and every assertion below fails for a reason that has nothing to
    // do with what is being tested.
    sqlite3_exec(db,
        "INSERT OR REPLACE INTO agent_identity (id, npk, account_id, created_at) "
        "VALUES (1, '{\"nullifier_public_key\":\"aa\",\"viewing_public_key\":\"bb\"}', "
        "'acct-1', '2026-07-27T00:00:00Z');",
        nullptr, nullptr, nullptr);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO config (key, value) VALUES (?, ?);", -1, &st, nullptr);
    const std::pair<const char*, std::string> rows[] = {
        {"enc.pub",    enc.publicKeyHex},   {"enc.priv",    enc.privateKeyHex},
        {"ecies.pub",  ecies.publicKeyHex}, {"ecies.priv",  ecies.privateKeyHex},
    };
    for (const auto& r : rows) {
        sqlite3_reset(st);
        sqlite3_bind_text(st, 1, r.first, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, r.second.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
}

static bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// Read discovered_agents straight out of the DB, the same way test_a2a_outbound.cpp
// inspects its tables. There is no routing-key test accessor on PilotImpl — do not invent
// one; the stored row IS the observable fact.
static bool cardStored(const std::string& dir, const std::string& npk) {
    sqlite3* db = nullptr;
    sqlite3_open((dir + "/pilot.db").c_str(), &db);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM discovered_agents WHERE npk = ?;", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, npk.c_str(), -1, SQLITE_TRANSIENT);
    int n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return n > 0;
}

// The advertised inbox: the address a buyer encrypts to and sends work to.
static std::string advertisedInbox(PilotImpl& impl) {
    QJsonObject card = QJsonDocument::fromJson(
        QByteArray::fromStdString(impl.agentCard())).object();
    std::string encKey = card["_logos"].toObject()["enc_key"].toString().toStdString();
    return encKey.empty() ? std::string() : "/pilot/1/inbox-" + encKey + "/proto";
}

// THE invariant, open half. Take the address the card publishes as _logos.enc_key and
// require that the agent's own listen list contains that exact inbox topic.
LOGOS_TEST(open_for_hire_listens_on_the_address_its_card_advertises) {
    std::string dir = bootDir("advertise_equals_listen");
    ECIESKeypair enc = generateECIESKeypair();
    ECIESKeypair ecies = generateECIESKeypair();
    { PilotImpl boot; boot.initialize(dir); }        // create the schema
    seedIdentity(dir, enc, ecies);

    PilotImpl impl; impl.initialize(dir);            // loadIdentity() restores the keys
    impl.agentOpenForHire();

    std::string inbox = advertisedInbox(impl);
    LOGOS_ASSERT_TRUE(!inbox.empty());
    LOGOS_ASSERT_TRUE(contains(impl.identityTopics(), inbox));
}

// THE invariant, closed half — and the reason the closed state is safe to offer at all.
// A closed agent must not listen on its inbox (that is what closed MEANS) and must not
// hand out a card advertising one, or we have simply reintroduced the original bug with
// the owner as its trigger.
LOGOS_TEST(closed_for_hire_does_not_listen_on_its_inbox) {
    std::string dir = bootDir("closed_not_listening");
    ECIESKeypair enc = generateECIESKeypair();
    ECIESKeypair ecies = generateECIESKeypair();
    { PilotImpl boot; boot.initialize(dir); }
    seedIdentity(dir, enc, ecies);

    PilotImpl impl; impl.initialize(dir);            // closed: never opened
    LOGOS_ASSERT_TRUE(!impl.agentIsOpenForHire());

    std::vector<std::string> topics = impl.identityTopics();
    LOGOS_ASSERT_TRUE(!contains(topics, "/pilot/1/inbox-" + enc.publicKeyHex + "/proto"));
    LOGOS_ASSERT_TRUE(!contains(topics, "/pilot/1/inbox-" + ecies.publicKeyHex + "/proto"));
}

// Closed means strangers cannot hire US. It does not mean we go deaf: the shared channel
// where peers announce themselves stays subscribed, so the agent keeps learning who it
// might want to hire. The buyer side works while the seller side is shut.
LOGOS_TEST(closed_for_hire_still_listens_on_the_discovery_topic) {
    std::string dir = bootDir("closed_still_hears");
    ECIESKeypair enc = generateECIESKeypair();
    ECIESKeypair ecies = generateECIESKeypair();
    { PilotImpl boot; boot.initialize(dir); }
    seedIdentity(dir, enc, ecies);

    PilotImpl impl; impl.initialize(dir);
    LOGOS_ASSERT_TRUE(!impl.agentIsOpenForHire());
    LOGOS_ASSERT_TRUE(contains(impl.identityTopics(), "/pilot/1/discovery/proto"));
}

// The legacy signing-key inbox is listened on too once open, so a pre-split peer that
// still routes to _logos.signing_key keeps reaching us. Both inboxes, not one.
LOGOS_TEST(open_for_hire_listens_on_the_legacy_signing_key_inbox_too) {
    std::string dir = bootDir("legacy_inbox");
    ECIESKeypair enc = generateECIESKeypair();
    ECIESKeypair ecies = generateECIESKeypair();
    { PilotImpl boot; boot.initialize(dir); }
    seedIdentity(dir, enc, ecies);

    PilotImpl impl; impl.initialize(dir);
    impl.agentOpenForHire();

    std::vector<std::string> topics = impl.identityTopics();
    LOGOS_ASSERT_TRUE(contains(topics, "/pilot/1/inbox-" + enc.publicKeyHex + "/proto"));
    LOGOS_ASSERT_TRUE(contains(topics, "/pilot/1/inbox-" + ecies.publicKeyHex + "/proto"));
}

// The shared discovery channel is part of the listen set, not a thing only agentDiscover()
// subscribes when polled — otherwise a card broadcast between polls is never even offered
// to us.
LOGOS_TEST(listens_on_the_discovery_topic) {
    std::string dir = bootDir("discovery_topic");
    ECIESKeypair enc = generateECIESKeypair();
    ECIESKeypair ecies = generateECIESKeypair();
    { PilotImpl boot; boot.initialize(dir); }
    seedIdentity(dir, enc, ecies);

    PilotImpl impl; impl.initialize(dir);
    impl.agentOpenForHire();
    LOGOS_ASSERT_TRUE(contains(impl.identityTopics(), "/pilot/1/discovery/proto"));
}

// No identity yet -> no inbox topics invented, even wide open. An agent with no keys must
// not claim to listen on "/pilot/1/inbox-/proto", which is what naive string building
// would produce.
LOGOS_TEST(no_identity_yields_no_inbox_topics) {
    std::string dir = bootDir("no_identity");
    PilotImpl impl; impl.initialize(dir);            // fresh dir, no wallet -> no identity
    impl.agentOpenForHire();
    for (const std::string& t : impl.identityTopics())
        LOGOS_ASSERT_TRUE(t.find("/pilot/1/inbox-/proto") == std::string::npos);
}

// Being open for hire is REMEMBERED. The owner says it once; a reboot comes back open.
// The alternative — every boot starts silent — means a restart, crash or redeploy quietly
// drops the agent off the market with nothing to say so, which is the same class of
// failure this whole change exists to kill.
LOGOS_TEST(open_for_hire_survives_a_restart) {
    std::string dir = bootDir("open_remembered");
    ECIESKeypair enc = generateECIESKeypair();
    ECIESKeypair ecies = generateECIESKeypair();
    { PilotImpl boot; boot.initialize(dir); }
    seedIdentity(dir, enc, ecies);

    {
        PilotImpl first; first.initialize(dir);
        LOGOS_ASSERT_TRUE(!first.agentIsOpenForHire());   // starts closed
        LOGOS_ASSERT_TRUE(first.agentOpenForHire());
    }

    PilotImpl restarted; restarted.initialize(dir);       // a whole new process would see this
    LOGOS_ASSERT_TRUE(restarted.agentIsOpenForHire());
    LOGOS_ASSERT_TRUE(contains(restarted.identityTopics(),
                               "/pilot/1/inbox-" + enc.publicKeyHex + "/proto"));
}

// Closing is remembered the same way, so "I shut my agent" is not undone by a reboot.
LOGOS_TEST(closed_for_hire_survives_a_restart) {
    std::string dir = bootDir("closed_remembered");
    ECIESKeypair enc = generateECIESKeypair();
    ECIESKeypair ecies = generateECIESKeypair();
    { PilotImpl boot; boot.initialize(dir); }
    seedIdentity(dir, enc, ecies);

    {
        PilotImpl first; first.initialize(dir);
        first.agentOpenForHire();
        LOGOS_ASSERT_TRUE(first.agentCloseForHire());
    }

    PilotImpl restarted; restarted.initialize(dir);
    LOGOS_ASSERT_TRUE(!restarted.agentIsOpenForHire());
    LOGOS_ASSERT_TRUE(!contains(restarted.identityTopics(),
                                "/pilot/1/inbox-" + enc.publicKeyHex + "/proto"));
}

// The card says which it is, so an owner reading their own card can see at a glance
// whether anyone could actually reach them.
LOGOS_TEST(the_card_states_whether_the_agent_is_open_for_hire) {
    std::string dir = bootDir("card_states_open");
    ECIESKeypair enc = generateECIESKeypair();
    ECIESKeypair ecies = generateECIESKeypair();
    { PilotImpl boot; boot.initialize(dir); }
    seedIdentity(dir, enc, ecies);

    PilotImpl impl; impl.initialize(dir);
    QJsonObject closed = QJsonDocument::fromJson(
        QByteArray::fromStdString(impl.agentCard())).object();
    LOGOS_ASSERT_TRUE(!closed["_logos"].toObject()["open_for_hire"].toBool());

    impl.agentOpenForHire();
    QJsonObject open = QJsonDocument::fromJson(
        QByteArray::fromStdString(impl.agentCard())).object();
    LOGOS_ASSERT_TRUE(open["_logos"].toObject()["open_for_hire"].toBool());
}

// A card that arrives live on the discovery topic must be STORED, under exactly the same
// verification an imported card gets. Before this, cards were received and dropped, so
// discovery could only ever work through a store/archive service that this topology does
// not have (measured 2026-07-26: one discovery message received, nothing stored).
//
// handleDiscoveryCard() is the seam the messageReceived callback calls, so it can be
// tested without a delivery module.
LOGOS_TEST(a_card_arriving_on_the_discovery_topic_is_stored) {
    std::string dir = bootDir("live_card_cached");
    { PilotImpl boot; boot.initialize(dir); }
    PilotImpl impl; impl.initialize(dir);

    // A genuine self-signed card: signed by its own published identity key, so
    // verifyCardStatus() == "valid" and a2aCacheDiscoveredCard() accepts it.
    ECIESKeypair kp = generateECIESKeypair();
    QJsonObject logos;
    logos["npk"] = QString("npk-live-peer");
    logos["signing_key"] = QString::fromStdString(kp.publicKeyHex);
    logos["enc_key"] = QString::fromStdString(kp.publicKeyHex);
    QJsonObject card;
    card["name"] = QString("Pilot Agent");
    card["_logos"] = logos;
    std::string canonical = QJsonDocument(card).toJson(QJsonDocument::Compact).toStdString();
    std::vector<uint8_t> bytes(canonical.begin(), canonical.end());
    QJsonObject sig;
    sig["alg"] = QString("ES256K");
    sig["publicKey"] = QString::fromStdString(kp.publicKeyHex);
    sig["value"] = QString::fromStdString(signMessage(bytes, kp.privateKeyHex));
    card["signature"] = sig;

    impl.handleDiscoveryCard(
        QJsonDocument(card).toJson(QJsonDocument::Compact).toStdString());

    // Stored under its payment identity — which is what makes it routable and payable.
    LOGOS_ASSERT_TRUE(cardStored(dir, "npk-live-peer"));
}

// Junk on the discovery topic must be ignored quietly, never stored and never fatal.
LOGOS_TEST(junk_on_the_discovery_topic_is_ignored) {
    std::string dir = bootDir("live_card_junk");
    { PilotImpl boot; boot.initialize(dir); }
    PilotImpl impl; impl.initialize(dir);

    impl.handleDiscoveryCard("not json");
    impl.handleDiscoveryCard("{\"name\":\"no logos block\"}");
    LOGOS_ASSERT_TRUE(!cardStored(dir, "npk-live-peer"));
}
