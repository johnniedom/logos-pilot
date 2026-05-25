#include "pilot_llm.h"
#include <cstdlib>

std::unique_ptr<LLMProvider> createAnthropicProvider(const std::string& modelOverride);
std::unique_ptr<LLMProvider> createOpenAIProvider(const std::string& modelOverride);

static void setupOpenAICompat(const char* baseUrl, const char* srcEnv) {
    setenv("OPENAI_BASE_URL", baseUrl, 1);
    if (srcEnv && std::getenv(srcEnv) && !std::getenv("OPENAI_API_KEY"))
        setenv("OPENAI_API_KEY", std::getenv(srcEnv), 1);
}

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

    if (p == "deepseek") {
        setupOpenAICompat("https://api.deepseek.com", "DEEPSEEK_API_KEY");
        std::string m = model.empty() ? "deepseek-chat" : model;
        auto llm = createOpenAIProvider(m);
        if (llm) return llm;
    }

    if (p == "google") {
        setupOpenAICompat("https://generativelanguage.googleapis.com/v1beta/openai", "GOOGLE_API_KEY");
        std::string m = model.empty() ? "gemini-2.5-flash" : model;
        auto llm = createOpenAIProvider(m);
        if (llm) return llm;
    }

    if (p == "openrouter") {
        setupOpenAICompat("https://openrouter.ai/api/v1", "OPENROUTER_API_KEY");
        std::string m = model.empty() ? "anthropic/claude-sonnet-4-6" : model;
        auto llm = createOpenAIProvider(m);
        if (llm) return llm;
    }

    if (p == "groq") {
        setupOpenAICompat("https://api.groq.com/openai/v1", "GROQ_API_KEY");
        std::string m = model.empty() ? "llama-3.3-70b-versatile" : model;
        auto llm = createOpenAIProvider(m);
        if (llm) return llm;
    }

    // Auto-detect
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
