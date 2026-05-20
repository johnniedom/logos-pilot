#include "pilot_impl.h"
#include "pilot_crypto.h"
#include "logos_api.h"
#include "logos_api_client.h"
#include <sstream>
#include <chrono>
#include <random>
#include <QString>
#include <QVariant>

static std::string genUuid() {
    std::random_device rd;
    std::mt19937_64 rng(rd());
    std::ostringstream ss;
    ss << std::hex << rng() << "-" << rng();
    return ss.str();
}

static std::string nowTimestamp() {
    auto now = std::chrono::system_clock::now();
    return std::to_string(std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());
}

std::string PilotImpl::agentCard() {
    if (agentNpk_.empty()) return "{\"error\": \"not initialized\"}";

    std::string inbox = "/pilot/1/inbox-" + agentNpk_ + "/proto";

    std::ostringstream card;
    card << "{"
         << "\"name\": \"Pilot Agent\","
         << "\"description\": \"Sovereign AI agent on LEZ with wallet, storage, and messaging\","
         << "\"url\": \"waku:" << inbox << "\","
         << "\"version\": \"1.0.0\","
         << "\"documentationUrl\": \"https://github.com/johnniedom/pilot\","
         << "\"capabilities\": {"
         << "  \"streaming\": true,"
         << "  \"pushNotifications\": true,"
         << "  \"stateTransitionHistory\": true"
         << "},"
         << "\"defaultInputModes\": [\"application/json\"],"
         << "\"defaultOutputModes\": [\"application/json\"],"
         << "\"skills\": ["
         << "  {"
         << "    \"id\": \"wallet-balance\","
         << "    \"name\": \"Wallet Balance\","
         << "    \"description\": \"Returns the agent's current shielded token balance\","
         << "    \"inputModes\": [\"application/json\"],"
         << "    \"outputModes\": [\"application/json\"]"
         << "  },"
         << "  {"
         << "    \"id\": \"wallet-send\","
         << "    \"name\": \"Wallet Send\","
         << "    \"description\": \"Sends LEZ tokens to a recipient, subject to spending threshold\","
         << "    \"inputModes\": [\"application/json\"],"
         << "    \"outputModes\": [\"application/json\"]"
         << "  },"
         << "  {"
         << "    \"id\": \"storage-upload\","
         << "    \"name\": \"Storage Upload\","
         << "    \"description\": \"Encrypts and uploads a file to Logos Storage\","
         << "    \"inputModes\": [\"application/json\", \"application/octet-stream\"],"
         << "    \"outputModes\": [\"application/json\"]"
         << "  },"
         << "  {"
         << "    \"id\": \"storage-download\","
         << "    \"name\": \"Storage Download\","
         << "    \"description\": \"Retrieves and decrypts a file from Logos Storage\","
         << "    \"inputModes\": [\"application/json\"],"
         << "    \"outputModes\": [\"application/json\", \"application/octet-stream\"]"
         << "  },"
         << "  {"
         << "    \"id\": \"storage-share\","
         << "    \"name\": \"Storage Share\","
         << "    \"description\": \"Shares access to a stored file with another Logos identity\","
         << "    \"inputModes\": [\"application/json\"],"
         << "    \"outputModes\": [\"application/json\"]"
         << "  },"
         << "  {"
         << "    \"id\": \"messaging-send\","
         << "    \"name\": \"Messaging Send\","
         << "    \"description\": \"Sends an encrypted message to a Logos address\","
         << "    \"inputModes\": [\"application/json\", \"text/plain\"],"
         << "    \"outputModes\": [\"application/json\"]"
         << "  },"
         << "  {"
         << "    \"id\": \"program-query\","
         << "    \"name\": \"Program Query\","
         << "    \"description\": \"Reads state from a LEZ program\","
         << "    \"inputModes\": [\"application/json\"],"
         << "    \"outputModes\": [\"application/json\"]"
         << "  },"
         << "  {"
         << "    \"id\": \"program-call\","
         << "    \"name\": \"Program Call\","
         << "    \"description\": \"Submits a transaction to a LEZ program\","
         << "    \"inputModes\": [\"application/json\"],"
         << "    \"outputModes\": [\"application/json\"]"
         << "  },"
         << "  {"
         << "    \"id\": \"program-deploy\","
         << "    \"name\": \"Program Deploy\","
         << "    \"description\": \"Deploys a compiled LEZ program binary to the network\","
         << "    \"inputModes\": [\"application/json\"],"
         << "    \"outputModes\": [\"application/json\"]"
         << "  }"
         << "],"
         << "\"authentication\": {"
         << "  \"schemes\": [\"ecies\"],"
         << "  \"credentials\": \"npk:" << agentNpk_ << "\""
         << "},"
         << "\"_logos\": {"
         << "  \"npk\": \"" << agentNpk_ << "\","
         << "  \"inbox_topic\": \"" << inbox << "\","
         << "  \"transport\": \"waku\","
         << "  \"pricing\": {"
         << "    \"storage-upload\": 10,"
         << "    \"storage-download\": 5,"
         << "    \"storage-share\": 5,"
         << "    \"messaging-send\": 1,"
         << "    \"program-call\": 10,"
         << "    \"program-deploy\": 100"
         << "  },"
         << "  \"payment\": \"lez\","
         << "  \"payment_timing\": \"on-acceptance\""
         << "}"
         << "}";

    if (logosAPI_) {
        auto* delivery = logosAPI_->getClient("delivery_module");
        if (delivery) {
            delivery->invokeRemoteMethod(
                "delivery_module", "send",
                QString("/pilot/1/discovery/proto"),
                QString::fromStdString(card.str()));
        }
    }

    return card.str();
}

