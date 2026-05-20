#include "pilot_impl.h"
#include "pilot_llm.h"
#include "pilot_skill.h"
#include <sqlite3.h>

std::string PilotImpl::metaSkills() {
    if (registry_) return registry_->listSkills();
    return "{\"skills\": [], \"count\": 0}";
}

std::string PilotImpl::metaStatus() {
    std::string balance = walletBalance();
    std::string pending = getPendingSpends();

    std::string llmInfo = "\"none\"";
    if (llm_ && llm_->isConfigured()) {
        llmInfo = "{\"provider\": \"" + llm_->providerName() +
                  "\", \"model\": \"" + llm_->model() + "\"}";
    }

    return "{\"initialized\": " + std::string(initialized_ ? "true" : "false") +
        ", \"npk\": \"" + agentNpk_ + "\""
        ", \"account\": \"" + agentAccountId_ + "\""
        ", \"owner_channel\": \"" + ownerChannelId_ + "\""
        ", \"balance\": " + balance +
        ", \"pending\": " + pending +
        ", \"llm\": " + llmInfo +
        "}";
}

bool PilotImpl::metaConfigure(const std::string& key, const std::string& value) {
    if (!db_) return false;

    if (key == "spending.per_transaction_limit") {
        spendLimitPerTx_ = std::stoll(value);
    } else if (key == "spending.per_period_limit") {
        spendLimitPerPeriod_ = std::stoll(value);
    } else if (key == "spending.period_seconds") {
        spendPeriodSeconds_ = std::stoll(value);
    } else if (key == "llm.provider") {
        llmProvider_ = value;
        initLLM();
    } else if (key == "llm.model") {
        llmModel_ = value;
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
