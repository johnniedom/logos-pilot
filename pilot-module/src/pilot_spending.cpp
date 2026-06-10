#include "pilot_impl.h"
#include "logos_api.h"
#include "logos_api_client.h"
#include <sqlite3.h>
#include <sstream>
#include <chrono>
#include <random>
#include <cstring>
#include <vector>
#include <QString>
#include <QVariant>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

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

// Execute a private (shielded) transfer, choosing the right wallet method by recipient form:
//   - recipient is a keys JSON ({nullifier_public_key, viewing_public_key}) -> transfer_private
//     (external payee whose public keys we were given, e.g. from an Agent Card)
//   - recipient is a plain 32-byte account id hex -> transfer_private_owned
//     (a private account this wallet owns)
// A bare account id cannot be shielded-paid without the payee's keys, so id form is
// only valid for owned recipients.
static QVariant doPrivateTransfer(LogosAPIClient* wallet, const std::string& fromId,
                                  const std::string& recipient, int64_t amount) {
    const bool hasKeys = recipient.find("nullifier_public_key") != std::string::npos
                      || recipient.find("viewing_public_key") != std::string::npos;
    const char* method = hasKeys ? "transfer_private" : "transfer_private_owned";
    return wallet->invokeRemoteMethod(
        "logos_execution_zone", method,
        QString::fromStdString(fromId),
        QString::fromStdString(recipient),
        QString::fromStdString(amountToHexLE(amount)), Timeout(120000));
}

// A transfer result is JSON: {"error":"...","success":bool,"tx_hash":"..."}.
// Parse the success flag explicitly — substring-matching is wrong because the
// "error" key matches even when error is empty, and a real error message may
// not contain the word "fail".
static bool transferSucceeded(const QVariant& result) {
    if (result.isNull()) return false;
    QJsonDocument d = QJsonDocument::fromJson(result.toString().toUtf8());
    return d.isObject() && d.object().value("success").toBool();
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
        "SELECT recipient, amount, state, expires_at FROM spend_requests WHERE id = ?;",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, requestId.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return false;
    }

    std::string recipient = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    int64_t amount = sqlite3_column_int64(stmt, 1);
    std::string state = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    std::string expiresAt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    sqlite3_finalize(stmt);

    if (state != "HELD" && state != "NOTIFIED") return false;

    int64_t nowEpoch = std::stoll(currentTimestamp());
    if (!expiresAt.empty() && nowEpoch > std::stoll(expiresAt)) {
        std::string ts = std::to_string(nowEpoch);
        sqlite3_prepare_v2(db_,
            "UPDATE spend_requests SET state = 'EXPIRED', updated_at = ? WHERE id = ?;",
            -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, requestId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        sendToOwner("Spend request " + requestId + " has expired.");
        return false;
    }

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

    auto* wallet = logosAPI_->getClient("logos_execution_zone");
    if (!wallet) return false;

    QVariant result = doPrivateTransfer(wallet, agentAccountId_, recipient, amount);

    bool ok = transferSucceeded(result);
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

// Proactively cancel any pending request whose 60-minute approval window has
// passed. Without this the deadline shown to the owner ("Expires: 60 min") is
// only enforced lazily at approve-time — a stale request would otherwise sit in
// the pending list forever and be re-announced on restart. Returns the count.
// SQL verified against real SQLite in /tmp/test_expiry.cpp (red->green).
int PilotImpl::expireStaleSpends() {
    if (!db_) return 0;
    long long now = std::stoll(currentTimestamp());

    // Only states still awaiting the owner can expire; APPROVED/EXECUTING/etc. are
    // mid-flight and COMPLETED/REJECTED are terminal.
    static const char* PENDING =
        "state IN ('CREATED','HELD','NOTIFIED') "
        "AND expires_at != '' AND CAST(expires_at AS INTEGER) < ?";

    std::vector<std::string> ids;
    std::string selSql = std::string("SELECT id FROM spend_requests WHERE ") + PENDING + ";";
    sqlite3_stmt* sel = nullptr;
    sqlite3_prepare_v2(db_, selSql.c_str(), -1, &sel, nullptr);
    sqlite3_bind_int64(sel, 1, now);
    while (sqlite3_step(sel) == SQLITE_ROW)
        ids.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(sel, 0)));
    sqlite3_finalize(sel);
    if (ids.empty()) return 0;

    std::string updSql =
        std::string("UPDATE spend_requests SET state='EXPIRED', updated_at=? WHERE ") + PENDING + ";";
    sqlite3_stmt* upd = nullptr;
    sqlite3_prepare_v2(db_, updSql.c_str(), -1, &upd, nullptr);
    std::string nowStr = std::to_string(now);
    sqlite3_bind_text(upd, 1, nowStr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(upd, 2, now);
    sqlite3_step(upd);
    sqlite3_finalize(upd);

    // Tell the owner once per cancelled request (best-effort; no-op if no channel).
    for (const auto& id : ids)
        sendToOwner("Spend request " + id + " expired before approval and was cancelled.");
    return static_cast<int>(ids.size());
}

