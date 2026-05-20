#include "pilot_impl.h"
#include "pilot_crypto.h"
#include "logos_api.h"
#include "logos_api_client.h"
#include <sstream>
#include <random>
#include <QString>
#include <QVariant>

static std::string genGroupId() {
    std::random_device rd;
    std::mt19937_64 rng(rd());
    std::ostringstream ss;
    ss << std::hex << rng();
    return ss.str();
}

std::string PilotImpl::messagingSend(const std::string& recipient, const std::string& message) {
    if (!logosAPI_) return "{\"error\": \"not initialized\"}";

    std::string payload = "{\"from\": \"" + agentNpk_ + "\", \"message\": \"" + message + "\"}";
    std::vector<uint8_t> plainBytes(payload.begin(), payload.end());
    ECIESCiphertext encrypted = eciesEncrypt(recipient, plainBytes);
    std::string encPayload = eciesSerialize(encrypted);

    std::string topic = "/pilot/1/inbox-" + recipient + "/proto";

    auto* delivery = logosAPI_->getClient("delivery_module");
    if (!delivery) return "{\"error\": \"delivery module unavailable\"}";

    delivery->invokeRemoteMethod(
        "delivery_module", "send",
        QString::fromStdString(topic),
        QString::fromStdString(encPayload));

    return "{\"sent\": true, \"recipient\": \"" + recipient + "\", \"encrypted\": true}";
}

bool PilotImpl::messagingJoin(const std::string& groupId) {
    if (!logosAPI_) return false;

    std::string topic = "/pilot/1/group-" + groupId + "/proto";

    auto* delivery = logosAPI_->getClient("delivery_module");
    if (!delivery) return false;

    QVariant result = delivery->invokeRemoteMethod(
        "delivery_module", "subscribe",
        QString::fromStdString(topic));

    return !result.isNull();
}

std::string PilotImpl::messagingCreateGroup(const std::string& membersJson) {
    if (!logosAPI_) return "{\"error\": \"not initialized\"}";

    std::string groupId = genGroupId();
    std::string topic = "/pilot/1/group-" + groupId + "/proto";

    auto* delivery = logosAPI_->getClient("delivery_module");
    if (!delivery) return "{\"error\": \"delivery module unavailable\"}";

    QVariant subResult = delivery->invokeRemoteMethod(
        "delivery_module", "subscribe",
        QString::fromStdString(topic));
    if (subResult.isNull())
        return "{\"error\": \"failed to create group topic\"}";

    std::string invite = "{\"type\": \"group_invite\", \"group_id\": \"" + groupId +
        "\", \"topic\": \"" + topic + "\", \"from\": \"" + agentNpk_ + "\"}";

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
        delivery->invokeRemoteMethod(
            "delivery_module", "send",
            QString::fromStdString(memberTopic),
            QString::fromStdString(invite));
    }

    return "{\"group_id\": \"" + groupId + "\", \"topic\": \"" + topic + "\"}";
}
