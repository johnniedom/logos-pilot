#include "pilot_impl.h"
#include "pilot_llm.h"
#include "pilot_skill.h"
#include "logos_api.h"
#include "logos_api_client.h"
#include "logos_object.h"
#include "pilot_crypto.h"
#include <sqlite3.h>
#include <stdexcept>
#include <cstdlib>
#include <sys/stat.h>

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
    )SQL";

    char* errMsg = nullptr;
    rc = sqlite3_exec(db_, schema, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::string err = errMsg ? errMsg : "unknown";
        sqlite3_free(errMsg);
        throw std::runtime_error("Schema creation failed: " + err);
    }
}

void PilotImpl::initDependencyModules() {
    if (!logosAPI_) return;

    auto* storage = logosAPI_->getClient("storage_module");
    if (storage) {
        storage->invokeRemoteMethod("storage_module", "init",
            QString("{\"nat\":\"none\"}"));
        storage->invokeRemoteMethod("storage_module", "start");
    }

    std::string wakuAddr;
    if (const char* env = std::getenv("PILOT_WAKU_ADDR"))
        wakuAddr = env;
    else
        wakuAddr = "/ip4/127.0.0.1/tcp/30303";

    auto* delivery = logosAPI_->getClient("delivery_module");
    if (delivery) {
        std::string cfg = "{\"clusterId\":2,\"shards\":[0,1,2,3,4,5,6,7],"
            "\"staticNodes\":[\"" + wakuAddr + "\"]}";
        delivery->invokeRemoteMethod("delivery_module", "createNode",
            QString::fromStdString(cfg));
        delivery->invokeRemoteMethod("delivery_module", "start");

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

void PilotImpl::initLLM() {
    llm_ = createLLMProvider(llmProvider_, llmModel_);
    if (!llm_)
        llm_ = std::make_unique<NoOpProvider>();
}

std::string PilotImpl::buildLLMSystemPrompt() {
    std::string prompt =
        "You are the Pilot agent's reasoning layer. "
        "Parse owner messages into structured actions.\n\n"
        "Available commands:\n"
        "/approve <id> - approve a pending spend request\n"
        "/reject <id> - reject a pending spend request\n"
        "/balance - check wallet balance\n"
        "/history - view transaction history\n"
        "/send <recipient> <amount> <reason> - send LEZ tokens\n"
        "/upload <path> <label> - upload a file\n"
        "/download <cid> <path> - download a file\n"
        "/files - list stored files\n"
        "/skills - list available skills\n"
        "/status - agent status\n"
        "/discover - discover peer agents\n\n"
        "Current state:\n"
        "- NPK: " + agentNpk_ + "\n"
        "- Account: " + agentAccountId_ + "\n"
        "- Initialized: " + (initialized_ ? "yes" : "no") + "\n\n"
        "Respond with ONLY a JSON object:\n"
        "{\"action\": \"<command_name>\", \"params\": {<relevant params>}}\n"
        "If the message is casual/unclear, respond:\n"
        "{\"action\": \"reply\", \"params\": {\"text\": \"<your response>\"}}\n";
    return prompt;
}

std::string PilotImpl::processOwnerMessage(const std::string& message) {
    if (message.empty()) return "{\"action\": \"none\"}";

    // Slash commands bypass LLM
    if (message[0] == '/') {
        return "{\"action\": \"command\", \"params\": {\"raw\": \"" + message + "\"}}";
    }

    if (!llm_ || !llm_->isConfigured()) {
        return "{\"action\": \"reply\", \"params\": {\"text\": "
               "\"I'm in command-only mode (no LLM configured). Use /help for available commands.\"}}";
    }

    std::string systemPrompt = buildLLMSystemPrompt();
    std::vector<LLMMessage> messages = {{"user", message}};
    std::string response = llm_->complete(systemPrompt, messages);

    if (response.empty()) {
        return "{\"action\": \"reply\", \"params\": {\"text\": "
               "\"I couldn't process that. Use /help for available commands.\"}}";
    }

    return response;
}

std::string PilotImpl::dispatchSkill(const std::string& skillName, const std::string& argsJson) {
    if (!registry_) return "{\"error\": \"registry not initialized\"}";
    return registry_->dispatch(skillName, argsJson);
}
