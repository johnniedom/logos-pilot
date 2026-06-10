#include "pilot_impl.h"
#include "pilot_crypto.h"
#include "logos_api.h"
#include "logos_api_client.h"
#include "logos_mode.h"
#include <sqlite3.h>
#include <chrono>
#include <thread>
#include <QString>
#include <QVariant>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

static const Timeout OWNER_TIMEOUT(15000);

bool PilotImpl::establishOwnerChannel() {
    if (!logosAPI_ || agentNpk_.empty()) return false;
    initDeliveryModule();

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
    if (!logosAPI_ || ownerChannelId_.empty()) return false;

    auto accepted = [](const QVariant& v) {
        if (v.isNull()) return false;
        const QString s = v.toString();
        if (s.isEmpty()) return false;
        const QJsonDocument d = QJsonDocument::fromJson(s.toUtf8());
        if (d.isObject()) {
            const QJsonObject o = d.object();
            if (o.contains("success")) return o.value("success").toBool();
            if (o.contains("error") && !o.value("error").toString().isEmpty()) return false;
        }
        return true;   // non-empty, non-error reply (e.g. a requestId) -> accepted
    };

    const int kAttempts = 3;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        auto* delivery = logosAPI_->getClient("delivery_module");
        if (delivery && delivery->isConnected()) {
            QVariant r = delivery->invokeRemoteMethod(
                "delivery_module", "send",
                QString::fromStdString(ownerChannelId_),
                QString::fromStdString(payload), OWNER_TIMEOUT);
            if (accepted(r)) return true;
        }
        if (attempt + 1 < kAttempts)
            std::this_thread::sleep_for(std::chrono::milliseconds(250 * (attempt + 1)));
    }
    qWarning() << "[pilot] sendToOwner: delivery failed after" << kAttempts << "attempts";
    return false;
}

bool PilotImpl::sendToOwner(const std::string& message) {
    if (!logosAPI_ || ownerChannelId_.empty()) return false;

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
