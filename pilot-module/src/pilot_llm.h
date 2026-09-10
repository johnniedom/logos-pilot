#pragma once
#include <string>
#include <memory>
#include <vector>

struct LLMMessage {
    std::string role;
    std::string content;
};

class LLMProvider {
public:
    virtual ~LLMProvider() = default;
    virtual std::string complete(const std::string& systemPrompt,
                                 const std::vector<LLMMessage>& messages) = 0;
    virtual std::string model() const = 0;
    virtual std::string providerName() const = 0;
    virtual bool isConfigured() const = 0;
};

class NoOpProvider : public LLMProvider {
public:
    std::string complete(const std::string&, const std::vector<LLMMessage>&) override {
        return "";
    }
    std::string model() const override { return "none"; }
    std::string providerName() const override { return "none"; }
    bool isConfigured() const override { return false; }
};

std::unique_ptr<LLMProvider> createLLMProvider(const std::string& provider,
                                                const std::string& model = "");

// L6 — hard per-request timeout (ms) for the blocking LLM HTTP call. PILOT_LLM_TIMEOUT_MS
// (positive int) else 60000. External linkage so both providers and tests share it.
int pilotLlmTimeoutMs();

// Per-provider default model ids, checked against each provider's live list on 2026-09-10
// (DeepSeek: deepseek-v4-pro is routed to V4.1 Flash from 14 Sep 2026; deepseek-flash is the id).
// "" for an unknown provider.
std::string pilotLlmDefaultModel(const std::string& provider);

// Output budget of one completion: PILOT_LLM_MAX_TOKENS (positive int) else 4096. Reasoning
// models spend their thinking inside this budget, so the old 1024 could leave the visible answer
// empty (deepseek-v4-pro: reasoning_tokens == completion_tokens, content "").
int pilotLlmMaxTokens();

// PILOT_LLM_THINKING=1 keeps a provider's default thinking mode; otherwise the OpenAI-compatible
// client asks DeepSeek for a plain answer ("thinking": {"type": "disabled"}), which keeps owner and
// A2A replies short and fast (the daemon drops a module call past 20 s).
bool pilotLlmThinkingEnabled();

// Response parsers, exposed so tests can feed captured provider replies. Return the assistant's
// visible text or "" (no choices, no text block, malformed JSON).
std::string pilotExtractAnthropicText(const std::string& responseJson);
std::string pilotExtractOpenAIText(const std::string& responseJson);
