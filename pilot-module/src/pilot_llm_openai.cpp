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

std::string pilotExtractOpenAIText(const std::string& responseJson) {
    QJsonDocument responseDoc = QJsonDocument::fromJson(QByteArray::fromStdString(responseJson));
    if (responseDoc.isNull() || !responseDoc.isObject()) return "";

    QJsonArray choices = responseDoc.object()["choices"].toArray();
    if (choices.isEmpty()) return "";

    // DeepSeek adds "reasoning_content" beside "content"; only the visible answer is returned.
    return choices[0].toObject()["message"].toObject()["content"].toString().toStdString();
}

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
        body["messages"] = jsonMessages;
        // OpenAI's own endpoint retired "max_tokens" for its reasoning models in favour of
        // "max_completion_tokens"; the compatible endpoints (DeepSeek, Gemini, OpenRouter, Groq)
        // still take "max_tokens".
        const bool nativeOpenAI = baseUrl_.find("api.openai.com") != std::string::npos;
        body[nativeOpenAI ? "max_completion_tokens" : "max_tokens"] = pilotLlmMaxTokens();
        // DeepSeek's current models think by default (reasoning tokens count against the budget
        // and add seconds per reply); ask for a plain answer unless PILOT_LLM_THINKING=1.
        if (baseUrl_.find("api.deepseek.com") != std::string::npos && !pilotLlmThinkingEnabled()) {
            QJsonObject thinking;
            thinking["type"] = "disabled";
            body["thinking"] = thinking;
        }

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

        return pilotExtractOpenAIText(responseData.toStdString());
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
        modelId = (envModel && *envModel) ? envModel : pilotLlmDefaultModel("openai");
    }

    return std::make_unique<OpenAIProvider>(key, baseUrl, modelId);
}
