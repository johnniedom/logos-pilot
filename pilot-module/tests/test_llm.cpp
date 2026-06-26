#include <logos_test.h>
#include "../src/pilot_llm.h"
#include "../src/pilot_impl.h"
#include "../src/pilot_crypto.h"
#include <string>
#include <vector>
#include <memory>
#include <cstdlib>
#include <cstdio>
#include <QString>
#include <QByteArray>
#include <QJsonObject>
#include <QJsonDocument>

LOGOS_TEST(noop_provider_not_configured) {
    NoOpProvider noop;
    LOGOS_ASSERT_FALSE(noop.isConfigured());
    LOGOS_ASSERT_EQ(noop.model(), std::string("none"));
    LOGOS_ASSERT_EQ(noop.providerName(), std::string("none"));
}

LOGOS_TEST(noop_provider_returns_empty) {
    NoOpProvider noop;
    std::string result = noop.complete("system", {{"user", "hello"}});
    LOGOS_ASSERT_EQ(result, std::string(""));
}

LOGOS_TEST(factory_returns_noop_when_no_env) {
    auto provider = createLLMProvider("", "");
    LOGOS_ASSERT(provider != nullptr);
    LOGOS_ASSERT_FALSE(provider->isConfigured());
    LOGOS_ASSERT_EQ(provider->providerName(), std::string("none"));
}

LOGOS_TEST(factory_returns_noop_for_unknown_provider) {
    auto provider = createLLMProvider("nonexistent", "");
    LOGOS_ASSERT(provider != nullptr);
    LOGOS_ASSERT_FALSE(provider->isConfigured());
}

LOGOS_TEST(process_owner_message_slash_command_bypasses_llm) {
    PilotImpl impl;
    std::string result = impl.processOwnerMessage("/balance");
    LOGOS_ASSERT_CONTAINS(result, "command");
    LOGOS_ASSERT_CONTAINS(result, "/balance");
}

LOGOS_TEST(process_owner_message_empty_returns_none) {
    PilotImpl impl;
    std::string result = impl.processOwnerMessage("");
    LOGOS_ASSERT_CONTAINS(result, "none");
}

LOGOS_TEST(process_owner_message_freetext_without_llm) {
    PilotImpl impl;
    std::string result = impl.processOwnerMessage("what's my balance?");
    LOGOS_ASSERT_CONTAINS(result, "command-only mode");
}

LOGOS_TEST(meta_status_shows_llm_none_by_default) {
    PilotImpl impl;
    std::string status = impl.metaStatus();
    LOGOS_ASSERT_CONTAINS(status, "\"llm\":\"none\"");
}

// ===================== Wave 3: I1 re-entrant owner history alternation ===============

// I1: an LLM whose complete() RE-ENTERS processOwnerMessage with a second owner message while the
// first is parked in complete() (the nested-QEventLoop hazard). It records every history snapshot
// it is handed so the test can inspect the committed order. depth guard => exactly one re-entry.
class ReentrantOwnerLLM : public LLMProvider {
public:
    explicit ReentrantOwnerLLM(PilotImpl* impl) : impl_(impl) {}
    std::string complete(const std::string&, const std::vector<LLMMessage>& msgs) override {
        calls_.push_back(msgs);          // the history snapshot this call was handed
        if (depth_ == 0) {
            depth_ = 1;
            impl_->processOwnerMessage("NESTED");   // re-enter WHILE the outer is parked here
        }
        return "REPLY";
    }
    std::string model() const override { return "reentrant-owner"; }
    std::string providerName() const override { return "reentrant-owner"; }
    bool isConfigured() const override { return true; }
    std::vector<std::vector<LLMMessage>> calls_;
private:
    PilotImpl* impl_;
    int depth_ = 0;
};

// I1: committing the user turn only AFTER complete() keeps chatHistory_ alternating even when a
// re-entrant owner message interleaves during the blocking call. We OBSERVE the committed history
// via a PROBE turn (the snapshot the LLM receives reflects chatHistory_): no two adjacent turns
// share a role, and BOTH overlapping user turns survive. Pre-fix (user pushed before complete())
// the history reads [user OUTER, user NESTED, ...] — adjacent same-role — and this fails.
LOGOS_TEST(process_owner_message_reentrant_keeps_history_alternation) {
    PilotImpl impl;
    auto* llm = new ReentrantOwnerLLM(&impl);
    pilotSetLLMProvider(impl, std::unique_ptr<LLMProvider>(llm));

    impl.processOwnerMessage("OUTER");   // re-enters with "NESTED" during complete()
    impl.processOwnerMessage("PROBE");   // a clean turn whose snapshot reflects the full history

    const std::vector<LLMMessage>& hist = llm->calls_.back();   // committed history + PROBE

    // (1) Strict alternation: no two ADJACENT turns share a role.
    for (size_t i = 1; i < hist.size(); ++i)
        LOGOS_ASSERT_TRUE(hist[i].role != hist[i - 1].role);

    // (2) Both overlapping user turns are present in the committed history.
    bool sawOuter = false, sawNested = false;
    for (const auto& m : hist) {
        if (m.content == "OUTER") sawOuter = true;
        if (m.content == "NESTED") sawNested = true;
    }
    LOGOS_ASSERT_TRUE(sawOuter);
    LOGOS_ASSERT_TRUE(sawNested);
}

