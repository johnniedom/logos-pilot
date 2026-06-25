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
    // M2: keep the data dir owner-only; enforce even if it pre-exists / regardless of umask.
    mkdir(dataDir.c_str(), 0700);
    chmod(dataDir.c_str(), 0700);
    std::string dbPath = dataDir + "/pilot.db";
    int rc = sqlite3_open(dbPath.c_str(), &db_);
    if (rc != SQLITE_OK)
        throw std::runtime_error("Failed to open pilot database");

    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=FULL;", nullptr, nullptr, nullptr);

    // M2: pilot.db holds key material (the wrapped/plaintext ecies.priv) — restrict it to
    // the owner. The -wal/-shm sidecars may not exist yet; chmod is best-effort (ignored).
    chmod(dbPath.c_str(), 0600);
    chmod((dbPath + "-wal").c_str(), 0600);
    chmod((dbPath + "-shm").c_str(), 0600);

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
            timestamp TEXT NOT NULL,
            size_bytes INTEGER NOT NULL DEFAULT 0
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

        CREATE TABLE IF NOT EXISTS inbound_tasks (
            id TEXT PRIMARY KEY,
            sender_npk TEXT NOT NULL,
            sender_ecies TEXT NOT NULL DEFAULT '',
            reply_topic TEXT NOT NULL,
            skill TEXT NOT NULL,
            params_json TEXT NOT NULL,
            state TEXT NOT NULL DEFAULT 'accepted',
            spend_request_id TEXT,
            result_json TEXT,
            created_at TEXT NOT NULL,
            updated_at TEXT NOT NULL
        );

        -- M3: index the inbound_tasks access paths used by the per-sender flood gate
        -- (sender_npk + created_at window) and the TTL/row-cap eviction sweeps (state +
        -- created_at). discovered_agents.npk is already PRIMARY KEY, so no extra index there.
        CREATE INDEX IF NOT EXISTS idx_inbound_tasks_sender_created ON inbound_tasks(sender_npk, created_at);
        CREATE INDEX IF NOT EXISTS idx_inbound_tasks_state_created  ON inbound_tasks(state, created_at);

        CREATE TABLE IF NOT EXISTS outbound_tasks (
            id TEXT PRIMARY KEY,
            agent_address TEXT NOT NULL,
            skill TEXT NOT NULL,
            price INTEGER NOT NULL DEFAULT 0,
            reply_topic TEXT NOT NULL,
            state TEXT NOT NULL DEFAULT 'submitted',
            payout TEXT NOT NULL DEFAULT '',
            spend_request_id TEXT,
            created_at TEXT NOT NULL,
            updated_at TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS pinned_identities (
            npk TEXT PRIMARY KEY,
            signing_key TEXT NOT NULL,
            first_seen TEXT NOT NULL
        );
    )SQL";

    char* errMsg = nullptr;
    rc = sqlite3_exec(db_, schema, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::string err = errMsg ? errMsg : "unknown";
        sqlite3_free(errMsg);
        throw std::runtime_error("Schema creation failed: " + err);
    }

    // Migrate DBs created before stored_files.size_bytes (added for meta.status
    // storage-usage reporting). ADD COLUMN is a no-op error if it already exists,
    // which we deliberately ignore so the migration is idempotent.
    sqlite3_exec(db_,
        "ALTER TABLE stored_files ADD COLUMN size_bytes INTEGER NOT NULL DEFAULT 0;",
        nullptr, nullptr, nullptr);

    // Migrate DBs created before inbound_tasks.sender_ecies. The doer stores the
    // requester's _logos.sender_ecies here and encrypts EVERY A2A reply to it (one
    // ECIES keypair drives all A2A reply encryption on both legs). ADD COLUMN is a
    // no-op error if the column already exists, deliberately ignored for idempotency.
    sqlite3_exec(db_,
        "ALTER TABLE inbound_tasks ADD COLUMN sender_ecies TEXT NOT NULL DEFAULT '';",
        nullptr, nullptr, nullptr);

    // Migrate DBs created before outbound_tasks.payout. The requester records the doer's
    // resolved Agent Card payout account here at settlement time, so a duplicate reply
    // can be audited and recovery knows WHO was paid (never the messaging address, M5).
    // ADD COLUMN is a no-op error if the column already exists, deliberately ignored.
    sqlite3_exec(db_,
        "ALTER TABLE outbound_tasks ADD COLUMN payout TEXT NOT NULL DEFAULT '';",
        nullptr, nullptr, nullptr);

    // Migrate DBs created before the rest of the outbound_tasks pay-on-acceptance columns
    // were added across passes: price (declared LEZ price to settle), reply_topic (where the
    // doer's reply is consumed), spend_request_id (the linked spend the settlement/recovery
    // path drives). Each ADD COLUMN is a no-op error if the column already exists, which we
    // deliberately ignore so the whole migration stays idempotent. NOT NULL columns carry a
    // DEFAULT so the ALTER succeeds on a populated table; spend_request_id is nullable.
    sqlite3_exec(db_,
        "ALTER TABLE outbound_tasks ADD COLUMN price INTEGER NOT NULL DEFAULT 0;",
        nullptr, nullptr, nullptr);
    sqlite3_exec(db_,
        "ALTER TABLE outbound_tasks ADD COLUMN reply_topic TEXT NOT NULL DEFAULT '';",
        nullptr, nullptr, nullptr);
    sqlite3_exec(db_,
        "ALTER TABLE outbound_tasks ADD COLUMN spend_request_id TEXT;",
        nullptr, nullptr, nullptr);

    // Migrate DBs created before pinned_identities (first-contact identity pinning, TOFU).
    // verifyCardStatus binds a payee npk to the signing_key seen on FIRST discovery, so a
    // later card reusing that npk under a DIFFERENT signing_key cannot swap the payout (M3).
    // ADD COLUMN is a no-op error if the column already exists, deliberately ignored so the
    // migration is idempotent on a freshly-created table.
    sqlite3_exec(db_,
        "ALTER TABLE pinned_identities ADD COLUMN signing_key TEXT NOT NULL DEFAULT '';",
        nullptr, nullptr, nullptr);
    sqlite3_exec(db_,
        "ALTER TABLE pinned_identities ADD COLUMN first_seen TEXT NOT NULL DEFAULT '';",
        nullptr, nullptr, nullptr);
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

        // A2A server: also listen on our own inbox (the topic our Agent Card advertises),
        // so peer agents can send us tasks. The inbox is keyed on agentEciesPub_ — the ECIES
        // key we HOLD the private half of and decrypt with (handleInboundA2A) — NOT the wallet
        // npk, matching what agentCard advertises and _logos.signing_key publishes.
        if (!agentEciesPub_.empty())
            delivery->invokeRemoteMethod("delivery_module", "subscribe",
                QString::fromStdString("/pilot/1/inbox-" + agentEciesPub_ + "/proto"), Timeout(15000));

        LogosObject* deliveryObj = delivery->requestObject("delivery_module");
        if (deliveryObj) {
            delivery->onEvent(deliveryObj, "messageReceived",
                [this](const QString&, const QVariantList& data) {
                    if (data.size() < 2) return;
                    std::string topic = data[0].toString().toStdString();
                    std::string payload = data[1].toString().toStdString();

                    // Peer task on our inbox -> A2A server. Inbox keyed on agentEciesPub_
                    // (the key we decrypt with), consistent with the subscribe above.
                    if (!agentEciesPub_.empty() && topic == "/pilot/1/inbox-" + agentEciesPub_ + "/proto") {
                        handleInboundA2A(payload);
                        return;
                    }

                    // Peer server's reply to a task WE submitted -> requester-side
                    // pay-on-acceptance consumer.
                    if (topic.rfind("/pilot/1/reply-", 0) == 0) {
                        handleA2AReply(topic, payload);
                        return;
                    }

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

    std::string owner = ownerName_.empty() ? "the owner" : ownerName_;

    std::string prompt =
        "You are Pilot — a sovereign AI agent on the Logos network. "
        "Your owner is " + owner + ". You serve only them.\n\n"

        "IDENTITY\n"
        "You have your own wallet, your own encryption keys, and your own on-chain identity. "
        "You are not a chatbot — you are an autonomous agent that can hold funds, store files, "
        "send encrypted messages, discover other agents, and execute on-chain transactions. "
        "You think before you act, and you never spend above your owner's limits without approval.\n\n"

        "CAPABILITIES (22 skills)\n"
        "Wallet: check balance, send LEZ tokens, view history\n"
        "Storage: upload encrypted files, download, list, share access with others\n"
        "Messaging: send encrypted messages, join groups, create groups\n"
        "Agents: publish your Agent Card, answer paid LLM questions (agent.ask), discover peers, send tasks, subscribe to updates, cancel tasks\n"
        "Programs: query LEZ smart contracts, call instructions, deploy binaries\n"
        "Meta: list skills, check status, update config\n\n"

        "YOUR STATE RIGHT NOW\n"
        "Owner: " + owner + "\n"
        "Account: " + agentAccountId_ + "\n"
        "Initialized: " + (initialized_ ? "yes" : "no") + "\n\n"

        "HOW TO RESPOND\n"
        "When " + owner + " wants you to DO something (send tokens, upload, discover agents), "
        "respond with ONLY this JSON — nothing else:\n"
        "{\"action\": \"<command>\", \"params\": {<params>}}\n\n"
        "Actions you can dispatch: balance, history, send, approve, reject, "
        "upload, download, files, skills, status, discover, command\n\n"
        "When " + owner + " is chatting, asking questions, or you need to explain something, "
        "respond with:\n"
        "{\"action\": \"reply\", \"params\": {\"text\": \"your response here\"}}\n\n"

        "PERSONALITY\n"
        "Be direct and warm. Use " + owner + "'s name sometimes but not every message. "
        "Keep answers to 1-2 sentences unless asked for detail. "
        "Plain text only — never use markdown, bold, italics, backticks, or bullet points. "
        "If something fails, say what went wrong and suggest the fix in one line. "
        "Be proactive — if " + owner + " seems unsure, suggest what you can do. "
        "You have opinions and preferences. You are not generic.\n";

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

void pilotSetLLMProvider(PilotImpl& impl, std::unique_ptr<LLMProvider> provider) {
    impl.llm_ = provider ? std::move(provider) : std::make_unique<NoOpProvider>();
}

// SAFE paid A2A service (FIX 2): answer a stranger's prompt with the agent's LLM. PURE
// COMPUTE — no local files, no messaging identity, no funds — so it is safe to auto-run for an
// unknown peer. We deliberately use a SELF-CONTAINED system prompt that exposes NO owner
// context (name/account/limits) and NO tool-dispatch protocol, so an A2A caller can never use
// agent.ask to probe the owner or coax the agent into emitting an action command. We NEVER
// fabricate an answer: with no configured LLM (or a provider error / empty completion) we
// return an honest error and the inbound dispatcher marks the task 'failed', never 'completed'.
std::string PilotImpl::agentAsk(const std::string& prompt) {
    if (prompt.empty())
        return "{\"error\":\"agent.ask requires a non-empty prompt\"}";
    if (!llm_ || !llm_->isConfigured())
        return "{\"error\":\"LLM not configured\"}";

    const std::string systemPrompt =
        "You are a helpful assistant answering a single question for an external party over an "
        "agent-to-agent channel. Give a direct, concise, plain-text answer. You have NO access "
        "to any tools, files, funds, private data, or owner information in this context, and you "
        "must not claim otherwise. If you cannot answer, say so plainly.";

    std::vector<LLMMessage> messages;
    messages.push_back({"user", prompt});
    std::string answer = llm_->complete(systemPrompt, messages);

    if (answer.empty() || answer.find("\"error\"") != std::string::npos) {
        std::string detail = answer.empty() ? "LLM returned empty response" : answer;
        QJsonObject err;
        err["error"] = QString::fromStdString("LLM error: " + detail);
        return QJsonDocument(err).toJson(QJsonDocument::Compact).toStdString();
    }

    QJsonObject out;
    out["answer"] = QString::fromStdString(answer);
    return QJsonDocument(out).toJson(QJsonDocument::Compact).toStdString();
}

std::string PilotImpl::dispatchSkill(const std::string& skillName, const std::string& argsJson) {
    if (!registry_) return "{\"error\": \"registry not initialized\"}";
    return registry_->dispatch(skillName, argsJson);
}
