// pilot_owner_impl.cpp — see pilot_owner_impl.h. JSON in, JSON out; OwnerClient does the work.
#include "pilot_owner_impl.h"

#include "owner_client.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

std::string compact(const QJsonObject& o) {
    return QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString();
}

std::string failure(const std::string& err) {
    QJsonObject o;
    o["ok"] = false;
    o["error"] = qs(err);
    return compact(o);
}

constexpr int64_t kSecondNs = 1000000000LL;

}  // namespace

PilotOwnerImpl::PilotOwnerImpl()
    : client_(std::make_unique<OwnerClient>()), lastPollNs_(0) {}

PilotOwnerImpl::~PilotOwnerImpl() = default;

std::string PilotOwnerImpl::echo(const std::string& text) {
    return "echo: " + text;
}

std::string PilotOwnerImpl::createKey() {
    std::string pub, err;
    bool created = false;
    if (!client_->init(false, pub, created, err)) return failure(err);
    QJsonObject o;
    o["ok"] = true;
    o["created"] = created;
    o["owner_pub"] = qs(pub);
    o["bind"] = qs("PILOT_OWNER_NPK=" + pub + " pilot deploy");
    o["bind_alt"] = qs("logoscore call pilot metaConfigure owner.npk " + pub);
    o["state"] = qs(client_->statePath());
    return compact(o);
}

std::string PilotOwnerImpl::importKey(const std::string& privColonPub) {
    std::string pub, err;
    if (!client_->importKey(privColonPub, pub, err)) return failure(err);
    QJsonObject o;
    o["ok"] = true;
    o["owner_pub"] = qs(pub);
    o["state"] = qs(client_->statePath());
    return compact(o);
}

std::string PilotOwnerImpl::pair(const std::string& cardJsonOrPath, const std::string& accountId,
                                 const std::string& relayUrl) {
    std::string err;
    if (!client_->pair(cardJsonOrPath, accountId, relayUrl, err)) return failure(err);
    QJsonObject o;
    o["ok"] = true;
    o["agent_name"] = qs(client_->field("agent_name"));
    o["agent_key"] = qs(client_->field("agent_key"));
    o["topic"] = qs(client_->field("topic"));
    o["relay"] = qs(client_->field("relay"));
    return compact(o);
}

std::string PilotOwnerImpl::send(const std::string& text) {
    int64_t nonce = 0;
    size_t sealedBytes = 0;
    std::string err;
    if (!client_->send(text, nonce, sealedBytes, err)) return failure(err);
    QJsonObject o;
    o["ok"] = true;
    o["nonce"] = static_cast<double>(nonce);
    o["sealed_bytes"] = static_cast<double>(sealedBytes);
    o["topic"] = qs(client_->field("topic"));
    return compact(o);
}

std::string PilotOwnerImpl::poll() {
    // Window: since the previous poll minus a minute of overlap (the relay may file a message a
    // little late); the first poll looks 15 minutes back. The seen-hash list in the state file
    // keeps the overlap idempotent, so a reply is handed out exactly once.
    const int64_t now = OwnerClient::nowNs();
    const int64_t since = lastPollNs_ == 0 ? now - 900 * kSecondNs : lastPollNs_ - 60 * kSecondNs;
    std::vector<OwnerReply> replies;
    std::string err;
    if (!client_->poll(since, replies, err)) return failure(err);
    lastPollNs_ = now;
    QJsonArray arr;
    for (const OwnerReply& r : replies) {
        QJsonObject m;
        m["text"] = qs(r.text);
        m["time_ns"] = static_cast<double>(r.timestampNs);
        m["hash"] = qs(r.hash);
        arr.append(m);
    }
    QJsonObject o;
    o["ok"] = true;
    o["count"] = static_cast<int>(replies.size());
    o["replies"] = arr;
    return compact(o);
}

std::string PilotOwnerImpl::status() {
    return client_->statusJson();
}