// ===================== Wave 3: L6 LLM request-timeout configuration ==================

// L6: pilotLlmTimeoutMs() defaults to 60000, honors a positive PILOT_LLM_TIMEOUT_MS override, and
// falls back to the default for non-positive / unparseable values.
LOGOS_TEST(llm_timeout_ms_defaults_and_env_override) {
    unsetenv("PILOT_LLM_TIMEOUT_MS");
    LOGOS_ASSERT_EQ(pilotLlmTimeoutMs(), 60000);     // default

    setenv("PILOT_LLM_TIMEOUT_MS", "1234", 1);
    LOGOS_ASSERT_EQ(pilotLlmTimeoutMs(), 1234);      // positive override honored

    setenv("PILOT_LLM_TIMEOUT_MS", "0", 1);
    LOGOS_ASSERT_EQ(pilotLlmTimeoutMs(), 60000);     // non-positive -> default

    setenv("PILOT_LLM_TIMEOUT_MS", "notanumber", 1);
    LOGOS_ASSERT_EQ(pilotLlmTimeoutMs(), 60000);     // unparseable -> default

    unsetenv("PILOT_LLM_TIMEOUT_MS");                // clean up for other tests
}

// ===================== M1 — owner-channel authentication (FAIL-OPEN) =================
//
// verifyOwnerMessage() must NEVER lock the owner out: raw/unsigned text is accepted as-is
// (the pilot-cli / pilot-ui clients still send raw text). A client that opts in to signing gets
// the hardened path — ECDSA signature over the canonical bytes, a TOFU pin of the owner signing
// key, and a strictly-monotonic replay nonce — and any failed check drops the message.

// A fresh data dir per test so a leftover pilot.db can't carry a stale owner pin / nonce in.
static std::string ownerAuthDir(const std::string& name) {
    std::string base = "/tmp";
    if (const char* t = std::getenv("TMPDIR")) base = t;
    std::string dir = base + "/pilot_owner_" + name;
    std::remove((dir + "/pilot.db").c_str());
    std::remove((dir + "/pilot.db-wal").c_str());
    std::remove((dir + "/pilot.db-shm").c_str());
    return dir;
}

// Build a SIGNED owner envelope exactly as verifyOwnerMessage expects, mirroring the production
// signA2AEnvelope / verifyInboundRequest canonical-bytes scheme: set _logos.signing_key + nonce,
// drop any prior _logos.signature, sign the Compact bytes (signature ABSENT), then attach the
// signature. verifyOwnerMessage reproduces these bytes (envelope minus _logos.signature) and
// verifies them against signing_key.
static std::string signOwnerEnvelope(const std::string& message, long long nonce,
                                     const ECIESKeypair& kp) {
    QJsonObject env;
    env["message"] = QString::fromStdString(message);
    QJsonObject logos;
    logos["signing_key"] = QString::fromStdString(kp.publicKeyHex);
    logos["nonce"] = static_cast<double>(nonce);   // JSON has one numeric type (Double)
    env["_logos"] = logos;
    std::string canonical = QJsonDocument(env).toJson(QJsonDocument::Compact).toStdString();
    std::vector<uint8_t> bytes(canonical.begin(), canonical.end());
    logos["signature"] = QString::fromStdString(signMessage(bytes, kp.privateKeyHex));
    env["_logos"] = logos;
    return QJsonDocument(env).toJson(QJsonDocument::Compact).toStdString();
}

// FAIL-OPEN: a legacy raw-text owner message (no envelope) is accepted unchanged so the owner is
// never locked out. No db / no signing key required for this path.
LOGOS_TEST(verify_owner_message_raw_text_accepted_fail_open) {
    PilotImpl impl;
    std::string inner;
    LOGOS_ASSERT_TRUE(pilotTestVerifyOwnerMessage(impl,"/approve x", inner));
    LOGOS_ASSERT_EQ(inner, std::string("/approve x"));

    // A JSON object that is NOT a signed envelope (no _logos.signature) is ALSO accepted as-is.
    std::string inner2;
    LOGOS_ASSERT_TRUE(pilotTestVerifyOwnerMessage(impl,"{\"message\":\"hi\"}", inner2));
    LOGOS_ASSERT_EQ(inner2, std::string("{\"message\":\"hi\"}"));
}

