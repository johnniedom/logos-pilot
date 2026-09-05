#include "pilot_impl.h"
#include "pilot_crypto.h"
// Generated per-build; typed client for delivery_module (see pilot_impl.h).
#include "logos_sdk.h"
#include <sqlite3.h>
#include <chrono>
#include <thread>
#include <vector>
#include <QString>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

static constexpr int kOwnerTimeoutMs = 15000;

bool PilotImpl::establishOwnerChannel() {
    if (!isContextReady() || agentNpk_.empty()) return false;
    initDeliveryModule();

    std::string topic = "/pilot/1/owner-" + agentAccountId_ + "/proto";

    modules().delivery_module.subscribe(topic, nullptr, kDeliveryFireAndForgetMs);

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
            nullptr, kDeliveryFireAndForgetMs);
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

namespace {
const char* kOwnerHelp =
    "Commands over the owner channel:\n"
    "  /balance                      wallet balances (private + public account)\n"
    "  /history                      the agent's spend ledger\n"
    "  /pending                      spend requests waiting for your approval\n"
    "  /approve <id> | /reject <id>  decide a held spend\n"
    "  /send <to> <amount> [reason]  spend through the spending FSM (held above your limits)\n"
    "  /status | /skills | /files | /inbox | /discover\n"
    "  /help";

std::vector<std::string> splitWords(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n') { if (!cur.empty()) { out.push_back(cur); cur.clear(); } }
        else cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
}  // namespace

std::string PilotImpl::ownerCommand(const std::string& actionJson) {
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(actionJson));
    if (!doc.isObject()) return actionJson;          // plain text already
    QJsonObject obj = doc.object();
    std::string action = obj["action"].toString().toStdString();
    QJsonObject params = obj["params"].toObject();

    // A slash command: turn "/approve abc" into action=approve, params.id=abc, and so on.
    if (action == "command") {
        std::vector<std::string> w = splitWords(params["raw"].toString().toStdString());
        if (w.empty() || w[0].empty() || w[0][0] != '/') return "unknown command\n" + std::string(kOwnerHelp);
        std::string cmd = w[0].substr(1);
        params = QJsonObject();
        if (cmd == "approve" || cmd == "reject") {
            if (w.size() < 2) return "usage: /" + cmd + " <spend request id>";
            params["id"] = QString::fromStdString(w[1]);
        } else if (cmd == "send") {
            if (w.size() < 3) return "usage: /send <to> <amount> [reason]";
            params["recipient"] = QString::fromStdString(w[1]);
            params["amount"] = QString::fromStdString(w[2]).toDouble();
            std::string reason;
            for (size_t i = 3; i < w.size(); ++i) reason += (i > 3 ? " " : "") + w[i];
            params["reason"] = QString::fromStdString(reason.empty() ? "owner request" : reason);
        }
        action = cmd;
    }

    if (action == "reply")    return params["text"].toString().toStdString();
    if (action == "help")     return kOwnerHelp;
    if (action == "balance")  return walletBalance();
    if (action == "history")  return walletHistory();
    if (action == "pending")  return getPendingSpends();
    if (action == "status")   return metaStatus();
    if (action == "skills")   return metaSkills();
    if (action == "files")    return storageList();
    if (action == "inbox")    return messagingInbox();
    if (action == "discover") return agentDiscover("");
    if (action == "approve") {
        std::string id = params["id"].toString().toStdString();
        return approveSpend(id) ? "approved " + id
                                : "could not approve " + id + " (unknown id, not held, or expired)";
    }
    if (action == "reject") {
        std::string id = params["id"].toString().toStdString();
        return rejectSpend(id) ? "rejected " + id
                               : "could not reject " + id + " (unknown id, not held, or expired)";
    }
    if (action == "send") {
        std::string to = params["recipient"].toString().toStdString();
        int64_t amount = static_cast<int64_t>(params["amount"].toDouble());
        std::string reason = params["reason"].toString().toStdString();
        if (to.empty() || amount <= 0) return "usage: /send <to> <amount> [reason]";
        return walletSend(to, amount, reason.empty() ? "owner request" : reason);
    }
    if (action == "upload")
        return storageUpload(params["path"].toString().toStdString(), params["label"].toString().toStdString());
    if (action == "download")
        return storageDownload(params["cid"].toString().toStdString(), params["path"].toString().toStdString());
    return "unknown action '" + action + "'\n" + std::string(kOwnerHelp);
}
