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
#include <algorithm>
#include <cstdlib>
#include <sys/stat.h>
#include <thread>
#include <chrono>
#include <QString>
#include <QByteArray>
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

    // L8 — safety margin on the WAL RESERVED lock. Production uses a single db_ connection (no
    // self-contention), so this is not a runtime fix; it hardens any future second writer and
    // prevents spurious SQLITE_BUSY in test inspector connections. The atomicity guarantee of the
    // new settlement / inbound-wallet-send transactions comes from BEGIN IMMEDIATE/COMMIT, not this.
    sqlite3_busy_timeout(db_, 5000);

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
            expires_at TEXT NOT NULL,
            tx_hash TEXT NOT NULL DEFAULT ''
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

        -- Every message delivery_module hands us, recorded BEFORE any routing decision.
        -- There is no other way to know whether the handoff happens at all: the module logs
        -- nothing (zero [pilot] lines in a 550KB daemon log, measured 2026-07-27), and
        -- delivery_module logs a send but never a subscribe. Measured that night: a task
        -- reached the doer's NODE (same msgHash on both sides) and its agent never acted on
        -- it. This table distinguishes the two possible reasons — our callback never fires at
        -- all, versus it fires but not for our topics — which point at completely different
        -- fixes (ours vs upstream).
        CREATE TABLE IF NOT EXISTS delivery_events (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            topic TEXT NOT NULL,
            bytes INTEGER NOT NULL,
            received_at TEXT NOT NULL
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

    // L7 — record the on-chain transfer hash atomically with the terminal spend write (audit +
    // future status query). Duplicate-column error on a fresh DB (CREATE already added it) is
    // deliberately ignored, exactly like every other ADD COLUMN migration above.
    sqlite3_exec(db_,
        "ALTER TABLE spend_requests ADD COLUMN tx_hash TEXT NOT NULL DEFAULT '';",
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

        // The agent's own inbox subscriptions do NOT belong here. Their topic names contain the
        // agent's own keys, and this function runs BEFORE the identity is loaded — it found empty
        // keys, skipped both subscribes, and its run-once guard meant it never tried again, so the
        // agent published a card naming an address it was not listening on (measured 2026-07-26).
        // They live in subscribeIdentityTopics(), called once an identity actually exists.

        LogosObject* deliveryObj = delivery->requestObject("delivery_module");
        if (deliveryObj) {
            delivery->onEvent(deliveryObj, "messageReceived",
                [this](const QString&, const QVariantList& data) {
                    if (data.size() < 2) return;
                    std::string topic = data[0].toString().toStdString();
                    std::string payload = data[1].toString().toStdString();

                    // Record the handoff FIRST, before any branch can drop the message. A row
                    // here proves delivery_module reached our code; no rows at all proves it
                    // never did, whatever its subscribe call reported. Deliberately ahead of
                    // every topic test so a message for an unknown topic still leaves a trace.
                    if (db_) {
                        sqlite3_stmt* ev = nullptr;
                        if (sqlite3_prepare_v2(db_,
                                "INSERT INTO delivery_events (topic, bytes, received_at) "
                                "VALUES (?, ?, ?);", -1, &ev, nullptr) == SQLITE_OK) {
                            // Self-contained: nowTimestamp() is local to pilot_a2a.cpp.
                            std::string ts = std::to_string(
                                std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::system_clock::now().time_since_epoch()).count());
                            sqlite3_bind_text(ev, 1, topic.c_str(), -1, SQLITE_TRANSIENT);
                            sqlite3_bind_int64(ev, 2, static_cast<sqlite3_int64>(payload.size()));
                            sqlite3_bind_text(ev, 3, ts.c_str(), -1, SQLITE_TRANSIENT);
                            sqlite3_step(ev);
                            sqlite3_finalize(ev);
                        }
                    }

                    // Peer task on EITHER of our inboxes -> A2A server (L1). The primary inbox is
                    // keyed on the enc key (a2aSelfEncKey()); the legacy signing-key inbox is also
                    // accepted for pre-split peers. handleInboundA2A decrypts with either private
                    // key (a2aTryDecrypt), consistent with the dual subscribe above.
                    std::string selfEnc = a2aSelfEncKey();
                    if ((!selfEnc.empty() && topic == "/pilot/1/inbox-" + selfEnc + "/proto") ||
                        (!agentEciesPub_.empty() && topic == "/pilot/1/inbox-" + agentEciesPub_ + "/proto")) {
                        handleInboundA2A(payload);
                        return;
                    }

                    // Peer server's reply to a task WE submitted -> requester-side
                    // pay-on-acceptance consumer.
                    if (topic.rfind("/pilot/1/reply-", 0) == 0) {
                        handleA2AReply(topic, payload);
                        return;
                    }

                    // A peer's Agent Card broadcast on the shared discovery channel. Without
                    // this branch the message was received and dropped, so a card could only
                    // ever be learned by store query (there is no archive service in this
                    // topology) or by out-of-band import — which is why discovery returned
                    // {"count":0} forever.
                    if (topic == "/pilot/1/discovery/proto") {
                        handleDiscoveryCard(payload);
                        return;
                    }

                    if (topic != ownerChannelId_ || agentEciesPriv_.empty()) return;

                    try {
                        ECIESCiphertext ct = eciesDeserialize(payload);
                        std::vector<uint8_t> plain = eciesDecrypt(agentEciesPriv_, ct);
                        std::string message(plain.begin(), plain.end());
                        // M1: authenticate the owner payload FAIL-OPEN. Raw/legacy text is accepted
                        // (owner never locked out); a SIGNED envelope must pass signature + TOFU pin
                        // + replay nonce, else it is dropped silently (no LLM processing / no cost).
                        std::string inner;
                        if (verifyOwnerMessage(message, inner)) {
                            std::string response = processOwnerMessage(inner);
                            sendToOwner(response);
                        }
                    } catch (...) {}
                });
        }
    }
}