// A correctly SIGNED envelope is accepted, the inner message is unwrapped, the owner key is
// pinned, and a SECOND signed message from the SAME key with a HIGHER nonce also passes.
LOGOS_TEST(verify_owner_message_signed_envelope_accepted_and_pins) {
    std::string dir = ownerAuthDir("signed_ok");
    PilotImpl impl;
    impl.initialize(dir);   // opens db_ (the config table that holds owner.signing_key / last_nonce)
    ECIESKeypair owner = generateECIESKeypair();

    std::string env1 = signOwnerEnvelope("/approve abc", 1, owner);
    std::string inner;
    LOGOS_ASSERT_TRUE(pilotTestVerifyOwnerMessage(impl,env1, inner));   // pins owner key, last_nonce -> 1
    LOGOS_ASSERT_EQ(inner, std::string("/approve abc"));

    std::string env2 = signOwnerEnvelope("hello again", 2, owner);   // same key, higher nonce
    std::string inner2;
    LOGOS_ASSERT_TRUE(pilotTestVerifyOwnerMessage(impl,env2, inner2));
    LOGOS_ASSERT_EQ(inner2, std::string("hello again"));
}

// A signed envelope whose signature does NOT verify over the presented bytes is dropped. Here the
// inner `message` is swapped AFTER signing, so the attached signature no longer matches.
LOGOS_TEST(verify_owner_message_bad_signature_rejected) {
    std::string dir = ownerAuthDir("bad_sig");
    PilotImpl impl;
    impl.initialize(dir);
    ECIESKeypair owner = generateECIESKeypair();

    std::string env = signOwnerEnvelope("/approve x", 1, owner);
    QJsonObject obj = QJsonDocument::fromJson(QByteArray::fromStdString(env)).object();
    obj["message"] = QString("EVIL injected");   // signature was over the ORIGINAL message bytes
    std::string tampered = QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString();

    std::string inner;
    LOGOS_ASSERT_FALSE(pilotTestVerifyOwnerMessage(impl,tampered, inner));
}

// A signed envelope from a DIFFERENT key than the pinned owner key is dropped (TOFU mismatch),
// even though the attacker's signature is internally valid over its own bytes.
LOGOS_TEST(verify_owner_message_pin_mismatch_rejected) {
    std::string dir = ownerAuthDir("pin_mismatch");
    PilotImpl impl;
    impl.initialize(dir);
    ECIESKeypair owner    = generateECIESKeypair();
    ECIESKeypair attacker = generateECIESKeypair();

    std::string env1 = signOwnerEnvelope("legit", 1, owner);
    std::string inner;
    LOGOS_ASSERT_TRUE(pilotTestVerifyOwnerMessage(impl,env1, inner));   // pins the owner key

    std::string env2 = signOwnerEnvelope("takeover", 2, attacker);   // valid sig, WRONG key
    std::string inner2;
    LOGOS_ASSERT_FALSE(pilotTestVerifyOwnerMessage(impl,env2, inner2));
}

// Replay protection: an equal or lower nonce than the stored high-water mark is dropped.
LOGOS_TEST(verify_owner_message_replay_nonce_rejected) {
    std::string dir = ownerAuthDir("replay");
    PilotImpl impl;
    impl.initialize(dir);
    ECIESKeypair owner = generateECIESKeypair();

    std::string env5 = signOwnerEnvelope("first", 5, owner);
    std::string inner;
    LOGOS_ASSERT_TRUE(pilotTestVerifyOwnerMessage(impl,env5, inner));   // last_nonce -> 5

    std::string envEqual = signOwnerEnvelope("again", 5, owner);   // equal nonce -> replay
    std::string i2;
    LOGOS_ASSERT_FALSE(pilotTestVerifyOwnerMessage(impl,envEqual, i2));

    std::string envLower = signOwnerEnvelope("rollback", 3, owner);   // lower nonce -> replay
    std::string i3;
    LOGOS_ASSERT_FALSE(pilotTestVerifyOwnerMessage(impl,envLower, i3));
}

// A signed envelope with NO nonce is dropped (replay protection requires a monotonic nonce). The
// signature is otherwise valid and the key would pin, so this isolates the missing-nonce branch.
LOGOS_TEST(verify_owner_message_missing_nonce_rejected) {
    std::string dir = ownerAuthDir("no_nonce");
    PilotImpl impl;
    impl.initialize(dir);
    ECIESKeypair owner = generateECIESKeypair();

    QJsonObject env;
    env["message"] = QString("no nonce");
    QJsonObject logos;
    logos["signing_key"] = QString::fromStdString(owner.publicKeyHex);
    env["_logos"] = logos;   // deliberately NO nonce
    std::string canonical = QJsonDocument(env).toJson(QJsonDocument::Compact).toStdString();
    std::vector<uint8_t> bytes(canonical.begin(), canonical.end());
    logos["signature"] = QString::fromStdString(signMessage(bytes, owner.privateKeyHex));
    env["_logos"] = logos;
    std::string noNonce = QJsonDocument(env).toJson(QJsonDocument::Compact).toStdString();

    std::string inner;
    LOGOS_ASSERT_FALSE(pilotTestVerifyOwnerMessage(impl,noNonce, inner));
}
