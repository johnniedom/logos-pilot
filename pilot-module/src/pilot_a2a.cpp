#include "pilot_impl.h"
#include "pilot_crypto.h"
#include "logos_api.h"
#include "logos_api_client.h"
#include "logos_mode.h"
#include <sqlite3.h>
#include <sstream>
#include <chrono>
#include <random>

#include <QString>
#include <QVariant>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QByteArray>

static const Timeout RPC_TIMEOUT(15000);

static std::string extractEncryptionKey(const std::string& addr) {
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(addr));
    if (doc.isObject() && doc.object().contains("viewing_public_key"))
        return doc.object()["viewing_public_key"].toString().toStdString();
    return addr;
}

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

    QJsonObject card;
    card["name"] = QString("Pilot Agent");
    card["description"] = QString("Sovereign AI agent on LEZ with wallet, storage, and messaging");
    card["url"] = QString::fromStdString("waku:" + inbox);
    card["version"] = QString("1.0.0");
    card["documentationUrl"] = QString("https://github.com/johnniedom/pilot");

    QJsonObject capabilities;
    capabilities["streaming"] = true;
    capabilities["pushNotifications"] = true;
    capabilities["stateTransitionHistory"] = true;
    card["capabilities"] = capabilities;

    QJsonArray defaultModes;
    defaultModes.append(QString("application/json"));
    card["defaultInputModes"] = defaultModes;
    card["defaultOutputModes"] = defaultModes;

    QJsonArray jsonMode;
    jsonMode.append(QString("application/json"));
    QJsonArray jsonOctetIn;
    jsonOctetIn.append(QString("application/json"));
    jsonOctetIn.append(QString("application/octet-stream"));
    QJsonArray jsonOctetOut;
    jsonOctetOut.append(QString("application/json"));
    jsonOctetOut.append(QString("application/octet-stream"));
    QJsonArray textJsonIn;
    textJsonIn.append(QString("application/json"));
    textJsonIn.append(QString("text/plain"));

    auto mkSkill = [&](const char* id, const char* name, const char* desc,
                       const QJsonArray& in, const QJsonArray& out) {
        QJsonObject s;
        s["id"] = QString(id);
        s["name"] = QString(name);
        s["description"] = QString(desc);
        s["inputModes"] = in;
        s["outputModes"] = out;
        return s;
    };

    QJsonArray skills;
    skills.append(mkSkill("wallet-balance", "Wallet Balance",
        "Returns the agent's current shielded token balance", jsonMode, jsonMode));
    skills.append(mkSkill("wallet-send", "Wallet Send",
        "Sends LEZ tokens to a recipient, subject to spending threshold", jsonMode, jsonMode));
    skills.append(mkSkill("storage-upload", "Storage Upload",
        "Encrypts and uploads a file to Logos Storage", jsonOctetIn, jsonMode));
    skills.append(mkSkill("storage-download", "Storage Download",
        "Retrieves and decrypts a file from Logos Storage", jsonMode, jsonOctetOut));
    skills.append(mkSkill("storage-share", "Storage Share",
        "Shares access to a stored file with another Logos identity", jsonMode, jsonMode));
    skills.append(mkSkill("messaging-send", "Messaging Send",
        "Sends an encrypted message to a Logos address", textJsonIn, jsonMode));
    skills.append(mkSkill("program-query", "Program Query",
        "Reads state from a LEZ program", jsonMode, jsonMode));
    skills.append(mkSkill("program-call", "Program Call",
        "Submits a transaction to a LEZ program", jsonMode, jsonMode));
    skills.append(mkSkill("program-deploy", "Program Deploy",
        "Deploys a compiled LEZ program binary to the network", jsonMode, jsonMode));
    card["skills"] = skills;

    QJsonObject auth;
    QJsonArray schemes;
    schemes.append(QString("ecies"));
    auth["schemes"] = schemes;
    auth["credentials"] = QString::fromStdString("npk:" + agentNpk_);
    card["authentication"] = auth;

    QJsonObject logos;
    logos["npk"] = QString::fromStdString(agentNpk_);
    logos["inbox_topic"] = QString::fromStdString(inbox);
    logos["transport"] = QString("waku");

    QJsonObject pricing;
    pricing["storage-upload"] = 10;
    pricing["storage-download"] = 5;
    pricing["storage-share"] = 5;
    pricing["messaging-send"] = 1;
    pricing["program-call"] = 10;
    pricing["program-deploy"] = 100;
    logos["pricing"] = pricing;

    logos["payment"] = QString("lez");
    logos["payment_timing"] = QString("on-acceptance");
    card["_logos"] = logos;

    std::string cardStr = QJsonDocument(card).toJson(QJsonDocument::Compact).toStdString();

    if (logosAPI_) {
        auto* delivery = logosAPI_->getClient("delivery_module");
        if (delivery && delivery->isConnected()) {
            delivery->invokeRemoteMethod(
                "delivery_module", "send",
                QString("/pilot/1/discovery/proto"),
                QString::fromStdString(cardStr), RPC_TIMEOUT);
        }
    }

    return cardStr;
}