std::string PilotImpl::agentDiscover(const std::string& topic) {
    if (!logosAPI_) return "{\"error\": \"not initialized\"}";

    std::string discoveryTopic = topic.empty() ? "/pilot/1/discovery/proto" : topic;

    auto* delivery = logosAPI_->getClient("delivery_module");
    if (!delivery) return "{\"error\": \"delivery module unavailable\"}";

    QVariant subResult = delivery->invokeRemoteMethod(
        "delivery_module", "subscribe",
        QString::fromStdString(discoveryTopic));
    if (subResult.isNull())
        return "{\"error\": \"subscribe failed\"}";

    auto* waku = logosAPI_->getClient("waku_module");
    if (!waku)
        return "{\"agents\": [], \"note\": \"waku module unavailable\"}";

    QVariant storeResult = waku->invokeRemoteMethod(
        "waku_module", "storeQuery",
        QString::fromStdString(discoveryTopic));

    if (storeResult.isNull())
        return "{\"agents\": [], \"note\": \"store query failed, listening for live cards\"}";

    return "{\"agents\": " + storeResult.toString().toStdString() + "}";
}

std::string PilotImpl::agentTask(const std::string& agentAddress, const std::string& skill, const std::string& paramsJson) {
    if (!logosAPI_) return "{\"error\": \"not initialized\"}";

    std::string taskId = genUuid();
    std::string replyTopic = "/pilot/1/reply-" + taskId + "/proto";

    auto* delivery = logosAPI_->getClient("delivery_module");
    if (!delivery) return "{\"error\": \"delivery module unavailable\"}";

    QVariant subResult = delivery->invokeRemoteMethod(
        "delivery_module", "subscribe",
        QString::fromStdString(replyTopic));
    if (subResult.isNull())
        return "{\"error\": \"failed to subscribe to reply topic\"}";

    std::ostringstream request;
    request << "{"
            << "\"jsonrpc\": \"2.0\","
            << "\"method\": \"tasks/send\","
            << "\"id\": \"" << taskId << "\","
            << "\"params\": {"
            << "  \"id\": \"" << taskId << "\","
            << "  \"message\": {"
            << "    \"role\": \"user\","
            << "    \"parts\": [{"
            << "      \"type\": \"text\","
            << "      \"text\": " << paramsJson
            << "    }]"
            << "  },"
            << "  \"metadata\": {"
            << "    \"skill\": \"" << skill << "\""
            << "  }"
            << "},"
            << "\"_logos\": {"
            << "  \"sender_npk\": \"" << agentNpk_ << "\","
            << "  \"reply_topic\": \"" << replyTopic << "\","
            << "  \"timestamp\": \"" << nowTimestamp() << "\""
            << "}"
            << "}";

    std::string requestStr = request.str();
    std::vector<uint8_t> plainBytes(requestStr.begin(), requestStr.end());
    ECIESCiphertext encrypted = eciesEncrypt(agentAddress, plainBytes);
    std::string encPayload = eciesSerialize(encrypted);

    std::string inboxTopic = "/pilot/1/inbox-" + agentAddress + "/proto";
    delivery->invokeRemoteMethod(
        "delivery_module", "send",
        QString::fromStdString(inboxTopic),
        QString::fromStdString(encPayload));

    return "{"
        "\"id\": \"" + taskId + "\","
        "\"status\": {\"state\": \"submitted\"},"
        "\"_logos\": {\"reply_topic\": \"" + replyTopic + "\"}"
        "}";
}

std::string PilotImpl::agentSubscribe(const std::string& agentAddress, const std::string& taskId) {
    if (!logosAPI_) return "{\"error\": \"not initialized\"}";

    std::string taskTopic = "/pilot/1/task-" + taskId + "/proto";

    auto* delivery = logosAPI_->getClient("delivery_module");
    if (!delivery) return "{\"error\": \"delivery module unavailable\"}";

    QVariant result = delivery->invokeRemoteMethod(
        "delivery_module", "subscribe",
        QString::fromStdString(taskTopic));
    if (result.isNull())
        return "{\"error\": \"subscribe failed\"}";

    std::ostringstream request;
    request << "{"
            << "\"jsonrpc\": \"2.0\","
            << "\"method\": \"tasks/sendSubscribe\","
            << "\"id\": \"" << genUuid() << "\","
            << "\"params\": {"
            << "  \"id\": \"" << taskId << "\""
            << "},"
            << "\"_logos\": {"
            << "  \"sender_npk\": \"" << agentNpk_ << "\","
            << "  \"timestamp\": \"" << nowTimestamp() << "\""
            << "}"
            << "}";

    std::string reqStr = request.str();
    std::vector<uint8_t> subPlain(reqStr.begin(), reqStr.end());
    ECIESCiphertext subEnc = eciesEncrypt(agentAddress, subPlain);
    std::string subPayload = eciesSerialize(subEnc);

    std::string inboxTopic = "/pilot/1/inbox-" + agentAddress + "/proto";
    delivery->invokeRemoteMethod(
        "delivery_module", "send",
        QString::fromStdString(inboxTopic),
        QString::fromStdString(subPayload));

    return "{\"subscribed\": true, \"task_id\": \"" + taskId + "\", \"topic\": \"" + taskTopic + "\"}";
}

