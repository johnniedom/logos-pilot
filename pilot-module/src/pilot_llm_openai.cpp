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

class OpenAIProvider : public LLMProvider {
public:
    OpenAIProvider(const std::string& apiKey, const std::string& baseUrl,
                   const std::string& modelId)
        : apiKey_(apiKey), baseUrl_(baseUrl), modelId_(modelId) {}

    std::string complete(const std::string& systemPrompt,
                         const std::vector<LLMMessage>& messages) override {
        if (apiKey_.empty()) return "";

        QJsonArray jsonMessages;
        if (!systemPrompt.empty()) {
            QJsonObject sysMsg;
            sysMsg["role"] = "system";
            sysMsg["content"] = QString::fromStdString(systemPrompt);
            jsonMessages.append(sysMsg);
        }
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

        QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

        std::string url = baseUrl_ + "/chat/completions";
        QNetworkAccessManager manager;
        QNetworkRequest request(QUrl(QString::fromStdString(url)));
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        request.setRawHeader("Authorization",
            QByteArray::fromStdString("Bearer " + apiKey_));

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
        QJsonArray choices = responseObj["choices"].toArray();
        if (choices.isEmpty()) return "";

        return choices[0].toObject()["message"].toObject()["content"].toString().toStdString();
    }

    std::string model() const override { return modelId_; }
    std::string providerName() const override { return "openai"; }
    bool isConfigured() const override { return !apiKey_.empty(); }

private:
    std::string apiKey_;
    std::string baseUrl_;
    std::string modelId_;
};

std::unique_ptr<LLMProvider> createOpenAIProvider(const std::string& modelOverride) {
    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key) return nullptr;

    const char* baseUrlEnv = std::getenv("OPENAI_BASE_URL");
    std::string baseUrl = baseUrlEnv ? baseUrlEnv : "https://api.openai.com/v1";

    std::string modelId = modelOverride;
    if (modelId.empty()) {
        const char* envModel = std::getenv("PILOT_LLM_MODEL");
        modelId = envModel ? envModel : "gpt-4o";
    }

    return std::make_unique<OpenAIProvider>(key, baseUrl, modelId);
}