// The topics whose NAMES depend on our own identity keys. Kept pure and separate from the
// subscribing so the one invariant that matters — we listen where our card says we listen —
// can be asserted without a delivery module (nothing in this suite mocks one).
std::vector<std::string> PilotImpl::identityTopics() {
    std::vector<std::string> topics;

    // Always on. This is the shared channel peers broadcast their Agent Cards to, and hearing
    // one is passive — it tells a stranger nothing about us. Listening here is what lets a card
    // broadcast BETWEEN discovery polls reach us at all, and it is how a CLOSED agent still
    // learns who it might want to hire.
    topics.push_back("/pilot/1/discovery/proto");

    // Only while the owner has us open for hire: our own inbox(es), the addresses a stranger
    // sends work to. Closed means nobody can reach us here.
    if (!openForHire_) return topics;

    // L1 key separation: the PRIMARY inbox is keyed on the dedicated ENCRYPTION key
    // (a2aSelfEncKey(), advertised as _logos.enc_key) — the key new peers encrypt to. The legacy
    // SIGNING-key inbox (agentEciesPub_) is also listened on when it differs, so a pre-split peer
    // still routing to _logos.signing_key keeps reaching us (a2aTryDecrypt handles either).
    const std::string encKey = a2aSelfEncKey();
    if (!encKey.empty())
        topics.push_back("/pilot/1/inbox-" + encKey + "/proto");
    if (!agentEciesPub_.empty() && agentEciesPub_ != encKey)
        topics.push_back("/pilot/1/inbox-" + agentEciesPub_ + "/proto");
    return topics;
}

// Did delivery_module accept this call? Same shape as deliverToOwner's check: a JSON reply
// carrying success:false or a non-empty error is a refusal; anything else non-null counts.
static bool deliveryAccepted(const QVariant& v) {
    if (v.isNull()) return false;
    const QString s = v.toString();
    const QJsonDocument d = QJsonDocument::fromJson(s.toUtf8());
    if (d.isObject()) {
        const QJsonObject o = d.object();
        if (o.contains("success")) return o.value("success").toBool();
        if (o.contains("error") && !o.value("error").toString().isEmpty()) return false;
    }
    return true;
}

