#include "pilot_impl.h"
#include "logos_api.h"
#include "logos_api_client.h"
#include "logos_object.h"
#include <sqlite3.h>
#include <chrono>
#include <QString>
#include <QVariant>
#include <QByteArray>

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
    if (!logosAPI_) return false;

    auto* chat = logosAPI_->getClient("chat_module");
    if (!chat) return false;

    LogosObject* chatObj = chat->requestObject("chat_module");
    if (!chatObj) return false;

    chat->onEvent(chatObj, "chatCreateIntroBundleResult",
        [this, chat, chatObj](const QString&, const QVariantList& data) {
            if (data.isEmpty()) return;
            QString introBundle = data[0].toString();
            if (introBundle.isEmpty()) return;

            std::string greeting = toHex("Pilot connected.");
            chat->onEventResponse(chatObj, "chatNewPrivateConversation",
                {introBundle, QString::fromStdString(greeting)});
        });

    chat->onEvent(chatObj, "chatNewConversation",
        [this](const QString&, const QVariantList& data) {
            if (data.isEmpty()) return;
            QString convoJson = data[0].toString();
            int start = convoJson.indexOf("\"conversationId\":\"");
            if (start < 0) return;
            start += 18;
            int end = convoJson.indexOf("\"", start);
            if (end < 0) return;
            ownerChannelId_ = convoJson.mid(start, end - start).toStdString();

            if (db_ && !ownerChannelId_.empty()) {
                auto now = std::chrono::system_clock::now();
                auto secs = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
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
        });

    chat->onEventResponse(chatObj, "chatCreateIntroBundle", {});
    return true;
}

bool PilotImpl::sendToOwner(const std::string& message) {
    if (!logosAPI_ || ownerChannelId_.empty()) return false;

    auto* chat = logosAPI_->getClient("chat_module");
    if (!chat) return false;

    LogosObject* chatObj = chat->requestObject("chat_module");
    if (!chatObj) return false;

    std::string contentHex = toHex(message);
    chat->onEventResponse(chatObj, "chatSendMessage",
        {QString::fromStdString(ownerChannelId_),
         QString::fromStdString(contentHex)});
    return true;
}

std::string PilotImpl::getOwnerChannelId() {
    return ownerChannelId_;
}
