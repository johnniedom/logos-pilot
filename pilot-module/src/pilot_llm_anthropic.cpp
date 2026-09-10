#include "pilot_llm.h"
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <cstdlib>

std::string pilotExtractAnthropicText(const std::string& responseJson) {
    QJsonDocument responseDoc = QJsonDocument::fromJson(QByteArray::fromStdString(responseJson));
    if (responseDoc.isNull() || !responseDoc.isObject()) return "";

    // Current Claude models think by default: the reply carries a "thinking" block (empty text
    // unless a summary was asked for) before the "text" block. Return the first text block.
    QJsonArray content = responseDoc.object()["content"].toArray();
    for (const auto& block : content) {
        QJsonObject obj = block.toObject();
        if (obj["type"].toString() == "text")
            return obj["text"].toString().toStdString();
    }
    return "";
}

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
        body["max_tokens"] = pilotLlmMaxTokens();
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
        // L6 — hard total deadline so this nested loop on the delivery thread can never hang.
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeout.start(pilotLlmTimeoutMs());
        loop.exec();

        if (!reply->isFinished()) {
            reply->abort();
            reply->deleteLater();
            QJsonObject errObj;
            errObj["error"] = QString("LLM request timed out");
            return QJsonDocument(errObj).toJson(QJsonDocument::Compact).toStdString();
        }

        if (reply->error() != QNetworkReply::NoError) {
            std::string err = reply->errorString().toStdString();
            reply->deleteLater();
            QJsonObject errObj;
            errObj["error"] = QString::fromStdString(err);
            return QJsonDocument(errObj).toJson(QJsonDocument::Compact).toStdString();
        }

        QByteArray responseData = reply->readAll();
        reply->deleteLater();

        return pilotExtractAnthropicText(responseData.toStdString());
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
        modelId = (envModel && *envModel) ? envModel : pilotLlmDefaultModel("anthropic");
    }

    return std::make_unique<AnthropicProvider>(key, modelId);
}