void PilotImpl::subscribeIdentityTopics() {
    auto* delivery = logosAPI_ ? logosAPI_->getClient("delivery_module") : nullptr;
    if (!delivery || !delivery->isConnected()) return;

    const std::vector<std::string> want = identityTopics();

    // Subscribe what we want and have not already asked for. A topic is recorded ONLY when
    // delivery_module confirms it: subscribedTopics() is the agent's answer to "where are you
    // actually listening", and it would be worthless if it recorded intentions. This matters
    // because delivery_module does NOT log subscribe calls (it logs sends) — measured
    // 2026-07-27 by subscribing a canary topic directly and finding it nowhere in the log —
    // so its confirmation here is the only evidence that exists.
    for (const std::string& topic : want) {
        if (std::find(subscribedTopics_.begin(), subscribedTopics_.end(), topic)
                != subscribedTopics_.end())
            continue;
        QVariant r = delivery->invokeRemoteMethod("delivery_module", "subscribe",
            QString::fromStdString(topic), Timeout(15000));
        if (deliveryAccepted(r))
            subscribedTopics_.push_back(topic);
        else
            qWarning() << "[pilot] subscribeIdentityTopics: delivery REFUSED"
                       << QString::fromStdString(topic);
    }

    // Drop what we hold but no longer want. This is what makes closing for hire REAL rather
    // than cosmetic: without it a closed agent would keep receiving strangers' tasks on an
    // inbox it had already stopped advertising.
    for (auto it = subscribedTopics_.begin(); it != subscribedTopics_.end(); ) {
        if (std::find(want.begin(), want.end(), *it) != want.end()) { ++it; continue; }
        delivery->invokeRemoteMethod("delivery_module", "unsubscribe",
            QString::fromStdString(*it), Timeout(15000));
        it = subscribedTopics_.erase(it);
    }
}

