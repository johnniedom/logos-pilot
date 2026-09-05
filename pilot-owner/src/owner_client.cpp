// owner_client.cpp — see owner_client.h. Qt JSON for the agent's canonical bytes, Qt Network for
// the relay's REST API; the state file is the only thing that persists.
#include "owner_client.h"

#include <QByteArray>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <sys/stat.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

QJsonObject loadState(const std::string& path) {
    QFile f(qs(path));
    if (!f.open(QIODevice::ReadOnly)) return {};
    QJsonDocument d = QJsonDocument::fromJson(f.readAll());
    return d.isObject() ? d.object() : QJsonObject();
}

bool saveState(const std::string& dir, const std::string& path, const QJsonObject& st) {
    QDir().mkpath(qs(dir));
    QFile f(qs(path));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(QJsonDocument(st).toJson(QJsonDocument::Indented));
    f.close();
    chmod(path.c_str(), 0600);   // the owner's private key lives here
    return true;
}

std::string compact(const QJsonObject& o) {
    return QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString();
}

// Blocking HTTP with a deadline. Returns the body; *status = HTTP code, 0 when nothing came back.
// A nested event loop: both front-ends run inside a Qt application (the console client, the
// module host), the same way the agent module itself reads the relay store.
QByteArray http(const char* verb, const QString& url, const QByteArray& body, int timeoutMs, int* status) {
    QNetworkAccessManager mgr;
    QNetworkRequest req{QUrl(url)};
    QNetworkReply* reply = nullptr;
    if (std::strcmp(verb, "POST") == 0) {
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        reply = mgr.post(req, body);
    } else {
        reply = mgr.get(req);
    }
    QEventLoop loop;
    QTimer deadline; deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    deadline.start(timeoutMs);
    loop.exec();
    QByteArray data;
    int code = 0;
    if (reply->isFinished()) {
        code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        data = reply->readAll();
    } else {
        reply->abort();
    }
    if (status) *status = code;
    reply->deleteLater();
    return data;
}

ECIESKeypair ownerKeyFrom(const QJsonObject& st) {
    ECIESKeypair k;
    k.publicKeyHex = st["owner_pub"].toString().toStdString();
    k.privateKeyHex = st["owner_priv"].toString().toStdString();
    return k;
}

bool readyFrom(const QJsonObject& st, std::string& why) {
    if (st["owner_priv"].toString().isEmpty()) { why = "no owner key yet: create or import one first"; return false; }
    if (st["agent_key"].toString().isEmpty() || st["topic"].toString().isEmpty()) {
        why = "not paired with an agent yet: pair with its card and account id first"; return false;
    }
    return true;
}

std::string trimmed(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

}  // namespace

OwnerClient::OwnerClient(std::string stateDir)
    : dir_(stateDir.empty() ? defaultStateDir() : std::move(stateDir)) {}

std::string OwnerClient::defaultStateDir() {
    if (const char* h = std::getenv("PILOT_OWNER_HOME"); h && *h) return h;
    return QDir::homePath().toStdString() + "/.pilot-owner";
}

std::string OwnerClient::statePath() const { return dir_ + "/state.json"; }

int64_t OwnerClient::nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
int64_t OwnerClient::nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

int64_t OwnerClient::nextNonce(int64_t lastNonce, int64_t nowMs) {
    return std::max<int64_t>(lastNonce + 1, nowMs);
}

std::string OwnerClient::topicFor(const std::string& accountId) {
    return "/pilot/1/owner-" + accountId + "/proto";
}

std::string OwnerClient::buildEnvelope(const std::string& text, const ECIESKeypair& owner, int64_t nonce) {
    QJsonObject env;
    env["message"] = qs(text);
    QJsonObject logos;
    logos["signing_key"] = qs(owner.publicKeyHex);
    logos["nonce"] = static_cast<double>(nonce);
    env["_logos"] = logos;
    std::string canonical = compact(env);
    std::vector<uint8_t> bytes(canonical.begin(), canonical.end());
    logos["signature"] = qs(signMessage(bytes, owner.privateKeyHex));
    env["_logos"] = logos;
    return compact(env);
}

bool OwnerClient::verifyEnvelope(const std::string& raw, std::string& messageOut, std::string& keyOut) {
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(raw));
    if (!doc.isObject()) return false;
    QJsonObject env = doc.object();
    QJsonObject logos = env["_logos"].toObject();
    std::string key = logos["signing_key"].toString().toStdString();
    std::string sig = logos["signature"].toString().toStdString();
    if (key.empty() || sig.empty() || !env["message"].isString()) return false;
    QJsonObject canonLogos = logos; canonLogos.remove("signature");
    QJsonObject canonEnv = env; canonEnv["_logos"] = canonLogos;
    std::string canonical = compact(canonEnv);
    std::vector<uint8_t> bytes(canonical.begin(), canonical.end());
    if (!verifySignature(bytes, sig, key)) return false;
    messageOut = env["message"].toString().toStdString();
    keyOut = key;
    return true;
}

