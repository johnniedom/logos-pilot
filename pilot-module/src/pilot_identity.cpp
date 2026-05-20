#include "pilot_impl.h"
#include "pilot_skill.h"
#include "logos_api.h"
#include "logos_api_client.h"
#include <sqlite3.h>
#include <sstream>
#include <chrono>
#include <QString>
#include <QVariant>

bool PilotImpl::initialize(const std::string& dataDir) {
    if (initialized_) return true;

    initDatabase(dataDir);
    initLLM();

    if (loadIdentity()) {
        initialized_ = true;
        recoverPendingTransactions();
        return true;
    }

    if (createIdentity()) {
        initialized_ = true;
        return true;
    }

    return false;
}

bool PilotImpl::isInitialized() {
    return initialized_;
}

bool PilotImpl::loadIdentity() {
    if (!db_) return false;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT npk, account_id FROM agent_identity WHERE id = 1;",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        agentNpk_ = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        agentAccountId_ = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        found = true;
    }
    sqlite3_finalize(stmt);

    if (found) {
        rc = sqlite3_prepare_v2(db_,
            "SELECT conversation_id FROM owner_channel WHERE id = 1;",
            -1, &stmt, nullptr);
        if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
            ownerChannelId_ = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        }
        if (stmt) sqlite3_finalize(stmt);

        rc = sqlite3_prepare_v2(db_,
            "SELECT key, value FROM config;",
            -1, &stmt, nullptr);
        if (rc == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                std::string key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                std::string val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                if (key == "spend_limit_per_tx") spendLimitPerTx_ = std::stoll(val);
                else if (key == "spend_limit_per_period") spendLimitPerPeriod_ = std::stoll(val);
                else if (key == "spend_period_seconds") spendPeriodSeconds_ = std::stoll(val);
                else if (key == "llm.provider") llmProvider_ = val;
                else if (key == "llm.model") llmModel_ = val;
            }
        }
        if (stmt) sqlite3_finalize(stmt);

        if (!llmProvider_.empty() || !llmModel_.empty())
            initLLM();
    }

    return found;
}

bool PilotImpl::createIdentity() {
    if (!logosAPI_) return false;

    auto* wallet = logosAPI_->getClient("lez_wallet_module");
    if (!wallet) return false;

    QVariant result = wallet->invokeRemoteMethod(
        "lez_wallet_module", "createAccountPrivate");
    if (result.isNull()) return false;

    agentAccountId_ = result.toString().toStdString();

    QVariant keysResult = wallet->invokeRemoteMethod(
        "lez_wallet_module", "getPrivateAccountKeys",
        QString::fromStdString(agentAccountId_));
    if (keysResult.isNull()) return false;

    agentNpk_ = keysResult.toString().toStdString();

    auto now = std::chrono::system_clock::now();
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    std::string timestamp = std::to_string(seconds);

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO agent_identity (id, npk, account_id, created_at) VALUES (1, ?, ?, ?);",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, agentNpk_.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, agentAccountId_.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, timestamp.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;
}

std::string PilotImpl::getAgentNpk() {
    return agentNpk_;
}

std::string PilotImpl::getAccountId() {
    return agentAccountId_;
}

std::string PilotImpl::walletBalance() {
    if (!logosAPI_ || agentAccountId_.empty()) return "{\"error\": \"not initialized\"}";

    auto* wallet = logosAPI_->getClient("lez_wallet_module");
    if (!wallet) return "{\"error\": \"wallet module unavailable\"}";

    QVariant result = wallet->invokeRemoteMethod(
        "lez_wallet_module", "getBalance",
        QString::fromStdString(agentAccountId_), QVariant(false));

    if (result.isNull())
        return "{\"error\": \"balance query failed\"}";

    return "{\"balance\": \"" + result.toString().toStdString() + "\", \"account\": \"" + agentAccountId_ + "\"}";
}

std::string PilotImpl::walletHistory() {
    if (!logosAPI_ || agentAccountId_.empty()) return "{\"error\": \"not initialized\"}";
    if (!db_) return "{\"error\": \"database not open\"}";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT id, recipient, amount, state, created_at FROM spend_requests "
        "ORDER BY created_at DESC LIMIT 50;",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return "{\"error\": \"query failed\"}";

    std::ostringstream json;
    json << "{\"transactions\": [";
    bool first = true;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (!first) json << ",";
        first = false;
        json << "{"
             << "\"id\": \"" << sqlite3_column_text(stmt, 0) << "\","
             << "\"recipient\": \"" << sqlite3_column_text(stmt, 1) << "\","
             << "\"amount\": " << sqlite3_column_int64(stmt, 2) << ","
             << "\"state\": \"" << sqlite3_column_text(stmt, 3) << "\","
             << "\"created_at\": \"" << sqlite3_column_text(stmt, 4) << "\""
             << "}";
    }
    json << "]}";
    sqlite3_finalize(stmt);

    return json.str();
}
