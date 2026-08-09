#include "pilot_impl.h"
#include "pilot_crypto.h"
// Generated per-build; typed client for delivery_module (see pilot_impl.h).
#include "logos_sdk.h"
#include <sqlite3.h>
#include <chrono>
#include <thread>
#include <QString>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

static constexpr int kOwnerTimeoutMs = 15000;

bool PilotImpl::establishOwnerChannel() {
    if (!isContextReady() || agentNpk_.empty()) return false;
    initDeliveryModule();

    std::string topic = "/pilot/1/owner-" + agentAccountId_ + "/proto";

    modules().delivery_module.subscribe(topic, nullptr, kOwnerTimeoutMs);

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

    // Best-effort greeting — now sent through the same honest, retrying path as
    // every other owner message (no silent swallow). Channel establishment itself
    // does not hinge on the greeting landing.
    if (!ownerNpk_.empty())
        sendToOwner("Pilot agent connected. Account: " + agentAccountId_);

    return true;
}

// Publish an already-prepared payload to the owner channel, retrying transient
// failures before giving up. Returns true ONLY if the delivery module actually
// accepted the message. NOTE: "accepted" means "handed to the network" — delivery
// is fire-and-forget with no read receipts, so this is the most we can honestly
// assert. We never claim the owner has seen it.
bool PilotImpl::deliverToOwner(const std::string& payload) {
    if (!isContextReady() || ownerChannelId_.empty()) return false;

    const int kAttempts = 3;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        // Acceptance is the typed result's success flag (the old code re-derived it by
        // parsing a JSON reply); the value carries the requestId, which we don't need.
        StdLogosResult r = modules().delivery_module.send(
            ownerChannelId_,
            std::vector<uint8_t>(payload.begin(), payload.end()),
            nullptr, kOwnerTimeoutMs);
        if (r.success) return true;
        if (attempt + 1 < kAttempts)
            std::this_thread::sleep_for(std::chrono::milliseconds(250 * (attempt + 1)));
    }
    qWarning() << "[pilot] sendToOwner: delivery failed after" << kAttempts << "attempts";
    return false;
}

bool PilotImpl::sendToOwner(const std::string& message) {
    if (!isContextReady() || ownerChannelId_.empty()) return false;

    // Prepare the payload ONCE. Encryption failure is deterministic — report it
    // immediately rather than retrying a broken operation.
    std::string payload;
    if (!ownerNpk_.empty()) {
        try {
            std::vector<uint8_t> plainBytes(message.begin(), message.end());
            ECIESCiphertext ct = eciesEncrypt(ownerNpk_, plainBytes);
            payload = eciesSerialize(ct);
        } catch (...) {
            qWarning() << "[pilot] sendToOwner: encryption failed; message not sent";
            return false;
        }
    } else {
        payload = message;   // no owner key yet -> plaintext channel
    }

    return deliverToOwner(payload);
}

std::string PilotImpl::getOwnerChannelId() {
    return ownerChannelId_;
}