std::string OwnerClient::sealForAgent(const std::string& envelope, const std::string& agentKeyHex) {
    std::vector<uint8_t> plain(envelope.begin(), envelope.end());
    return eciesSerialize(eciesEncrypt(agentKeyHex, plain));
}

std::string OwnerClient::storeUrl(const std::string& relay, const std::string& topic, int64_t sinceNs) {
    QUrl url(qs(relay + "/store/v3/messages"));
    QUrlQuery q;
    q.addQueryItem("includeData", "true");
    q.addQueryItem("contentTopics", qs(topic));
    q.addQueryItem("startTime", QString::number(static_cast<qlonglong>(sinceNs)));
    q.addQueryItem("pageSize", "100");
    q.addQueryItem("ascending", "true");
    url.setQuery(q);
    return url.toString(QUrl::FullyEncoded).toStdString();
}

std::vector<OwnerReply> OwnerClient::repliesFromStoreBody(const std::string& body,
                                                          const std::string& ownerPrivHex,
                                                          std::vector<std::string>& seen) {
    std::vector<OwnerReply> out;
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(body));
    if (!doc.isObject()) return out;
    for (const QJsonValue& v : doc.object().value("messages").toArray()) {
        QJsonObject m = v.toObject();
        std::string hash = m.value("messageHash").toString().toStdString();
        if (hash.empty() || std::find(seen.begin(), seen.end(), hash) != seen.end()) continue;
        seen.push_back(hash);
        QJsonObject msg = m.value("message").toObject();
        std::string payload = QByteArray::fromBase64(msg.value("payload").toString().toLatin1()).toStdString();
        try {
            std::vector<uint8_t> plain = eciesDecrypt(ownerPrivHex, eciesDeserialize(payload));
            OwnerReply r;
            r.text.assign(plain.begin(), plain.end());
            r.hash = hash;
            r.timestampNs = static_cast<int64_t>(msg.value("timestamp").toDouble());
            out.push_back(std::move(r));
        } catch (...) {
            // Not for us: our own sealed send, or a payload sealed to someone else.
        }
    }
    return out;
}

bool OwnerClient::init(bool force, std::string& ownerPubOut, bool& createdOut, std::string& err) {
    QJsonObject st = loadState(statePath());
    createdOut = false;
    if (!st["owner_priv"].toString().isEmpty() && !st["owner_pub"].toString().isEmpty() && !force) {
        ownerPubOut = st["owner_pub"].toString().toStdString();
        return true;
    }
    ECIESKeypair kp = generateECIESKeypair();
    st["owner_pub"] = qs(kp.publicKeyHex);
    st["owner_priv"] = qs(kp.privateKeyHex);
    if (!st.contains("last_nonce")) st["last_nonce"] = 0.0;
    if (!saveState(dir_, statePath(), st)) { err = "cannot write " + statePath(); return false; }
    ownerPubOut = kp.publicKeyHex;
    createdOut = true;
    return true;
}

bool OwnerClient::importKey(const std::string& privColonPub, std::string& ownerPubOut, std::string& err) {
    std::string s = trimmed(privColonPub);
    size_t c = s.find(':');
    if (c == std::string::npos || c == 0 || c + 1 >= s.size()) {
        err = "import expects '<priv hex>:<pub hex>' (the pair the agent was bound to)"; return false;
    }
    ECIESKeypair kp;
    kp.privateKeyHex = trimmed(s.substr(0, c));
    kp.publicKeyHex = trimmed(s.substr(c + 1));
    // The two halves must belong together: seal to the public half, open with the private one.
    try {
        std::vector<uint8_t> probe{'o', 'k'};
        std::vector<uint8_t> back = eciesDecrypt(kp.privateKeyHex, eciesEncrypt(kp.publicKeyHex, probe));
        if (back != probe) throw std::runtime_error("mismatch");
    } catch (...) {
        err = "the private and public halves do not belong together"; return false;
    }
    QJsonObject st = loadState(statePath());
    st["owner_pub"] = qs(kp.publicKeyHex);
    st["owner_priv"] = qs(kp.privateKeyHex);
    if (!st.contains("last_nonce")) st["last_nonce"] = 0.0;
    if (!saveState(dir_, statePath(), st)) { err = "cannot write " + statePath(); return false; }
    ownerPubOut = kp.publicKeyHex;
    return true;
}

