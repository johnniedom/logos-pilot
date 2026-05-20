#include "pilot_llm.h"
#include <cstdlib>

std::unique_ptr<LLMProvider> createAnthropicProvider(const std::string& modelOverride);
std::unique_ptr<LLMProvider> createOpenAIProvider(const std::string& modelOverride);

std::unique_ptr<LLMProvider> createLLMProvider(const std::string& provider,
                                                const std::string& model) {
    std::string p = provider;
    if (p.empty()) {
        const char* env = std::getenv("PILOT_LLM_PROVIDER");
        p = env ? env : "";
    }

    if (p == "anthropic") {
        auto llm = createAnthropicProvider(model);
        if (llm) return llm;
    }

    if (p == "openai") {
        auto llm = createOpenAIProvider(model);
        if (llm) return llm;
    }

    // Auto-detect: try anthropic first, then openai
    if (p.empty()) {
        if (std::getenv("ANTHROPIC_API_KEY")) {
            auto llm = createAnthropicProvider(model);
            if (llm) return llm;
        }
        if (std::getenv("OPENAI_API_KEY")) {
            auto llm = createOpenAIProvider(model);
            if (llm) return llm;
        }
    }

    return std::make_unique<NoOpProvider>();
}