std::vector<std::string> PilotImpl::subscribedTopics() {
    return subscribedTopics_;
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

        "CAPABILITIES (23 skills)\n"
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
        "ACTION PARAMETERS — use these EXACT param keys. Never invent, rename, or omit them:\n"
        "  balance | history | files | skills | status | pending : {}\n"
        "  send     : {\"recipient\": \"<npk-or-account>\", \"amount\": <whole number, not a string>, \"reason\": \"<why>\"}\n"
        "  approve  : {\"id\": \"<spend-request-id>\"}\n"
        "  reject   : {\"id\": \"<spend-request-id>\"}\n"
        "  upload   : {\"path\": \"<absolute path of the file to store>\", \"label\": \"<short name>\"}\n"
        "  download : {\"cid\": \"<label-or-CID>\", \"path\": \"<absolute path to WRITE the file to on disk>\"}\n"
        "  discover : {}\n"
        "  command  : {\"raw\": \"/<slash-command>\"}\n"
        "download WRITES the file to \"path\" on the local disk; ALWAYS include \"path\" or it silently "
        "defaults to /tmp/download. If a required parameter is missing, ask for it in a reply — never "
        "guess a value, and never claim a skill cannot do something it can.\n\n"
        "MANDATORY RULES — follow these exactly; they govern every action:\n"
        "1. To DO anything, EMIT THE ACTION JSON. Never reply with text like \"let me "
        "download it\", \"I'll do that now\", or \"downloading...\" — a text reply runs "
        "NOTHING. If you intend to act, emit the action; never just describe it.\n"
        "2. For download, pass whatever the owner names (a label OR a CID) directly as "
        "\"cid\". NEVER ask the owner for a CID, and NEVER say a file \"doesn't exist\", "
        "\"isn't on the network\", or \"came back unknown\" — the system resolves names to "
        "CIDs and reports the real result. Just emit the download action with the name given.\n"
        "3. NEVER invent the outcome of an action. You do NOT know whether it succeeded "
        "until the system runs it and reports back. Do not fabricate errors such as "
        "\"unknown CID\", \"the network is rejecting it\", \"different vault\", or \"storage "
        "problem\". Emit the action and let the real result speak.\n"
        "4. \"download/upload X into Y\" means: emit the action immediately with X as the "
        "file reference and Y as the path. Only ask a question when a REQUIRED parameter is "
        "genuinely missing (for example, no destination path was given at all).\n\n"
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

namespace {
// L6: serialize/bound blocking LLM calls on the delivery thread. The 'concurrency' guarded is a
// nested QEventLoop (inside complete()) pumping a fresh inbound agent-ask back into this object,
// not OS threads, so a non-atomic int is correct. Bound 1 = strict serialize of the billable path.
constexpr int kMaxConcurrentLLMCalls = 1;
struct InFlightGuard {
    int& c;
    explicit InFlightGuard(int& counter) : c(counter) { ++c; }
    ~InFlightGuard() { --c; }
};
}  // namespace

// M1 — owner-channel authentication, FAIL-OPEN. See pilot_impl.h for the full contract.
//
// FAIL-OPEN: the owner clients (pilot-cli, pilot-ui) currently send RAW TEXT and do NOT sign,
// so an unsigned/non-envelope payload is ACCEPTED unchanged — the owner is NEVER locked out.
// A client that opts in to signing gets the hardened path: signature + TOFU pin + replay nonce,
// any failure -> drop. Flipping to fail-closed (reject unsigned) requires the owner clients to
// sign first; that is a documented follow-up, NOT done here.
bool PilotImpl::verifyOwnerMessage(const std::string& raw, std::string& innerOut) {
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(raw));
    if (doc.isObject()) {
        QJsonObject env = doc.object();
        QJsonObject logos = env["_logos"].toObject();
        const bool isSignedEnvelope =
            env.contains("message") && env["message"].isString() &&
            logos.contains("signing_key") && logos["signing_key"].isString() &&
            logos.contains("signature")   && logos["signature"].isString();

        if (isSignedEnvelope) {
            // From here this is a SIGNED ENVELOPE: ANY failed check returns false (caller drops it).
            const std::string key = logos["signing_key"].toString().toStdString();
            const std::string sig = logos["signature"].toString().toStdString();

            // (a) Canonical bytes = the WHOLE envelope with _logos.signature REMOVED (signing_key
            // kept), serialized Compact — EXACTLY signA2AEnvelope / verifyInboundRequest. We then
            // verify with verifySignature(message_bytes, signatureHex, publicKeyHex).
            QJsonObject canonEnv = env;
            QJsonObject canonLogos = canonEnv["_logos"].toObject();
            canonLogos.remove("signature");
            canonEnv["_logos"] = canonLogos;
            const std::string canonical =
                QJsonDocument(canonEnv).toJson(QJsonDocument::Compact).toStdString();
            const std::vector<uint8_t> bytes(canonical.begin(), canonical.end());
            if (!verifySignature(bytes, sig, key)) return false;   // (message, signatureHex, publicKeyHex)

            // (b) TOFU-pin the owner signing key in the config table under "owner.signing_key"
            // (SEPARATE from the A2A pin tables). INSERT OR IGNORE creates it on first signed
            // contact; thereafter the stored value MUST equal the presented key (mismatch -> drop).
            if (db_) {
                sqlite3_stmt* ins = nullptr;
                if (sqlite3_prepare_v2(db_,
                        "INSERT OR IGNORE INTO config (key, value) VALUES ('owner.signing_key', ?);",
                        -1, &ins, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(ins, 1, key.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_step(ins);
                }
                sqlite3_finalize(ins);

                std::string pinned;
                sqlite3_stmt* sel = nullptr;
                if (sqlite3_prepare_v2(db_,
                        "SELECT value FROM config WHERE key='owner.signing_key';",
                        -1, &sel, nullptr) == SQLITE_OK) {
                    if (sqlite3_step(sel) == SQLITE_ROW && sqlite3_column_text(sel, 0))
                        pinned = reinterpret_cast<const char*>(sqlite3_column_text(sel, 0));
                }
                sqlite3_finalize(sel);
                if (pinned.empty() || pinned != key) return false;   // TOFU mismatch -> drop
            }

            // (c) Replay protection: integer _logos.nonce MUST strictly exceed the stored
            // "owner.last_nonce" (default 0). A missing / non-increasing nonce on a signed
            // envelope -> drop. Persist the new high-water nonce on success.
            if (!logos.contains("nonce") || !logos["nonce"].isDouble()) return false;
            const long long nonce = static_cast<long long>(logos["nonce"].toDouble());
            long long last = 0;
            if (db_) {
                sqlite3_stmt* sel = nullptr;
                if (sqlite3_prepare_v2(db_,
                        "SELECT value FROM config WHERE key='owner.last_nonce';",
                        -1, &sel, nullptr) == SQLITE_OK) {
                    if (sqlite3_step(sel) == SQLITE_ROW && sqlite3_column_text(sel, 0))
                        last = std::strtoll(
                            reinterpret_cast<const char*>(sqlite3_column_text(sel, 0)), nullptr, 10);
                }
                sqlite3_finalize(sel);
            }
            if (nonce <= last) return false;   // replay / equal / lower -> drop
            if (db_) {
                sqlite3_stmt* up = nullptr;
                if (sqlite3_prepare_v2(db_,
                        "INSERT OR REPLACE INTO config (key, value) VALUES ('owner.last_nonce', ?);",
                        -1, &up, nullptr) == SQLITE_OK) {
                    const std::string ns = std::to_string(nonce);
                    sqlite3_bind_text(up, 1, ns.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_step(up);
                }
                sqlite3_finalize(up);
            }

            // (d) signature + pin + nonce all passed -> accept the inner message.
            innerOut = env["message"].toString().toStdString();
            return true;
        }
    }

    // FAIL-OPEN: raw text, a non-object, or an object without a _logos.signature is a legacy
    // unsigned owner message and is accepted unchanged so the owner is never locked out.
    innerOut = raw;
    return true;
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

    // I1: build the LLM request from a SNAPSHOT of history plus this turn, appending the new
    // user message ONLY to the local request vector — do NOT mutate shared chatHistory_ yet.
    // complete() blocks on a nested QEventLoop that keeps pumping the delivery thread, so a
    // re-entrant owner message can run processOwnerMessage() again while we are parked here.
    // Committing the user turn now would let that re-entrant call interleave its push_backs
    // between our user message and our assistant reply, breaking role alternation.
    std::vector<LLMMessage> messages;
    for (const auto& [role, content] : chatHistory_)
        messages.push_back({role, content});
    messages.push_back({"user", message});

    // L6: the owner's blocking call registers as in-flight (so a nested inbound agent-ask is
    // refused by the bound) but the owner is NEVER itself refused — owner is privileged.
    InFlightGuard _inflight(llmInFlight_);
    std::string response = llm_->complete(systemPrompt, messages);

    // I1: commit this turn AFTER complete() as one contiguous (user, assistant) unit. A
    // re-entrant call that ran during complete() has already appended its own complete pair,
    // so ours lands after it with alternation intact.
    chatHistory_.push_back({"user", message});
    if (!response.empty() && response.find("\"error\"") == std::string::npos)
        chatHistory_.push_back({"assistant", response});
    while (chatHistory_.size() > 40)
        chatHistory_.erase(chatHistory_.begin(), chatHistory_.begin() + 2);

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

    // L6 — bound concurrent in-flight LLM calls so an inbound agent-ask FLOOD can never nest
    // QEventLoops on the single delivery thread. Refuse BEFORE starting a second blocking call.
    // Honest error -> a2aResultIsSuccess()==false -> inbound dispatcher marks the task 'failed'
    // (peer retries), mirroring the H3/M3 rate-limit contract.
    if (llmInFlight_ >= kMaxConcurrentLLMCalls)
        return "{\"error\":\"LLM busy; concurrency limit reached, retry later\"}";

    const std::string systemPrompt =
        "You are a helpful assistant answering a single question for an external party over an "
        "agent-to-agent channel. Give a direct, concise, plain-text answer. You have NO access "
        "to any tools, files, funds, private data, or owner information in this context, and you "
        "must not claim otherwise. If you cannot answer, say so plainly.";

    std::vector<LLMMessage> messages;
    messages.push_back({"user", prompt});
    InFlightGuard _inflight(llmInFlight_);
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
