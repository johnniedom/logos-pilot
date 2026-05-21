#include "pilot_impl.h"
#include "logos_api.h"
#include "logos_api_client.h"
#include <sqlite3.h>
#include <sstream>
#include <chrono>
#include <random>
#include <cstring>
#include <QString>
#include <QVariant>

static std::string amountToHexLE(int64_t amount) {
    uint8_t bytes[16] = {};
    for (int i = 0; i < 8 && amount > 0; i++) {
        bytes[i] = static_cast<uint8_t>(amount & 0xFF);
        amount >>= 8;
    }
    char hex[33];
    for (int i = 0; i < 16; i++)
        snprintf(hex + i * 2, 3, "%02x", bytes[i]);
    hex[32] = '\0';
    return std::string(hex);
}

static std::string generateId() {
    auto now = std::chrono::high_resolution_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    std::mt19937_64 rng(ns);
    std::ostringstream ss;
    ss << std::hex << rng();
    return ss.str();
}

static std::string currentTimestamp() {
    auto now = std::chrono::system_clock::now();
    return std::to_string(std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());
}

std::string PilotImpl::createSpendRequest(const std::string& recipient, int64_t amount, const std::string& reason) {
    if (!db_) return "{\"error\": \"not initialized\"}";

    std::string id = generateId();
    std::string now = currentTimestamp();
    int64_t expiresAt = std::stoll(now) + 3600;

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "INSERT INTO spend_requests (id, recipient, amount, reason, state, created_at, updated_at, expires_at) "
        "VALUES (?, ?, ?, ?, 'CREATED', ?, ?, ?);",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, recipient.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, amount);
    sqlite3_bind_text(stmt, 4, reason.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, now.c_str(), -1, SQLITE_TRANSIENT);
    std::string expStr = std::to_string(expiresAt);
    sqlite3_bind_text(stmt, 7, expStr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return id;
}

bool PilotImpl::approveSpend(const std::string& requestId) {
    if (!db_ || !logosAPI_) return false;

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT recipient, amount, state FROM spend_requests WHERE id = ?;",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, requestId.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return false;
    }

    std::string recipient = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    int64_t amount = sqlite3_column_int64(stmt, 1);
    std::string state = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    sqlite3_finalize(stmt);

    if (state != "HELD" && state != "NOTIFIED") return false;

    std::string now = currentTimestamp();
    sqlite3_prepare_v2(db_,
        "UPDATE spend_requests SET state = 'APPROVED', updated_at = ? WHERE id = ?;",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, requestId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(db_,
        "UPDATE spend_requests SET state = 'EXECUTING', updated_at = ? WHERE id = ?;",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, requestId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    auto* wallet = logosAPI_->getClient("lez_wallet_module");
    if (!wallet) return false;

    QVariant result = wallet->invokeRemoteMethod(
        "lez_wallet_module", "transfer_private",
        QString::fromStdString(agentAccountId_),
        QString::fromStdString(recipient),
        QString::fromStdString(amountToHexLE(amount)));

    QString resultStr = result.toString();
    bool ok = !result.isNull() && !resultStr.isEmpty() && !resultStr.contains("fail", Qt::CaseInsensitive);
    std::string finalState = ok ? "COMPLETED" : "TX_FAILED";
    now = currentTimestamp();
    sqlite3_prepare_v2(db_,
        "UPDATE spend_requests SET state = ?, updated_at = ? WHERE id = ?;",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, finalState.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, requestId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (ok) {
        sendToOwner("Transaction " + requestId + " completed: " + std::to_string(amount) + " LEZ sent to " + recipient);
    } else {
        sendToOwner("Transaction " + requestId + " FAILED");
    }

    return ok;
}

bool PilotImpl::rejectSpend(const std::string& requestId) {
    if (!db_) return false;

    std::string now = currentTimestamp();
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "UPDATE spend_requests SET state = 'REJECTED', updated_at = ? WHERE id = ? AND state IN ('HELD', 'NOTIFIED');",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, requestId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    bool changed = sqlite3_changes(db_) > 0;
    if (changed) {
        sendToOwner("Transaction " + requestId + " rejected.");
    }
    return changed;
}

std::string PilotImpl::getPendingSpends() {
    if (!db_) return "{\"error\": \"not initialized\"}";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT id, recipient, amount, reason, state, created_at, expires_at FROM spend_requests "
        "WHERE state IN ('CREATED', 'HELD', 'NOTIFIED') ORDER BY created_at;",
        -1, &stmt, nullptr);

    std::ostringstream json;
    json << "{\"pending\": [";
    bool first = true;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (!first) json << ",";
        first = false;
        json << "{"
             << "\"id\": \"" << sqlite3_column_text(stmt, 0) << "\","
             << "\"recipient\": \"" << sqlite3_column_text(stmt, 1) << "\","
             << "\"amount\": " << sqlite3_column_int64(stmt, 2) << ","
             << "\"reason\": \"" << sqlite3_column_text(stmt, 3) << "\","
             << "\"state\": \"" << sqlite3_column_text(stmt, 4) << "\","
             << "\"created_at\": \"" << sqlite3_column_text(stmt, 5) << "\","
             << "\"expires_at\": \"" << sqlite3_column_text(stmt, 6) << "\""
             << "}";
    }
    json << "]}";
    sqlite3_finalize(stmt);

    return json.str();
}