std::string PilotImpl::agentDiscover(const std::string& topic) {
    if (!logosAPI_) return "{\"error\": \"not initialized\"}";

    std::string discoveryTopic = topic.empty()
        ? "/pilot/1/discovery/proto"
        : "/pilot/1/discovery-" + topic + "/proto";

    QJsonArray agents;

    // 1. Check local cache first
    if (db_) {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_,
            "SELECT npk, card_json FROM discovered_agents WHERE topic = ? ORDER BY last_seen DESC;",
            -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, discoveryTopic.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string cardStr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            QJsonDocument cardDoc = QJsonDocument::fromJson(QByteArray::fromStdString(cardStr));
            if (cardDoc.isObject())
                agents.append(cardDoc.object());
        }
        sqlite3_finalize(stmt);
    }

    // 2. Try network discovery via delivery_module
    auto* delivery = logosAPI_->getClient("delivery_module");
    if (delivery && delivery->isConnected()) {
        delivery->invokeRemoteMethod(
            "delivery_module", "subscribe",
            QString::fromStdString(discoveryTopic), RPC_TIMEOUT);

        QVariant storeResult = delivery->invokeRemoteMethod(
            "delivery_module", "storeQuery",
            QString::fromStdString(discoveryTopic), RPC_TIMEOUT);

        if (!storeResult.isNull()) {
            QJsonDocument netDoc = QJsonDocument::fromJson(storeResult.toString().toUtf8());
            QJsonArray netAgents = netDoc.isArray() ? netDoc.array() : QJsonArray();

            // 3. Cache network results in SQLite
            auto now = std::chrono::system_clock::now();
            std::string ts = std::to_string(
                std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());

            for (const auto& val : netAgents) {
                if (!val.isObject()) continue;
                QJsonObject agentCard = val.toObject();
                QString npk = agentCard["name"].toString();
                if (npk.isEmpty()) continue;

                std::string cardJson = QJsonDocument(agentCard).toJson(QJsonDocument::Compact).toStdString();
                if (db_) {
                    sqlite3_stmt* ins = nullptr;
                    sqlite3_prepare_v2(db_,
                        "INSERT OR REPLACE INTO discovered_agents (npk, card_json, topic, last_seen) VALUES (?, ?, ?, ?);",
                        -1, &ins, nullptr);
                    sqlite3_bind_text(ins, 1, npk.toStdString().c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(ins, 2, cardJson.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(ins, 3, discoveryTopic.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(ins, 4, ts.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_step(ins);
                    sqlite3_finalize(ins);
                }

                bool found = false;
                for (const auto& existing : agents)
                    if (existing.toObject()["name"] == agentCard["name"]) { found = true; break; }
                if (!found)
                    agents.append(agentCard);
            }
        }
    }

    QJsonObject res;
    res["agents"] = agents;
    res["count"] = agents.size();
    res["topic"] = QString::fromStdString(discoveryTopic);
    if (agents.isEmpty())
        res["note"] = QString("no agents found — subscribed for live cards");
    return QJsonDocument(res).toJson(QJsonDocument::Compact).toStdString();
}

std::string PilotImpl::agentTask(const std::string& agentAddress, const std::string& skill, const std::string& paramsJson) {
    if (!logosAPI_) return "{\"error\": \"not initialized\"}";

    std::string taskId = genUuid();
    std::string replyTopic = "/pilot/1/reply-" + taskId + "/proto";

    auto* delivery = logosAPI_->getClient("delivery_module");
    if (!delivery || !delivery->isConnected()) return "{\"error\": \"delivery module unavailable\"}";

    QVariant subResult = delivery->invokeRemoteMethod(
        "delivery_module", "subscribe",
        QString::fromStdString(replyTopic), RPC_TIMEOUT);
    if (subResult.isNull())
        return "{\"error\": \"failed to subscribe to reply topic\"}";

    QJsonDocument paramsDoc = QJsonDocument::fromJson(QByteArray::fromStdString(paramsJson));
    QJsonValue textValue = paramsDoc.isObject() ? QJsonValue(paramsDoc.object()) :
        (paramsDoc.isArray() ? QJsonValue(paramsDoc.array()) :
         QJsonValue(QString::fromStdString(paramsJson)));

    QJsonObject textPart;
    textPart["type"] = QString("text");
    textPart["text"] = textValue;
    QJsonArray parts;
    parts.append(textPart);

    QJsonObject message;
    message["role"] = QString("user");
    message["parts"] = parts;

    QJsonObject metadata;
    metadata["skill"] = QString::fromStdString(skill);

    QJsonObject params;
    params["id"] = QString::fromStdString(taskId);
    params["message"] = message;
    params["metadata"] = metadata;

    QJsonObject logosExt;
    logosExt["sender_npk"] = QString::fromStdString(agentNpk_);
    logosExt["reply_topic"] = QString::fromStdString(replyTopic);
    logosExt["timestamp"] = QString::fromStdString(nowTimestamp());

    QJsonObject request;
    request["jsonrpc"] = QString("2.0");
    request["method"] = QString("tasks/send");
    request["id"] = QString::fromStdString(taskId);
    request["params"] = params;
    request["_logos"] = logosExt;

    std::string requestStr = QJsonDocument(request).toJson(QJsonDocument::Compact).toStdString();
    std::vector<uint8_t> plainBytes(requestStr.begin(), requestStr.end());
    std::string encPayload;
    try {
        ECIESCiphertext encrypted = eciesEncrypt(extractEncryptionKey(agentAddress), plainBytes);
        encPayload = eciesSerialize(encrypted);
    } catch (const std::exception& e) {
        return "{\"error\": \"encryption failed: " + std::string(e.what()) + "\"}";
    }

    std::string inboxTopic = "/pilot/1/inbox-" + extractEncryptionKey(agentAddress) + "/proto";
    delivery->invokeRemoteMethod(
        "delivery_module", "send",
        QString::fromStdString(inboxTopic),
        QString::fromStdString(encPayload), RPC_TIMEOUT);

    QJsonObject status;
    status["state"] = QString("submitted");
    QJsonObject logosReply;
    logosReply["reply_topic"] = QString::fromStdString(replyTopic);
    QJsonObject result;
    result["id"] = QString::fromStdString(taskId);
    result["status"] = status;
    result["_logos"] = logosReply;
    return QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString();
}

std::string PilotImpl::agentSubscribe(const std::string& agentAddress, const std::string& taskId) {
    if (!logosAPI_) return "{\"error\": \"not initialized\"}";

    std::string taskTopic = "/pilot/1/task-" + taskId + "/proto";

    auto* delivery = logosAPI_->getClient("delivery_module");
    if (!delivery || !delivery->isConnected()) return "{\"error\": \"delivery module unavailable\"}";

    QVariant result = delivery->invokeRemoteMethod(
        "delivery_module", "subscribe",
        QString::fromStdString(taskTopic), RPC_TIMEOUT);
    if (result.isNull())
        return "{\"error\": \"subscribe failed\"}";

    QJsonObject rpcParams;
    rpcParams["id"] = QString::fromStdString(taskId);

    QJsonObject logosExt;
    logosExt["sender_npk"] = QString::fromStdString(agentNpk_);
    logosExt["timestamp"] = QString::fromStdString(nowTimestamp());

    QJsonObject request;
    request["jsonrpc"] = QString("2.0");
    request["method"] = QString("tasks/sendSubscribe");
    request["id"] = QString::fromStdString(genUuid());
    request["params"] = rpcParams;
    request["_logos"] = logosExt;

    std::string reqStr = QJsonDocument(request).toJson(QJsonDocument::Compact).toStdString();
    std::vector<uint8_t> subPlain(reqStr.begin(), reqStr.end());
    std::string subPayload;
    try {
        ECIESCiphertext subEnc = eciesEncrypt(extractEncryptionKey(agentAddress), subPlain);
        subPayload = eciesSerialize(subEnc);
    } catch (const std::exception& e) {
        return "{\"error\": \"encryption failed: " + std::string(e.what()) + "\"}";
    }

    std::string inboxTopic = "/pilot/1/inbox-" + extractEncryptionKey(agentAddress) + "/proto";
    delivery->invokeRemoteMethod(
        "delivery_module", "send",
        QString::fromStdString(inboxTopic),
        QString::fromStdString(subPayload), RPC_TIMEOUT);

    QJsonObject res;
    res["subscribed"] = true;
    res["task_id"] = QString::fromStdString(taskId);
    res["topic"] = QString::fromStdString(taskTopic);
    return QJsonDocument(res).toJson(QJsonDocument::Compact).toStdString();
}

bool PilotImpl::agentCancel(const std::string& agentAddress, const std::string& taskId) {
    if (!logosAPI_) return false;

    auto* delivery = logosAPI_->getClient("delivery_module");
    if (!delivery || !delivery->isConnected()) return false;

    QJsonObject rpcParams;
    rpcParams["id"] = QString::fromStdString(taskId);

    QJsonObject logosExt;
    logosExt["sender_npk"] = QString::fromStdString(agentNpk_);
    logosExt["timestamp"] = QString::fromStdString(nowTimestamp());

    QJsonObject request;
    request["jsonrpc"] = QString("2.0");
    request["method"] = QString("tasks/cancel");
    request["id"] = QString::fromStdString(genUuid());
    request["params"] = rpcParams;
    request["_logos"] = logosExt;

    std::string cancelStr = QJsonDocument(request).toJson(QJsonDocument::Compact).toStdString();
    std::vector<uint8_t> cancelPlain(cancelStr.begin(), cancelStr.end());
    std::string cancelPayload;
    try {
        ECIESCiphertext cancelEnc = eciesEncrypt(extractEncryptionKey(agentAddress), cancelPlain);
        cancelPayload = eciesSerialize(cancelEnc);
    } catch (const std::exception& e) {
        return false;
    }

    std::string inboxTopic = "/pilot/1/inbox-" + extractEncryptionKey(agentAddress) + "/proto";
    delivery->invokeRemoteMethod(
        "delivery_module", "send",
        QString::fromStdString(inboxTopic),
        QString::fromStdString(cancelPayload), RPC_TIMEOUT);

    std::string taskTopic = "/pilot/1/task-" + taskId + "/proto";
    std::string replyTopic = "/pilot/1/reply-" + taskId + "/proto";
    delivery->invokeRemoteMethod(
        "delivery_module", "unsubscribe",
        QString::fromStdString(taskTopic), RPC_TIMEOUT);
    delivery->invokeRemoteMethod(
        "delivery_module", "unsubscribe",
        QString::fromStdString(replyTopic), RPC_TIMEOUT);

    return true;
}

std::string PilotImpl::programQuery(const std::string& programId, const std::string& paramsJson) {
    if (!logosAPI_) return "{\"error\": \"not initialized\"}";

    auto* wallet = logosAPI_->getClient("logos_execution_zone");
    if (!wallet || !wallet->isConnected()) return "{\"error\": \"wallet module unavailable\"}";

    QVariant result = wallet->invokeRemoteMethod(
        "logos_execution_zone", "queryProgram",
        QString::fromStdString(programId),
        QString::fromStdString(paramsJson), RPC_TIMEOUT);

    if (result.isNull()) {
        QJsonObject err;
        err["error"] = QString("query failed — wallet-ffi does not yet support program queries");
        err["program"] = QString::fromStdString(programId);
        return QJsonDocument(err).toJson(QJsonDocument::Compact).toStdString();
    }

    QJsonObject res;
    res["program"] = QString::fromStdString(programId);
    QJsonDocument resultDoc = QJsonDocument::fromJson(result.toString().toUtf8());
    res["result"] = resultDoc.isObject() ? QJsonValue(resultDoc.object()) :
        (resultDoc.isArray() ? QJsonValue(resultDoc.array()) : QJsonValue(result.toString()));
    return QJsonDocument(res).toJson(QJsonDocument::Compact).toStdString();
}

std::string PilotImpl::programCall(const std::string& programId, const std::string& instruction, const std::string& paramsJson) {
    if (!logosAPI_ || agentAccountId_.empty()) return "{\"error\": \"not initialized\"}";

    int64_t estimatedCost = 10;

    if (estimatedCost > spendLimitPerTx_) {
        std::string reqId = createSpendRequest(programId, estimatedCost,
            "program.call: " + instruction);
        QJsonObject res;
        res["status"] = QString("held");
        res["request_id"] = QString::fromStdString(reqId);
        res["message"] = QString("Program call requires approval");
        return QJsonDocument(res).toJson(QJsonDocument::Compact).toStdString();
    }

    auto* wallet = logosAPI_->getClient("logos_execution_zone");
    if (!wallet || !wallet->isConnected()) return "{\"error\": \"wallet module unavailable\"}";

    QVariant result = wallet->invokeRemoteMethod(
        "logos_execution_zone", "callProgram",
        QString::fromStdString(agentAccountId_),
        QString::fromStdString(programId),
        QString::fromStdString(instruction),
        QString::fromStdString(paramsJson), RPC_TIMEOUT);

    if (result.isNull()) {
        QJsonObject err;
        err["error"] = QString("call failed — wallet-ffi does not yet support program calls");
        err["program"] = QString::fromStdString(programId);
        return QJsonDocument(err).toJson(QJsonDocument::Compact).toStdString();
    }

    QJsonObject res;
    res["program"] = QString::fromStdString(programId);
    res["instruction"] = QString::fromStdString(instruction);
    QJsonDocument resultDoc = QJsonDocument::fromJson(result.toString().toUtf8());
    res["result"] = resultDoc.isObject() ? QJsonValue(resultDoc.object()) :
        (resultDoc.isArray() ? QJsonValue(resultDoc.array()) : QJsonValue(result.toString()));
    return QJsonDocument(res).toJson(QJsonDocument::Compact).toStdString();
}

std::string PilotImpl::programDeploy(const std::string& binaryPath) {
    if (!logosAPI_ || agentAccountId_.empty()) return "{\"error\": \"not initialized\"}";

    std::string reqId = createSpendRequest("program_deploy", 100,
        "Deploy program: " + binaryPath);

    sendToOwner("Program deployment requested:\nBinary: " + binaryPath +
        "\nEstimated cost: 100 LEZ\n/approve " + reqId + "\n/reject " + reqId);

    QJsonObject res;
    res["status"] = QString("held");
    res["request_id"] = QString::fromStdString(reqId);
    res["message"] = QString("Deployment requires owner approval");
    res["binary"] = QString::fromStdString(binaryPath);
    return QJsonDocument(res).toJson(QJsonDocument::Compact).toStdString();
}
