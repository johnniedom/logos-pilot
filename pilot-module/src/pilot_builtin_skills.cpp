#include "pilot_skill.h"
#include "pilot_impl.h"
#include <QJsonDocument>
#include <QJsonObject>

static void reg(SkillRegistry& r, const std::string& name, const std::string& cat,
                const std::string& desc, int64_t price,
                std::function<std::string(const std::string&)> fn) {
    r.registerSkill(std::make_unique<LambdaSkill>(
        name, cat, desc, "{}", "{}", price, std::move(fn)));
}

void registerBuiltinSkills(SkillRegistry& registry, PilotImpl* impl) {
    // Wallet (3)
    reg(registry, "wallet.balance", "wallet",
        "Returns the agent's current shielded token balance", 0,
        [impl](const std::string&) { return impl->walletBalance(); });

    reg(registry, "wallet.send", "wallet",
        "Sends LEZ tokens to a recipient, subject to spending threshold. Recipient forms: a "
        "payee's keys JSON or a private account id (paid from the agent's shielded account, "
        "needs a proof), or public:<64-hex account id> (paid from the agent's funded public "
        "account, no client proof)", 0,
        [impl](const std::string& args) {
            QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(args));
            QJsonObject obj = doc.object();
            return impl->walletSend(
                obj["recipient"].toString().toStdString(),
                obj["amount"].toInteger(),
                obj["reason"].toString().toStdString());
        });

    reg(registry, "wallet.history", "wallet",
        "Returns recent transaction history", 0,
        [impl](const std::string&) { return impl->walletHistory(); });

    // Storage (4)
    reg(registry, "storage.upload", "storage",
        "Encrypts and uploads a file to Logos Storage", 10,
        [impl](const std::string& args) {
            QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(args));
            QJsonObject obj = doc.object();
            return impl->storageUpload(
                obj["path"].toString().toStdString(),
                obj["label"].toString().toStdString());
        });

    reg(registry, "storage.download", "storage",
        "Retrieves and decrypts a file from Logos Storage", 5,
        [impl](const std::string& args) {
            QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(args));
            QJsonObject obj = doc.object();
            return impl->storageDownload(
                obj["cid"].toString().toStdString(),
                obj["path"].toString().toStdString());
        });

    reg(registry, "storage.list", "storage",
        "Lists all stored files with CIDs", 0,
        [impl](const std::string&) { return impl->storageList(); });

    reg(registry, "storage.share", "storage",
        "Shares access to a stored file with another Logos identity", 5,
        [impl](const std::string& args) {
            QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(args));
            QJsonObject obj = doc.object();
            return impl->storageShare(
                obj["cid"].toString().toStdString(),
                obj["recipient_npk"].toString().toStdString());
        });

    // Messaging (3)
    reg(registry, "messaging.send", "messaging",
        "Sends an encrypted message to a Logos address", 1,
        [impl](const std::string& args) {
            QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(args));
            QJsonObject obj = doc.object();
            return impl->messagingSend(
                obj["recipient"].toString().toStdString(),
                obj["message"].toString().toStdString());
        });

    reg(registry, "messaging.join", "messaging",
        "Joins a group messaging channel", 0,
        [impl](const std::string& args) {
            QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(args));
            QJsonObject obj = doc.object();
            bool ok = impl->messagingJoin(obj["group_id"].toString().toStdString());
            return ok ? "{\"joined\": true}" : "{\"joined\": false}";
        });

    reg(registry, "messaging.create_group", "messaging",
        "Creates a new group messaging channel", 0,
        [impl](const std::string& args) {
            QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(args));
            QJsonObject obj = doc.object();
            return impl->messagingCreateGroup(
                obj["members"].toString().toStdString());
        });

    // Agent / A2A (6)
    reg(registry, "agent.card", "agent",
        "Publishes and returns the agent's A2A Agent Card", 0,
        [impl](const std::string&) { return impl->agentCard(); });

    // SAFE paid A2A service (FIX 2): pure-compute LLM Q&A. No local files, no messaging
    // identity, no funds — safe to auto-run for an unknown peer. Price matches the SAFE
    // service catalog (a2aServiceCatalog) so advertised price == auto-serviced price.
    reg(registry, "agent.ask", "agent",
        "Answers a prompt with the agent's LLM (pure compute; no files, messaging, or funds)", 5,
        [impl](const std::string& args) {
            QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(args));
            QJsonObject obj = doc.object();
            return impl->agentAsk(obj["prompt"].toString().toStdString());
        });

    // Out-of-band peer introduction. Same verification as discovery; the card just
    // arrives by file or paste instead of over Waku.
    reg(registry, "agent.import_card", "agent",
        "Learns a peer from its signed Agent Card, handed over out-of-band", 0,
        [impl](const std::string& args) {
            QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(args));
            QJsonObject obj = doc.object();
            // Accept either {"card": {...}} or the card object itself, so a pasted card
            // works without the owner having to wrap it.
            QJsonObject card = obj.contains("card") ? obj["card"].toObject() : obj;
            return impl->agentImportCard(
                QJsonDocument(card).toJson(QJsonDocument::Compact).toStdString());
        });

    reg(registry, "agent.discover", "agent",
        "Discovers peer agents on the network", 0,
        [impl](const std::string& args) {
            QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(args));
            QJsonObject obj = doc.object();
            return impl->agentDiscover(obj["topic"].toString().toStdString());
        });

    reg(registry, "agent.task", "agent",
        "Sends a task request to another agent", 0,
        [impl](const std::string& args) {
            QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(args));
            QJsonObject obj = doc.object();
            return impl->agentTask(
                obj["agent_address"].toString().toStdString(),
                obj["skill"].toString().toStdString(),
                obj["params"].toString().toStdString());
        });

    reg(registry, "agent.subscribe", "agent",
        "Subscribes to task status updates from another agent", 0,
        [impl](const std::string& args) {
            QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(args));
            QJsonObject obj = doc.object();
            return impl->agentSubscribe(
                obj["agent_address"].toString().toStdString(),
                obj["task_id"].toString().toStdString());
        });

    reg(registry, "agent.cancel", "agent",
        "Cancels a task on another agent", 0,
        [impl](const std::string& args) {
            QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(args));
            QJsonObject obj = doc.object();
            bool ok = impl->agentCancel(
                obj["agent_address"].toString().toStdString(),
                obj["task_id"].toString().toStdString());
            return ok ? "{\"cancelled\": true}" : "{\"cancelled\": false}";
        });

    // Program / Blockchain (3)
    reg(registry, "program.query", "program",
        "Reads state from a LEZ program", 0,
        [impl](const std::string& args) {
            QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(args));
            QJsonObject obj = doc.object();
            return impl->programQuery(
                obj["program_id"].toString().toStdString(),
                obj["params"].toString().toStdString());
        });

    reg(registry, "program.call", "program",
        "Submits a transaction to a LEZ program", 10,
        [impl](const std::string& args) {
            QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(args));
            QJsonObject obj = doc.object();
            return impl->programCall(
                obj["program_id"].toString().toStdString(),
                obj["instruction"].toString().toStdString(),
                obj["params"].toString().toStdString());
        });

    reg(registry, "program.deploy", "program",
        "Deploys a compiled LEZ program binary to the network", 100,
        [impl](const std::string& args) {
            QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(args));
            QJsonObject obj = doc.object();
            return impl->programDeploy(
                obj["binary_path"].toString().toStdString());
        });

    // Meta (3)
    reg(registry, "meta.skills", "meta",
        "Lists all available skills", 0,
        [impl](const std::string&) { return impl->metaSkills(); });

    reg(registry, "meta.status", "meta",
        "Returns agent status and configuration", 0,
        [impl](const std::string&) { return impl->metaStatus(); });

    reg(registry, "meta.configure", "meta",
        "Updates agent configuration", 0,
        [impl](const std::string& args) {
            QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(args));
            QJsonObject obj = doc.object();
            bool ok = impl->metaConfigure(
                obj["key"].toString().toStdString(),
                obj["value"].toString().toStdString());
            return ok ? "{\"configured\": true}" : "{\"configured\": false}";
        });
}
