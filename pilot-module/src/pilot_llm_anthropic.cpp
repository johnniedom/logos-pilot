#include "pilot_llm.h"
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <cstdlib>

class AnthropicProvider : public LLMProvider {
public:
    AnthropicProvider(const std::string& apiKey, const std::string& modelId)
        : apiKey_(apiKey), modelId_(modelId) {}

    std::string complete(const std::string& systemPrompt,
                         const std::vector<LLMMessage>& messages) override {
        if (apiKey_.empty()) return "";

        QJsonArray jsonMessages;
        for (const auto& msg : messages) {
            QJsonObject m;
            m["role"] = QString::fromStdString(msg.role);
            m["content"] = QString::fromStdString(msg.content);
            jsonMessages.append(m);
        }

        QJsonObject body;
        body["model"] = QString::fromStdString(modelId_);
        body["max_tokens"] = 1024;
        body["messages"] = jsonMessages;
        if (!systemPrompt.empty()) {
            body["system"] = QString::fromStdString(systemPrompt);
        }

        QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

        QNetworkAccessManager manager;
        QNetworkRequest request(QUrl("https://api.anthropic.com/v1/messages"));
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        request.setRawHeader("x-api-key", QByteArray::fromStdString(apiKey_));
        request.setRawHeader("anthropic-version", "2023-06-01");

        QNetworkReply* reply = manager.post(request, payload);
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        if (reply->error() != QNetworkReply::NoError) {
            std::string err = reply->errorString().toStdString();
            reply->deleteLater();
            return "{\"error\": \"" + err + "\"}";
        }

        QByteArray responseData = reply->readAll();
        reply->deleteLater();

        QJsonDocument responseDoc = QJsonDocument::fromJson(responseData);
        if (responseDoc.isNull()) return "";

        QJsonObject responseObj = responseDoc.object();
        QJsonArray content = responseObj["content"].toArray();
        if (content.isEmpty()) return "";

        return content[0].toObject()["text"].toString().toStdString();
    }

    std::string model() const override { return modelId_; }
    std::string providerName() const override { return "anthropic"; }
    bool isConfigured() const override { return !apiKey_.empty(); }

private:
    std::string apiKey_;
    std::string modelId_;
};

std::unique_ptr<LLMProvider> createAnthropicProvider(const std::string& modelOverride) {
    const char* key = std::getenv("ANTHROPIC_API_KEY");
    if (!key) return nullptr;

    std::string modelId = modelOverride;
    if (modelId.empty()) {
        const char* envModel = std::getenv("PILOT_LLM_MODEL");
        modelId = envModel ? envModel : "claude-sonnet-4-6";
    }

    return std::make_unique<AnthropicProvider>(key, modelId);
}
