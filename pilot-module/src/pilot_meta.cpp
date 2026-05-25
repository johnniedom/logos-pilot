#include "pilot_impl.h"
#include "pilot_llm.h"
#include "pilot_skill.h"
#include <sqlite3.h>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QByteArray>
#include <QString>

std::string PilotImpl::metaSkills() {
    if (registry_) return registry_->listSkills();
    QJsonObject root;
    root["skills"] = QJsonArray();
    root["count"] = 0;
    return QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString();
}

std::string PilotImpl::metaStatus() {
    std::string balance = walletBalance();
    std::string pending = getPendingSpends();

    QJsonObject root;
    root["initialized"] = initialized_;
    root["npk"] = QString::fromStdString(agentNpk_);
    root["account"] = QString::fromStdString(agentAccountId_);
    root["owner_channel"] = QString::fromStdString(ownerChannelId_);
    root["balance"] = QJsonDocument::fromJson(QByteArray::fromStdString(balance)).object();
    root["pending"] = QJsonDocument::fromJson(QByteArray::fromStdString(pending)).object();

    if (llm_ && llm_->isConfigured()) {
        QJsonObject llmObj;
        llmObj["provider"] = QString::fromStdString(llm_->providerName());
        llmObj["model"] = QString::fromStdString(llm_->model());
        root["llm"] = llmObj;
    } else {
        root["llm"] = QString("none");
    }

    return QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString();
}

bool PilotImpl::metaConfigure(const std::string& key, const std::string& value) {
    if (!db_) return false;

    if (key == "spending.per_transaction_limit") {
        spendLimitPerTx_ = std::stoll(value);
    } else if (key == "spending.per_period_limit") {
        spendLimitPerPeriod_ = std::stoll(value);
    } else if (key == "spending.period_seconds") {
        spendPeriodSeconds_ = std::stoll(value);
    } else if (key == "owner.npk") {
        ownerNpk_ = value;
    } else if (key == "owner.name") {
        ownerName_ = value;
        if (value.empty()) return true;
    } else if (key == "llm.provider") {
        llmProvider_ = value;
        initLLM();
    } else if (key == "llm.model") {
        llmModel_ = value;
        initLLM();
    } else if (key == "llm.api_key") {
        if (llmProvider_ == "anthropic")
            setenv("ANTHROPIC_API_KEY", value.c_str(), 1);
        else if (llmProvider_ == "openai")
            setenv("OPENAI_API_KEY", value.c_str(), 1);
        else if (llmProvider_ == "deepseek")
            setenv("DEEPSEEK_API_KEY", value.c_str(), 1);
        else if (llmProvider_ == "google")
            setenv("GOOGLE_API_KEY", value.c_str(), 1);
        else if (llmProvider_ == "openrouter")
            setenv("OPENROUTER_API_KEY", value.c_str(), 1);
        else if (llmProvider_ == "groq")
            setenv("GROQ_API_KEY", value.c_str(), 1);
        initLLM();
    }

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO config (key, value) VALUES (?, ?);",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return true;
}