bool OwnerClient::pair(const std::string& cardJsonOrPath, const std::string& accountId,
                       const std::string& relayUrl, std::string& err) {
    std::string text = trimmed(cardJsonOrPath);
    QByteArray raw;
    if (!text.empty() && text[0] == '{') {
        raw = QByteArray::fromStdString(text);
    } else {
        QFile f(qs(text));
        if (text.empty() || !f.open(QIODevice::ReadOnly)) { err = "cannot read card " + text; return false; }
        raw = f.readAll();
    }
    QJsonDocument card = QJsonDocument::fromJson(raw);
    if (!card.isObject()) { err = "card is not a JSON object"; return false; }
    QJsonObject logos = card.object()["_logos"].toObject();
    QString agentKey = logos["signing_key"].toString();
    if (agentKey.isEmpty()) { err = "card has no _logos.signing_key"; return false; }
    std::string account = trimmed(accountId);
    if (account.empty()) { err = "an agent account id is required (metaStatus.account)"; return false; }
    QJsonObject st = loadState(statePath());
    st["agent_key"] = agentKey;
    st["agent_enc_key"] = logos["enc_key"].toString();
    st["agent_name"] = card.object()["name"].toString();
    st["account"] = qs(account);
    st["topic"] = qs(topicFor(account));
    std::string relay = trimmed(relayUrl);
    if (relay.empty()) relay = st.contains("relay") ? st["relay"].toString().toStdString() : "http://127.0.0.1:8645";
    while (!relay.empty() && relay.back() == '/') relay.pop_back();
    st["relay"] = qs(relay);
    if (!saveState(dir_, statePath(), st)) { err = "cannot write " + statePath(); return false; }
    return true;
}

bool OwnerClient::ready(std::string& why) const { return readyFrom(loadState(statePath()), why); }
bool OwnerClient::hasKey() const { return !loadState(statePath())["owner_priv"].toString().isEmpty(); }
bool OwnerClient::isPaired() const {
    QJsonObject st = loadState(statePath());
    return !st["agent_key"].toString().isEmpty() && !st["topic"].toString().isEmpty();
}

bool OwnerClient::send(const std::string& text, int64_t& nonceOut, size_t& sealedBytesOut, std::string& err) {
    QJsonObject st = loadState(statePath());
    if (!readyFrom(st, err)) return false;
    if (trimmed(text).empty()) { err = "nothing to send"; return false; }
    ECIESKeypair owner = ownerKeyFrom(st);
    int64_t nonce = nextNonce(static_cast<int64_t>(st["last_nonce"].toDouble()), nowMs());
    std::string envelope = buildEnvelope(text, owner, nonce);
    std::string sealed;
    try {
        sealed = sealForAgent(envelope, st["agent_key"].toString().toStdString());
    } catch (const std::exception& e) {
        err = std::string("encryption to the agent key failed: ") + e.what(); return false;
    }
    QJsonObject msg;
    msg["payload"] = QString::fromLatin1(QByteArray::fromStdString(sealed).toBase64());
    msg["contentTopic"] = st["topic"].toString();
    msg["timestamp"] = static_cast<double>(nowNs());
    int status = 0;
    QByteArray body = http("POST", st["relay"].toString() + "/relay/v1/auto/messages",
                           QJsonDocument(msg).toJson(QJsonDocument::Compact), 15000, &status);
    if (status < 200 || status >= 300) {
        err = "relay refused the publish (HTTP " + std::to_string(status) + "): " +
              QString::fromUtf8(body.left(200)).toStdString();
        return false;
    }
    st["last_nonce"] = static_cast<double>(nonce);
    saveState(dir_, statePath(), st);
    nonceOut = nonce;
    sealedBytesOut = sealed.size();
    return true;
}

bool OwnerClient::poll(int64_t sinceNs, std::vector<OwnerReply>& out, std::string& err) {
    QJsonObject st = loadState(statePath());
    if (!readyFrom(st, err)) return false;
    int status = 0;
    QByteArray body = http("GET", qs(storeUrl(st["relay"].toString().toStdString(),
                                             st["topic"].toString().toStdString(), sinceNs)),
                           {}, 10000, &status);
    if (status != 200) { err = "relay store answered HTTP " + std::to_string(status); return false; }
    std::vector<std::string> seen;
    for (const QJsonValue& v : st["seen"].toArray()) seen.push_back(v.toString().toStdString());
    out = repliesFromStoreBody(body.toStdString(), st["owner_priv"].toString().toStdString(), seen);
    while (seen.size() > 2000) seen.erase(seen.begin());
    QJsonArray arr;
    for (const std::string& s : seen) arr.append(qs(s));
    st["seen"] = arr;
    saveState(dir_, statePath(), st);
    return true;
}

std::string OwnerClient::statusJson() const {
    QJsonObject st = loadState(statePath());
    QJsonObject o;
    o["initialised"] = !st["owner_priv"].toString().isEmpty();
    o["paired"] = !st["agent_key"].toString().isEmpty() && !st["topic"].toString().isEmpty();
    o["owner_pub"] = st["owner_pub"].toString();
    o["agent_name"] = st["agent_name"].toString();
    o["agent_key"] = st["agent_key"].toString();
    o["account"] = st["account"].toString();
    o["topic"] = st["topic"].toString();
    o["relay"] = st["relay"].toString();
    o["last_nonce"] = st["last_nonce"].toDouble();
    o["state"] = qs(statePath());
    return compact(o);
}

std::string OwnerClient::field(const char* key) const {
    return loadState(statePath())[QString::fromLatin1(key)].toString().toStdString();
}

int64_t OwnerClient::lastNonce() const {
    return static_cast<int64_t>(loadState(statePath())["last_nonce"].toDouble());
}
