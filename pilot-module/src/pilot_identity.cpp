#include "pilot_impl.h"
#include "pilot_skill.h"
#include "pilot_crypto.h"
#include "logos_api.h"
#include "logos_api_client.h"
#include <sqlite3.h>
#include <sstream>
#include <chrono>
#include <fstream>
#include <cstdlib>
#include <QString>
#include <QVariant>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

bool PilotImpl::initialize(const std::string& dataDir) {
    if (initialized_) return true;

    initDatabase(dataDir);
    initLLM();
    initDependencyModules();

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
                else if (key == "owner.npk") ownerNpk_ = val;
                else if (key == "ecies.pub") agentEciesPub_ = val;
                else if (key == "ecies.priv") agentEciesPriv_ = val;
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

bool PilotImpl::initWallet() {
    if (!logosAPI_) { qWarning() << "[pilot] initWallet: logosAPI_ is NULL"; return false; }

    auto* wallet = logosAPI_->getClient("lez_wallet_module");
    if (!wallet) return false;

    std::string configPath = dataDir_ + "/wallet_config.json";
    std::string storagePath = dataDir_ + "/wallet_storage";

    QVariant openResult = wallet->invokeRemoteMethod(
        "lez_wallet_module", "open",
        QString::fromStdString(configPath),
        QString::fromStdString(storagePath));
    if (openResult.toInt() == 0) return true;

    std::string sequencerAddr = "http://127.0.0.1:8080";
    if (const char* env = std::getenv("PILOT_SEQUENCER_ADDR"))
        sequencerAddr = env;

    std::ofstream configFile(configPath);
    if (configFile.is_open()) {
        QJsonObject walletCfg;
        walletCfg["sequencer_addr"] = QString::fromStdString(sequencerAddr);
        walletCfg["seq_poll_timeout"] = QString("30s");
        walletCfg["seq_tx_poll_max_blocks"] = 15;
        walletCfg["seq_poll_max_retries"] = 10;
        walletCfg["seq_block_poll_max_amount"] = 100;
        walletCfg["initial_accounts"] = QJsonArray();
        configFile << QJsonDocument(walletCfg).toJson(QJsonDocument::Indented).toStdString();
        configFile.close();
    }

    QVariant createResult = wallet->invokeRemoteMethod(
        "lez_wallet_module", "create_new",
        QString::fromStdString(configPath),
        QString::fromStdString(storagePath),
        QString("pilot_agent"));
    return createResult.toInt() == 0;
}

bool PilotImpl::createIdentity() {
    if (!logosAPI_) return false;

    if (!initWallet()) return false;

    auto* wallet = logosAPI_->getClient("lez_wallet_module");
    if (!wallet) return false;

    QVariant result = wallet->invokeRemoteMethod(
        "lez_wallet_module", "create_account_private");
    if (result.isNull() || result.toString().isEmpty()) return false;

    agentAccountId_ = result.toString().toStdString();

    QVariant keysResult = wallet->invokeRemoteMethod(
        "lez_wallet_module", "get_private_account_keys",
        QString::fromStdString(agentAccountId_));
    if (keysResult.isNull()) return false;

    agentNpk_ = keysResult.toString().toStdString();

    ECIESKeypair kp = generateECIESKeypair();
    agentEciesPub_ = kp.publicKeyHex;
    agentEciesPriv_ = kp.privateKeyHex;

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
    if (rc != SQLITE_DONE) return false;

    sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO config (key, value) VALUES ('ecies.pub', ?);",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, agentEciesPub_.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO config (key, value) VALUES ('ecies.priv', ?);",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, agentEciesPriv_.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return true;
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
        "lez_wallet_module", "get_balance",
        QString::fromStdString(agentAccountId_), QVariant(false));

    if (result.isNull())
        return "{\"error\": \"balance query failed\"}";

    QJsonObject obj;
    obj["balance"] = result.toString();
    obj["account"] = QString::fromStdString(agentAccountId_);
    return QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString();
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

    QJsonArray arr;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        QJsonObject tx;
        tx["id"] = QString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
        tx["recipient"] = QString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
        tx["amount"] = static_cast<double>(sqlite3_column_int64(stmt, 2));
        tx["state"] = QString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)));
        tx["created_at"] = QString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)));
        arr.append(tx);
    }
    sqlite3_finalize(stmt);

    QJsonObject root;
    root["transactions"] = arr;
    return QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString();
}