std::string PilotImpl::getPendingSpends() {
    if (!db_) return "{\"error\": \"not initialized\"}";

    expireStaleSpends();   // never present a request that has already timed out

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT id, recipient, amount, reason, state, created_at, expires_at FROM spend_requests "
        "WHERE state IN ('CREATED', 'HELD', 'NOTIFIED') ORDER BY created_at;",
        -1, &stmt, nullptr);

    QJsonArray arr;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        QJsonObject obj;
        obj["id"] = QString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
        obj["recipient"] = QString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
        obj["amount"] = static_cast<double>(sqlite3_column_int64(stmt, 2));
        obj["reason"] = QString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)));
        obj["state"] = QString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)));
        obj["created_at"] = QString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5)));
        obj["expires_at"] = QString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6)));
        arr.append(obj);
    }
    sqlite3_finalize(stmt);

    QJsonObject root;
    root["pending"] = arr;
    return QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString();
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

    // Check per-period spending limit
    if (db_) {
        int64_t periodStart = std::stoll(currentTimestamp()) - spendPeriodSeconds_;
        std::string psStr = std::to_string(periodStart);
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_,
            "SELECT COALESCE(SUM(amount), 0) FROM spend_requests "
            "WHERE state IN ('COMPLETED', 'EXECUTING', 'APPROVED') AND created_at > ?;",
            -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, psStr.c_str(), -1, SQLITE_TRANSIENT);
        int64_t periodTotal = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW)
            periodTotal = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);

        if (periodTotal + amount > spendLimitPerPeriod_) {
            std::string reqId = createSpendRequest(recipient, amount, reason);
            sendToOwner("Period limit exceeded (" + std::to_string(periodTotal) + "/" +
                std::to_string(spendLimitPerPeriod_) + " LEZ). Approval required.\n/approve " +
                reqId + "\n/reject " + reqId);
            QJsonObject res;
            res["status"] = QString("held");
            res["request_id"] = QString::fromStdString(reqId);
            res["message"] = QString("Period spending limit exceeded");
            return QJsonDocument(res).toJson(QJsonDocument::Compact).toStdString();
        }
    }

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

        QJsonObject res;
        res["status"] = QString("held");
        res["request_id"] = QString::fromStdString(reqId);
        res["message"] = QString("Awaiting owner approval");
        return QJsonDocument(res).toJson(QJsonDocument::Compact).toStdString();
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

    auto* wallet = logosAPI_->getClient("logos_execution_zone");
    if (!wallet) {
        QJsonObject res;
        res["status"] = QString("failed");
        res["request_id"] = QString::fromStdString(reqId);
        res["error"] = QString("wallet unavailable");
        return QJsonDocument(res).toJson(QJsonDocument::Compact).toStdString();
    }

    QVariant result = doPrivateTransfer(wallet, agentAccountId_, recipient, amount);

    bool ok = transferSucceeded(result);
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

    QJsonObject res;
    res["status"] = ok ? QString("completed") : QString("failed");
    res["request_id"] = QString::fromStdString(reqId);
    return QJsonDocument(res).toJson(QJsonDocument::Compact).toStdString();
}

void PilotImpl::recoverPendingTransactions() {
    if (!db_ || !logosAPI_) return;

    expireStaleSpends();   // don't re-announce requests that already timed out while we were down

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
