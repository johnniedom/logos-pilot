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