bool PilotImpl::setSpendingLimits(int64_t perTransaction, int64_t perPeriod, int64_t periodSeconds) {
    spendLimitPerTx_ = perTransaction;
    spendLimitPerPeriod_ = perPeriod;
    spendPeriodSeconds_ = periodSeconds;

    if (!db_) return false;

    auto upsert = [&](const char* key, int64_t val) {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_,
            "INSERT OR REPLACE INTO config (key, value) VALUES (?, ?);",
            -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
        std::string v = std::to_string(val);
        sqlite3_bind_text(stmt, 2, v.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    };

    upsert("spend_limit_per_tx", perTransaction);
    upsert("spend_limit_per_period", perPeriod);
    upsert("spend_period_seconds", periodSeconds);
    return true;
}

std::string PilotImpl::walletSend(const std::string& recipient, int64_t amount, const std::string& reason) {
    if (!logosAPI_ || agentAccountId_.empty())
        return "{\"error\": \"not initialized\"}";

    if (amount > spendLimitPerTx_) {
        std::string reqId = createSpendRequest(recipient, amount, reason);

        std::string now = currentTimestamp();
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_,
            "UPDATE spend_requests SET state = 'HELD', updated_at = ? WHERE id = ?;",
            -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, now.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, reqId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        std::string msg = "Approval needed:\nAmount: " + std::to_string(amount) +
            " LEZ\nTo: " + recipient + "\nReason: " + reason +
            "\nExpires: 60 min\n/approve " + reqId + "\n/reject " + reqId;
        sendToOwner(msg);

        now = currentTimestamp();
        sqlite3_prepare_v2(db_,
            "UPDATE spend_requests SET state = 'NOTIFIED', updated_at = ? WHERE id = ?;",
            -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, now.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, reqId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        return "{\"status\": \"held\", \"request_id\": \"" + reqId + "\", \"message\": \"Awaiting owner approval\"}";
    }

    std::string reqId = createSpendRequest(recipient, amount, reason);

    std::string now = currentTimestamp();
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "UPDATE spend_requests SET state = 'EXECUTING', updated_at = ? WHERE id = ?;",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, reqId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    auto* wallet = logosAPI_->getClient("lez_wallet_module");
    if (!wallet)
        return "{\"status\": \"failed\", \"request_id\": \"" + reqId + "\", \"error\": \"wallet unavailable\"}";

    QVariant result = wallet->invokeRemoteMethod(
        "lez_wallet_module", "transfer_private",
        QString::fromStdString(agentAccountId_),
        QString::fromStdString(recipient),
        QString::fromStdString(amountToHexLE(amount)));

    QString resultStr = result.toString();
    bool ok = !result.isNull() && !resultStr.isEmpty() && !resultStr.contains("fail", Qt::CaseInsensitive) && !resultStr.contains("error", Qt::CaseInsensitive);
    std::string finalState = ok ? "COMPLETED" : "TX_FAILED";
    now = currentTimestamp();
    sqlite3_prepare_v2(db_,
        "UPDATE spend_requests SET state = ?, updated_at = ? WHERE id = ?;",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, finalState.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, reqId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (ok)
        return "{\"status\": \"completed\", \"request_id\": \"" + reqId + "\"}";
    else
        return "{\"status\": \"failed\", \"request_id\": \"" + reqId + "\"}";
}

void PilotImpl::recoverPendingTransactions() {
    if (!db_ || !logosAPI_) return;

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT id, state, recipient, amount FROM spend_requests WHERE state NOT IN ('COMPLETED', 'REJECTED', 'TX_FAILED', 'EXPIRED');",
        -1, &stmt, nullptr);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        std::string state = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));

        if (state == "HELD" || state == "NOTIFIED") {
            int64_t amount = sqlite3_column_int64(stmt, 3);
            std::string recipient = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            sendToOwner("Pending approval (recovered):\nAmount: " + std::to_string(amount) +
                " LEZ\nTo: " + recipient + "\n/approve " + id + "\n/reject " + id);
        }
    }
    sqlite3_finalize(stmt);
}