bool PilotImpl::agentCancel(const std::string& agentAddress, const std::string& taskId) {
    if (!logosAPI_) return false;

    auto* delivery = logosAPI_->getClient("delivery_module");
    if (!delivery) return false;

    std::ostringstream request;
    request << "{"
            << "\"jsonrpc\": \"2.0\","
            << "\"method\": \"tasks/cancel\","
            << "\"id\": \"" << genUuid() << "\","
            << "\"params\": {"
            << "  \"id\": \"" << taskId << "\""
            << "},"
            << "\"_logos\": {"
            << "  \"sender_npk\": \"" << agentNpk_ << "\","
            << "  \"timestamp\": \"" << nowTimestamp() << "\""
            << "}"
            << "}";

    std::string cancelStr = request.str();
    std::vector<uint8_t> cancelPlain(cancelStr.begin(), cancelStr.end());
    ECIESCiphertext cancelEnc = eciesEncrypt(agentAddress, cancelPlain);
    std::string cancelPayload = eciesSerialize(cancelEnc);

    std::string inboxTopic = "/pilot/1/inbox-" + agentAddress + "/proto";
    delivery->invokeRemoteMethod(
        "delivery_module", "send",
        QString::fromStdString(inboxTopic),
        QString::fromStdString(cancelPayload));

    std::string taskTopic = "/pilot/1/task-" + taskId + "/proto";
    std::string replyTopic = "/pilot/1/reply-" + taskId + "/proto";
    delivery->invokeRemoteMethod(
        "delivery_module", "unsubscribe",
        QString::fromStdString(taskTopic));
    delivery->invokeRemoteMethod(
        "delivery_module", "unsubscribe",
        QString::fromStdString(replyTopic));

    return true;
}

std::string PilotImpl::programQuery(const std::string& programId, const std::string& paramsJson) {
    if (!logosAPI_) return "{\"error\": \"not initialized\"}";

    auto* wallet = logosAPI_->getClient("lez_wallet_module");
    if (!wallet) return "{\"error\": \"wallet module unavailable\"}";

    QVariant result = wallet->invokeRemoteMethod(
        "lez_wallet_module", "queryProgram",
        QString::fromStdString(programId),
        QString::fromStdString(paramsJson));

    if (result.isNull())
        return "{\"error\": \"query failed\"}";

    return "{\"program\": \"" + programId + "\", \"result\": " + result.toString().toStdString() + "}";
}

std::string PilotImpl::programCall(const std::string& programId, const std::string& instruction, const std::string& paramsJson) {
    if (!logosAPI_ || agentAccountId_.empty()) return "{\"error\": \"not initialized\"}";

    int64_t estimatedCost = 10;

    if (estimatedCost > spendLimitPerTx_) {
        std::string reqId = createSpendRequest(programId, estimatedCost,
            "program.call: " + instruction);
        return "{\"status\": \"held\", \"request_id\": \"" + reqId + "\", \"message\": \"Program call requires approval\"}";
    }

    auto* wallet = logosAPI_->getClient("lez_wallet_module");
    if (!wallet) return "{\"error\": \"wallet module unavailable\"}";

    QVariant result = wallet->invokeRemoteMethod(
        "lez_wallet_module", "callProgram",
        QString::fromStdString(agentAccountId_),
        QString::fromStdString(programId),
        QString::fromStdString(instruction),
        QString::fromStdString(paramsJson));

    if (result.isNull())
        return "{\"error\": \"call failed\"}";

    return "{\"program\": \"" + programId + "\", \"instruction\": \"" + instruction +
        "\", \"result\": " + result.toString().toStdString() + "}";
}

std::string PilotImpl::programDeploy(const std::string& binaryPath) {
    if (!logosAPI_ || agentAccountId_.empty()) return "{\"error\": \"not initialized\"}";

    std::string reqId = createSpendRequest("program_deploy", 100,
        "Deploy program: " + binaryPath);

    sendToOwner("Program deployment requested:\nBinary: " + binaryPath +
        "\nEstimated cost: 100 LEZ\n/approve " + reqId + "\n/reject " + reqId);

    return "{\"status\": \"held\", \"request_id\": \"" + reqId +
        "\", \"message\": \"Deployment requires owner approval\", \"binary\": \"" + binaryPath + "\"}";
}
