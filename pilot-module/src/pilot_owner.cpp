#include "pilot_impl.h"
#include "pilot_crypto.h"
#include "logos_api.h"
#include "logos_api_client.h"
#include "logos_mode.h"
#include <sqlite3.h>
#include <chrono>
#include <QString>
#include <QVariant>

static const Timeout OWNER_TIMEOUT(15000);

bool PilotImpl::establishOwnerChannel() {
    if (!logosAPI_ || agentNpk_.empty()) return false;

    auto* delivery = logosAPI_->getClient("delivery_module");
    if (!delivery || !delivery->isConnected()) return false;

    std::string topic = "/pilot/1/owner-" + agentAccountId_ + "/proto";

    delivery->invokeRemoteMethod(
        "delivery_module", "subscribe",
        QString::fromStdString(topic), OWNER_TIMEOUT);

    ownerChannelId_ = topic;

    if (db_) {
        auto now = std::chrono::system_clock::now();
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()).count();
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_,
            "INSERT OR REPLACE INTO owner_channel (id, conversation_id, established_at) VALUES (1, ?, ?);",
            -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, ownerChannelId_.c_str(), -1, SQLITE_TRANSIENT);
        std::string ts = std::to_string(secs);
        sqlite3_bind_text(stmt, 2, ts.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    if (!ownerNpk_.empty()) {
        try {
            std::string greeting = "Pilot agent connected. Account: " + agentAccountId_;
            std::vector<uint8_t> plainBytes(greeting.begin(), greeting.end());
            ECIESCiphertext ct = eciesEncrypt(ownerNpk_, plainBytes);
            std::string payload = eciesSerialize(ct);

            delivery->invokeRemoteMethod(
                "delivery_module", "send",
                QString::fromStdString(topic),
                QString::fromStdString(payload), OWNER_TIMEOUT);
        } catch (...) {}
    }

    return true;
}

bool PilotImpl::sendToOwner(const std::string& message) {
    if (!logosAPI_ || ownerChannelId_.empty()) return false;

    auto* delivery = logosAPI_->getClient("delivery_module");
    if (!delivery || !delivery->isConnected()) return false;

    if (!ownerNpk_.empty()) {
        try {
            std::vector<uint8_t> plainBytes(message.begin(), message.end());
            ECIESCiphertext ct = eciesEncrypt(ownerNpk_, plainBytes);
            std::string payload = eciesSerialize(ct);
            delivery->invokeRemoteMethod(
                "delivery_module", "send",
                QString::fromStdString(ownerChannelId_),
                QString::fromStdString(payload), OWNER_TIMEOUT);
        } catch (...) {}
    } else {
        delivery->invokeRemoteMethod(
            "delivery_module", "send",
            QString::fromStdString(ownerChannelId_),
            QString::fromStdString(message), OWNER_TIMEOUT);
    }
    return true;
}

std::string PilotImpl::getOwnerChannelId() {
    return ownerChannelId_;
}
