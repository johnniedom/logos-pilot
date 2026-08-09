#include "pilot_impl.h"
#include "pilot_crypto.h"
// Generated per-build; typed client for delivery_module (see pilot_impl.h).
#include "logos_sdk.h"
#include <sstream>
#include <random>
#include <QString>
#include <QVariant>
#include <QJsonDocument>
#include <QJsonObject>

static std::string genGroupId() {
    std::random_device rd;
    std::mt19937_64 rng(rd());
    std::ostringstream ss;
    ss << std::hex << rng();
    return ss.str();
}

std::string PilotImpl::messagingSend(const std::string& recipient, const std::string& message) {
    if (!isContextReady()) return "{\"error\": \"not initialized\"}";
    initDeliveryModule();

    std::string recipientKey = recipient;
    QJsonDocument recipientDoc = QJsonDocument::fromJson(QByteArray::fromStdString(recipient));
    if (recipientDoc.isObject() && recipientDoc.object().contains("viewing_public_key"))
        recipientKey = recipientDoc.object()["viewing_public_key"].toString().toStdString();

    QJsonObject payload;
    payload["from"] = QString::fromStdString(agentNpk_);
    payload["message"] = QString::fromStdString(message);
    std::string payloadStr = QJsonDocument(payload).toJson(QJsonDocument::Compact).toStdString();
    std::vector<uint8_t> plainBytes(payloadStr.begin(), payloadStr.end());

    std::string encPayload;
    try {
        ECIESCiphertext encrypted = eciesEncrypt(recipientKey, plainBytes);
        encPayload = eciesSerialize(encrypted);
    } catch (const std::exception& e) {
        QJsonObject err;
        err["error"] = QString::fromStdString(std::string("encryption failed: ") + e.what());
        return QJsonDocument(err).toJson(QJsonDocument::Compact).toStdString();
    }

    std::string topic = "/pilot/1/inbox-" + recipientKey + "/proto";

    modules().delivery_module.send(
        topic, std::vector<uint8_t>(encPayload.begin(), encPayload.end()));

    QJsonObject result;
    result["sent"] = true;
    result["recipient"] = QString::fromStdString(recipient);
    result["encrypted"] = true;
    return QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString();
}

bool PilotImpl::messagingJoin(const std::string& groupId) {
    if (!isContextReady()) return false;

    std::string topic = "/pilot/1/group-" + groupId + "/proto";

    return modules().delivery_module.subscribe(topic).success;
}

std::string PilotImpl::messagingCreateGroup(const std::string& membersJson) {
    if (!isContextReady()) return "{\"error\": \"not initialized\"}";

    std::string groupId = genGroupId();
    std::string topic = "/pilot/1/group-" + groupId + "/proto";

    if (!modules().delivery_module.subscribe(topic).success)
        return "{\"error\": \"failed to create group topic\"}";

    QJsonObject invite;
    invite["type"] = QString("group_invite");
    invite["group_id"] = QString::fromStdString(groupId);
    invite["topic"] = QString::fromStdString(topic);
    invite["from"] = QString::fromStdString(agentNpk_);
    std::string inviteStr = QJsonDocument(invite).toJson(QJsonDocument::Compact).toStdString();

    std::string members = membersJson;
    if (!members.empty() && members.front() == '[') members = members.substr(1);
    if (!members.empty() && members.back() == ']') members.pop_back();

    std::istringstream stream(members);
    std::string member;
    while (std::getline(stream, member, ',')) {
        size_t start = member.find_first_not_of(" \"");
        size_t end = member.find_last_not_of(" \"");
        if (start == std::string::npos) continue;
        member = member.substr(start, end - start + 1);

        std::string memberTopic = "/pilot/1/inbox-" + member + "/proto";
        modules().delivery_module.send(
            memberTopic, std::vector<uint8_t>(inviteStr.begin(), inviteStr.end()));
    }

    QJsonObject result;
    result["group_id"] = QString::fromStdString(groupId);
    result["topic"] = QString::fromStdString(topic);
    return QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString();
}
