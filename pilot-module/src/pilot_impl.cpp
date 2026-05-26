#include "pilot_impl.h"
#include "pilot_llm.h"
#include "pilot_skill.h"
#include "logos_api.h"
#include "logos_api_client.h"
#include "logos_mode.h"
#include "logos_object.h"
#include "pilot_crypto.h"
#include <sqlite3.h>
#include <stdexcept>
#include <cstdlib>
#include <sys/stat.h>
#include <thread>
#include <chrono>
#include <QString>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

PilotImpl::PilotImpl()
    : llm_(std::make_unique<NoOpProvider>()),
      registry_(std::make_unique<SkillRegistry>()) {
    registerBuiltinSkills(*registry_, this);
}

PilotImpl::~PilotImpl() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}


std::string PilotImpl::echo(const std::string& input) {
    return "echo: " + input;
}

void PilotImpl::initDatabase(const std::string& dataDir) {
    dataDir_ = dataDir;
    mkdir(dataDir.c_str(), 0755);
    std::string dbPath = dataDir + "/pilot.db";
    int rc = sqlite3_open(dbPath.c_str(), &db_);
    if (rc != SQLITE_OK)
        throw std::runtime_error("Failed to open pilot database");

    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=FULL;", nullptr, nullptr, nullptr);

    const char* schema = R"SQL(
        CREATE TABLE IF NOT EXISTS agent_identity (
            id INTEGER PRIMARY KEY CHECK (id = 1),
            npk TEXT NOT NULL,
            account_id TEXT NOT NULL,
            created_at TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS owner_channel (
            id INTEGER PRIMARY KEY CHECK (id = 1),
            conversation_id TEXT NOT NULL,
            established_at TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS spend_requests (
            id TEXT PRIMARY KEY,
            recipient TEXT NOT NULL,
            amount INTEGER NOT NULL,
            reason TEXT NOT NULL,
            state TEXT NOT NULL DEFAULT 'CREATED',
            created_at TEXT NOT NULL,
            updated_at TEXT NOT NULL,
            expires_at TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS stored_files (
            cid TEXT PRIMARY KEY,
            label TEXT NOT NULL,
            file_key_encrypted TEXT NOT NULL,
            timestamp TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS config (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS discovered_agents (
            npk TEXT PRIMARY KEY,
            card_json TEXT NOT NULL,
            topic TEXT NOT NULL,
            last_seen TEXT NOT NULL
        );
    )SQL";

    char* errMsg = nullptr;
    rc = sqlite3_exec(db_, schema, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::string err = errMsg ? errMsg : "unknown";
        sqlite3_free(errMsg);
        throw std::runtime_error("Schema creation failed: " + err);
    }
}

static bool waitForConnection(LogosAPIClient* client, int maxMs = 5000) {
    if (!client) return false;
    for (int elapsed = 0; elapsed < maxMs && !client->isConnected(); elapsed += 250)
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    return client->isConnected();
}

void PilotImpl::initStorageModule() {
    if (!logosAPI_ || storageInitialized_) return;
    storageInitialized_ = true;

    auto* storage = logosAPI_->getClient("storage_module");
    if (waitForConnection(storage)) {
        storage->invokeRemoteMethod("storage_module", "init",
            QString("{\"nat\":\"none\"}"), Timeout(10000));
        storage->invokeRemoteMethod("storage_module", "start",
            QVariantList{}, Timeout(10000));
    }
}

void PilotImpl::initDeliveryModule() {
    if (!logosAPI_ || deliveryInitialized_) return;
    deliveryInitialized_ = true;

    std::string wakuAddr;
    if (const char* env = std::getenv("PILOT_WAKU_ADDR"))
        wakuAddr = env;
    else
        wakuAddr = "/ip4/127.0.0.1/tcp/30303";

    auto* delivery = logosAPI_->getClient("delivery_module");
    if (waitForConnection(delivery)) {
        QJsonArray shards;
        for (int i = 0; i < 8; i++) shards.append(i);
        QJsonArray staticNodes;
        staticNodes.append(QString::fromStdString(wakuAddr));
        QJsonObject cfgObj;
        cfgObj["preset"] = QString("logos.dev");
        if (const char* modeEnv = std::getenv("PILOT_WAKU_MODE"))
            cfgObj["mode"] = QString(modeEnv);
        else
            cfgObj["mode"] = QString("Core");
        if (const char* tcpEnv = std::getenv("PILOT_TCP_PORT"))
            cfgObj["tcpPort"] = std::atoi(tcpEnv);
        if (const char* natEnv = std::getenv("PILOT_NAT"))
            cfgObj["nat"] = QString(natEnv);
        std::string cfg = QJsonDocument(cfgObj).toJson(QJsonDocument::Compact).toStdString();
        delivery->invokeRemoteMethod("delivery_module", "createNode",
            QString::fromStdString(cfg), Timeout(15000));
        delivery->invokeRemoteMethod("delivery_module", "start",
            QVariantList{}, Timeout(15000));

        LogosObject* deliveryObj = delivery->requestObject("delivery_module");
        if (deliveryObj) {
            delivery->onEvent(deliveryObj, "messageReceived",
                [this](const QString&, const QVariantList& data) {
                    if (data.size() < 2) return;
                    std::string topic = data[0].toString().toStdString();
                    std::string payload = data[1].toString().toStdString();

                    if (topic != ownerChannelId_ || agentEciesPriv_.empty()) return;

                    try {
                        ECIESCiphertext ct = eciesDeserialize(payload);
                        std::vector<uint8_t> plain = eciesDecrypt(agentEciesPriv_, ct);
                        std::string message(plain.begin(), plain.end());
                        std::string response = processOwnerMessage(message);
                        sendToOwner(response);
                    } catch (...) {}
                });
        }
    }
}

void PilotImpl::initDependencyModules() {
    if (depsInitialized_) return;
    depsInitialized_ = true;
    initStorageModule();
    initDeliveryModule();
}

void PilotImpl::initLLM() {
    llm_ = createLLMProvider(llmProvider_, llmModel_);
    if (!llm_)
        llm_ = std::make_unique<NoOpProvider>();
}

std::string PilotImpl::buildLLMSystemPrompt() {
    std::string skillsList;
    if (registry_) skillsList = registry_->listSkills();

    std::string prompt =
        "You are Pilot, a sovereign AI agent on the Logos network. "
        "You help your owner manage their wallet, files, messaging, and interact with other agents.\n\n"
        "You have 21 module skills across 6 categories:\n"
        "- WALLET: balance, send, history\n"
        "- STORAGE: upload, download, list, share (encrypted file storage)\n"
        "- MESSAGING: send, join, create_group (encrypted messaging)\n"
        "- AGENT: card, discover, task, subscribe, cancel (A2A protocol)\n"
        "- PROGRAM: query, call, deploy (LEZ smart contracts)\n"
        "- META: status, skills, configure\n\n"
        "Owner slash commands you can dispatch:\n"
        "/balance, /history, /send <to> <amt> <reason>, /approve <id>, /reject <id>,\n"
        "/upload <path> <label>, /download <cid> <path>, /files,\n"
        "/skills, /status, /discover, /help\n\n"
        "Current state:\n"
        "- Owner: " + (ownerName_.empty() ? "unknown" : ownerName_) + "\n"
        "- NPK: " + agentNpk_ + "\n"
        "- Account: " + agentAccountId_ + "\n"
        "- Initialized: " + (initialized_ ? "yes" : "no") + "\n\n"
        "When the owner asks you to perform an action, respond with ONLY a JSON object:\n"
        "{\"action\": \"<command_name>\", \"params\": {<relevant params>}}\n"
        "When the owner asks a question or chats casually, respond naturally:\n"
        "{\"action\": \"reply\", \"params\": {\"text\": \"<your response>\"}}\n"
        "Be concise, helpful, and accurate about your capabilities.\n";
    return prompt;
}

std::string PilotImpl::processOwnerMessage(const std::string& message) {
    if (message.empty()) return "{\"action\": \"none\"}";

    if (message[0] == '/') {
        QJsonObject params;
        params["raw"] = QString::fromStdString(message);
        QJsonObject cmd;
        cmd["action"] = QString("command");
        cmd["params"] = params;
        return QJsonDocument(cmd).toJson(QJsonDocument::Compact).toStdString();
    }

    if (!llm_ || !llm_->isConfigured()) {
        QJsonObject params;
        params["text"] = QString("I'm in command-only mode (no LLM configured). Use /help for available commands.");
        QJsonObject reply;
        reply["action"] = QString("reply");
        reply["params"] = params;
        return QJsonDocument(reply).toJson(QJsonDocument::Compact).toStdString();
    }

    std::string systemPrompt = buildLLMSystemPrompt();
    chatHistory_.push_back({"user", message});
    if (chatHistory_.size() > 40)
        chatHistory_.erase(chatHistory_.begin(), chatHistory_.begin() + 2);

    std::vector<LLMMessage> messages;
    for (const auto& [role, content] : chatHistory_)
        messages.push_back({role, content});

    std::string response = llm_->complete(systemPrompt, messages);

    if (!response.empty() && response.find("\"error\"") == std::string::npos)
        chatHistory_.push_back({"assistant", response});

    if (response.empty() || response.find("\"error\"") != std::string::npos) {
        std::string errDetail = response;
        if (errDetail.empty()) errDetail = "LLM returned empty response";
        QJsonObject params;
        params["text"] = QString::fromStdString("LLM error: " + errDetail + ". Use /help for available commands.");
        QJsonObject reply;
        reply["action"] = QString("reply");
        reply["params"] = params;
        return QJsonDocument(reply).toJson(QJsonDocument::Compact).toStdString();
    }

    return response;
}

std::string PilotImpl::dispatchSkill(const std::string& skillName, const std::string& argsJson) {
    if (!registry_) return "{\"error\": \"registry not initialized\"}";
    return registry_->dispatch(skillName, argsJson);
}
