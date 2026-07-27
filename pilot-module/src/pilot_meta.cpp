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
    root["owner_name"] = QString::fromStdString(ownerName_);
    root["balance"] = QJsonDocument::fromJson(QByteArray::fromStdString(balance)).object();
    root["pending"] = QJsonDocument::fromJson(QByteArray::fromStdString(pending)).object();

    // Funding state, and WHY if it failed. An agent that could not fund itself used to look
    // identical to one that simply had no money yet, and the reason existed only as a
    // qWarning that never reaches the daemon log (measured 2026-07-27: zero [pilot] lines in
    // a 550KB log). Reading it back here means a funding failure can be answered by asking
    // the agent, with no log at all.
    if (db_) {
        QJsonObject funding;
        funding["funded"] = false;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_,
                "SELECT key, value FROM config WHERE key IN ('funded','funding.last_error');",
                -1, &st, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                std::string k = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
                std::string v = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
                if (k == "funded") funding["funded"] = (v == "1");
                else funding["last_error"] = QString::fromStdString(v);
            }
            sqlite3_finalize(st);
        }
        root["funding"] = funding;
    }

    // Whether strangers can hire this agent right now, and where it is actually listening.
    // subscribedTopics() is what delivery_module CONFIRMED, not what we intended — the two
    // disagreeing is the unhireable-agent bug.
    QJsonObject hire;
    hire["open_for_hire"] = openForHire_;
    QJsonArray subs;
    for (const std::string& t : subscribedTopics()) subs.append(QString::fromStdString(t));
    hire["listening_on"] = subs;
    root["hire"] = hire;

    // Real spending limits (the CLI must render these, not a hardcoded 100/500 — they
    // change with `pilot configure spend.*` and enforcement uses these exact fields).
    QJsonObject limits;
    limits["per_tx"] = static_cast<double>(spendLimitPerTx_);
    limits["per_period"] = static_cast<double>(spendLimitPerPeriod_);
    limits["period_seconds"] = static_cast<double>(spendPeriodSeconds_);
    root["limits"] = limits;

    if (llm_ && llm_->isConfigured()) {
        QJsonObject llmObj;
        llmObj["provider"] = QString::fromStdString(llm_->providerName());
        llmObj["model"] = QString::fromStdString(llm_->model());
        root["llm"] = llmObj;
    } else {
        root["llm"] = QString("none");
    }

    // Storage usage (spec: meta.status reports balance, storage usage, active tasks).
    QJsonObject storageObj;
    storageObj["files"] = 0;
    storageObj["bytes"] = 0;
    if (db_) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_,
                "SELECT COUNT(*), COALESCE(SUM(size_bytes), 0) FROM stored_files;",
                -1, &st, nullptr) == SQLITE_OK &&
            sqlite3_step(st) == SQLITE_ROW) {
            storageObj["files"] = static_cast<double>(sqlite3_column_int64(st, 0));
            storageObj["bytes"] = static_cast<double>(sqlite3_column_int64(st, 1));
        }
        if (st) sqlite3_finalize(st);
    }
    root["storage"] = storageObj;

    // Active inbound A2A tasks: not yet terminal (completed/failed/canceled).
    int64_t activeTasks = 0;
    if (db_) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_,
                "SELECT COUNT(*) FROM inbound_tasks "
                "WHERE state IN ('accepted', 'working', 'input-required');",
                -1, &st, nullptr) == SQLITE_OK &&
            sqlite3_step(st) == SQLITE_ROW) {
            activeTasks = sqlite3_column_int64(st, 0);
        }
        if (st) sqlite3_finalize(st);
    }
    root["active_tasks"] = static_cast<double>(activeTasks);

    return QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString();
}

bool PilotImpl::metaConfigure(const std::string& key, const std::string& value) {
    if (!db_) return false;

    // Persist under the SAME key names loadIdentity() reads on startup, or the setting
    // silently reverts to its default after a restart. The public/CLI keys are dotted
    // ("spending.per_transaction_limit"); the on-disk + loader keys are underscored.
    std::string storeKey = key;

    // Parse without throwing out of the Qt slot: a non-numeric value (e.g. a fat-finger
    // "abc" over the CLI) must fail the configure call, not std::terminate the module.
    auto parseLL = [](const std::string& v, bool& ok) -> long long {
        try {
            size_t pos = 0;
            long long n = std::stoll(v, &pos);
            ok = (pos == v.size());   // reject trailing junk like "12abc"
            return n;
        } catch (...) { ok = false; return 0; }
    };

    if (key == "spending.per_transaction_limit") {
        bool ok; long long n = parseLL(value, ok);
        if (!ok) return false;
        spendLimitPerTx_ = n;
        storeKey = "spend_limit_per_tx";
    } else if (key == "spending.per_period_limit") {
        bool ok; long long n = parseLL(value, ok);
        if (!ok) return false;
        spendLimitPerPeriod_ = n;
        storeKey = "spend_limit_per_period";
    } else if (key == "spending.period_seconds") {
        bool ok; long long n = parseLL(value, ok);
        if (!ok) return false;
        spendPeriodSeconds_ = n;
        storeKey = "spend_period_seconds";
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
    sqlite3_bind_text(stmt, 1, storeKey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return true;
}
