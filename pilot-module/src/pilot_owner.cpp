#include "pilot_impl.h"
#include "logos_api.h"
#include "logos_api_client.h"
#include <sqlite3.h>
#include <chrono>
#include <QString>
#include <QVariant>

static std::string toHex(const std::string& input) {
    std::string hex;
    hex.reserve(input.size() * 2);
    for (unsigned char c : input) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", c);
        hex += buf;
    }
    return hex;
}

bool PilotImpl::establishOwnerChannel() {
    if (!logosAPI_ || agentNpk_.empty()) return false;

    auto* delivery = logosAPI_->getClient("delivery_module");
    if (!delivery) return false;

    std::string topic = "/pilot/1/owner-" + agentNpk_ + "/proto";

    QVariant subResult = delivery->invokeRemoteMethod(
        "delivery_module", "subscribe",
        QString::fromStdString(topic));

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

    std::string greeting = toHex("Pilot agent connected. NPK: " + agentNpk_);
    delivery->invokeRemoteMethod(
        "delivery_module", "send",
        QString::fromStdString(topic),
        QString::fromStdString(greeting));

    return true;
}

bool PilotImpl::sendToOwner(const std::string& message) {
    if (!logosAPI_ || ownerChannelId_.empty()) return false;

    auto* delivery = logosAPI_->getClient("delivery_module");
    if (!delivery) return false;

    std::string payload = toHex(message);
    delivery->invokeRemoteMethod(
        "delivery_module", "send",
        QString::fromStdString(ownerChannelId_),
        QString::fromStdString(payload));
    return true;
}

std::string PilotImpl::getOwnerChannelId() {
    return ownerChannelId_;
}
