#include "pilot_llm.h"
#include <cstdlib>

std::unique_ptr<LLMProvider> createAnthropicProvider(const std::string& modelOverride);
std::unique_ptr<LLMProvider> createOpenAIProvider(const std::string& modelOverride);

int pilotLlmTimeoutMs() {
    if (const char* env = std::getenv("PILOT_LLM_TIMEOUT_MS")) {
        int v = std::atoi(env);
        if (v > 0) return v;
    }
    return 60000;
}

int pilotLlmMaxTokens() {
    if (const char* env = std::getenv("PILOT_LLM_MAX_TOKENS")) {
        int v = std::atoi(env);
        if (v > 0) return v;
    }
    return 4096;
}

bool pilotLlmThinkingEnabled() {
    const char* env = std::getenv("PILOT_LLM_THINKING");
    return env && std::string(env) == "1";
}

std::string pilotLlmDefaultModel(const std::string& provider) {
    if (provider == "anthropic")  return "claude-opus-5";
    if (provider == "openai")     return "gpt-5.6-terra";
    if (provider == "deepseek")   return "deepseek-flash";
    if (provider == "google")     return "gemini-3.8-flash";
    if (provider == "openrouter") return "anthropic/claude-opus-5";
    if (provider == "groq")       return "llama-3.3-70b-versatile";
    return "";
}

static void setupOpenAICompat(const char* baseUrl, const char* srcEnv) {
    setenv("OPENAI_BASE_URL", baseUrl, 1);
    if (srcEnv && std::getenv(srcEnv) && !std::getenv("OPENAI_API_KEY"))
        setenv("OPENAI_API_KEY", std::getenv(srcEnv), 1);
}

// The model for an OpenAI-compatible provider: the stored/explicit id, else PILOT_LLM_MODEL
// (a headless deploy names the model in the env), else the provider's default.
static std::string compatModel(const std::string& provider, const std::string& model) {
    if (!model.empty()) return model;
    if (const char* env = std::getenv("PILOT_LLM_MODEL"))
        if (*env) return env;
    return pilotLlmDefaultModel(provider);
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
        auto llm = createOpenAIProvider(compatModel(p, model));
        if (llm) return llm;
    }

    if (p == "google") {
        setupOpenAICompat("https://generativelanguage.googleapis.com/v1beta/openai", "GOOGLE_API_KEY");
        auto llm = createOpenAIProvider(compatModel(p, model));
        if (llm) return llm;
    }

    if (p == "openrouter") {
        setupOpenAICompat("https://openrouter.ai/api/v1", "OPENROUTER_API_KEY");
        auto llm = createOpenAIProvider(compatModel(p, model));
        if (llm) return llm;
    }

    if (p == "groq") {
        setupOpenAICompat("https://api.groq.com/openai/v1", "GROQ_API_KEY");
        auto llm = createOpenAIProvider(compatModel(p, model));
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
