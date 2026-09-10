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

// ===================== 2026-09-10: current model ids, output budget, thinking =========

// Defaults follow each provider's live model list (DeepSeek retires deepseek-v4-pro on
// 14 Sep 2026 and routes it to V4.1 Flash; deepseek-chat is an alias, not a listed model).
LOGOS_TEST(llm_default_models_track_live_provider_lists) {
    LOGOS_ASSERT_EQ(pilotLlmDefaultModel("deepseek"),   std::string("deepseek-flash"));
    LOGOS_ASSERT_EQ(pilotLlmDefaultModel("anthropic"),  std::string("claude-opus-5"));
    LOGOS_ASSERT_EQ(pilotLlmDefaultModel("openai"),     std::string("gpt-5.6-terra"));
    LOGOS_ASSERT_EQ(pilotLlmDefaultModel("google"),     std::string("gemini-3.8-flash"));
    LOGOS_ASSERT_EQ(pilotLlmDefaultModel("openrouter"), std::string("anthropic/claude-opus-5"));
    LOGOS_ASSERT_EQ(pilotLlmDefaultModel("groq"),       std::string("llama-3.3-70b-versatile"));
    LOGOS_ASSERT_EQ(pilotLlmDefaultModel("nonexistent"), std::string(""));
}

// A compatible provider takes the stored id, else PILOT_LLM_MODEL, else its default — the old
// factory skipped PILOT_LLM_MODEL for every provider but anthropic/openai.
LOGOS_TEST(factory_compat_provider_resolves_model_from_env_then_default) {
    setenv("DEEPSEEK_API_KEY", "test-key", 1);
    unsetenv("OPENAI_API_KEY");

    setenv("PILOT_LLM_MODEL", "custom-from-env", 1);
    auto fromEnv = createLLMProvider("deepseek", "");
    LOGOS_ASSERT_TRUE(fromEnv->isConfigured());
    LOGOS_ASSERT_EQ(fromEnv->model(), std::string("custom-from-env"));

    auto stored = createLLMProvider("deepseek", "stored-id");
    LOGOS_ASSERT_EQ(stored->model(), std::string("stored-id"));   // stored id wins over env

    unsetenv("PILOT_LLM_MODEL");
    auto dflt = createLLMProvider("deepseek", "");
    LOGOS_ASSERT_EQ(dflt->model(), std::string("deepseek-flash"));

    unsetenv("DEEPSEEK_API_KEY");
    unsetenv("OPENAI_API_KEY");
    unsetenv("OPENAI_BASE_URL");
}

LOGOS_TEST(llm_max_tokens_defaults_and_env_override) {
    unsetenv("PILOT_LLM_MAX_TOKENS");
    LOGOS_ASSERT_EQ(pilotLlmMaxTokens(), 4096);
    setenv("PILOT_LLM_MAX_TOKENS", "512", 1);
    LOGOS_ASSERT_EQ(pilotLlmMaxTokens(), 512);
    setenv("PILOT_LLM_MAX_TOKENS", "-3", 1);
    LOGOS_ASSERT_EQ(pilotLlmMaxTokens(), 4096);
    unsetenv("PILOT_LLM_MAX_TOKENS");
}

LOGOS_TEST(llm_thinking_off_unless_env_says_one) {
    unsetenv("PILOT_LLM_THINKING");
    LOGOS_ASSERT_FALSE(pilotLlmThinkingEnabled());
    setenv("PILOT_LLM_THINKING", "1", 1);
    LOGOS_ASSERT_TRUE(pilotLlmThinkingEnabled());
    setenv("PILOT_LLM_THINKING", "yes", 1);
    LOGOS_ASSERT_FALSE(pilotLlmThinkingEnabled());
    unsetenv("PILOT_LLM_THINKING");
}

// Claude Opus 5 thinks by default and puts a "thinking" block (empty text) BEFORE the "text"
// block; the old parser read content[0] and returned "".
LOGOS_TEST(anthropic_text_skips_thinking_block) {
    const std::string withThinking =
        "{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\",\"content\":["
        "{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"abc\"},"
        "{\"type\":\"text\",\"text\":\"Balance is 149 LEZ.\"}],\"stop_reason\":\"end_turn\"}";
    LOGOS_ASSERT_EQ(pilotExtractAnthropicText(withThinking), std::string("Balance is 149 LEZ."));

    const std::string textOnly =
        "{\"content\":[{\"type\":\"text\",\"text\":\"ok\"}]}";
    LOGOS_ASSERT_EQ(pilotExtractAnthropicText(textOnly), std::string("ok"));

    LOGOS_ASSERT_EQ(pilotExtractAnthropicText("{\"content\":[]}"), std::string(""));
    LOGOS_ASSERT_EQ(pilotExtractAnthropicText("{\"content\":[{\"type\":\"thinking\",\"thinking\":\"\"}]}"),
                    std::string(""));
    LOGOS_ASSERT_EQ(pilotExtractAnthropicText("not json"), std::string(""));
}

// Captured DeepSeek replies (2026-09-10): the visible answer sits in "content" beside
// "reasoning_content"; a reply whose budget went entirely to reasoning has content "".
LOGOS_TEST(openai_text_reads_content_beside_reasoning) {
    const std::string deepseek =
        "{\"id\":\"x\",\"object\":\"chat.completion\",\"model\":\"deepseek-flash\",\"choices\":["
        "{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"ok\","
        "\"reasoning_content\":\"The user wants ok.\"},\"finish_reason\":\"stop\"}]}";
    LOGOS_ASSERT_EQ(pilotExtractOpenAIText(deepseek), std::string("ok"));

    const std::string budgetSpentOnReasoning =
        "{\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"\","
        "\"reasoning_content\":\"We need answer\"},\"finish_reason\":\"length\"}]}";
    LOGOS_ASSERT_EQ(pilotExtractOpenAIText(budgetSpentOnReasoning), std::string(""));

    LOGOS_ASSERT_EQ(pilotExtractOpenAIText("{\"choices\":[]}"), std::string(""));
    LOGOS_ASSERT_EQ(pilotExtractOpenAIText("[]"), std::string(""));
}
