// pilot-owner — the owner's side of a Pilot agent's owner channel, as a separate program.
//
// The agent (pilot module inside logoscore) listens on one Logos Messaging content topic,
// "/pilot/1/owner-<agent account id>/proto". Anything it reads there must be ECIES-sealed to the
// agent's signing key (the card's _logos.signing_key) and, once an owner is bound, be a SIGNED
// envelope: {"message": <text>, "_logos": {"signing_key": <owner pub>, "nonce": <n>, "signature":
// <ECDSA hex over the compact JSON of the envelope without the signature>}}. The agent answers on
// the same topic, ECIES-sealed to the owner's key (config owner.npk). This tool does exactly that,
// with the agent's own crypto compiled in (pilot_crypto.cpp) and a Waku relay's REST API as the
// only transport: publish = POST /relay/v1/auto/messages, read = GET /store/v3/messages. No
// daemon socket, no local RPC, no server of ours anywhere between the two.
//
//   pilot-owner init [--import <priv hex>]              make (or import) the owner keypair
//   pilot-owner pair <card.json> <agent account id>     learn the agent's key + topic
//               [--relay http://127.0.0.1:8645]
//   pilot-owner send "<text>"                            sign, seal, publish
//   pilot-owner listen [--since <secs>] [--follow]       poll the store, decrypt what is ours
//   pilot-owner status                                   what this client knows (no secrets)
//   pilot-owner selftest                                 envelope + ECIES round trip (no network)
//
// State: $PILOT_OWNER_HOME (default ~/.pilot-owner)/state.json, mode 0600. Nonces are the wall
// clock in milliseconds, strictly increasing, so a lost state file never replays an old value.

#include "pilot_crypto.h"

#include <QCoreApplication>
#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <sys/stat.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

QTextStream& out() { static QTextStream s(stdout); return s; }
QTextStream& err() { static QTextStream s(stderr); return s; }

std::string stateDir() {
    if (const char* h = std::getenv("PILOT_OWNER_HOME"); h && *h) return h;
    return QDir::homePath().toStdString() + "/.pilot-owner";
}
std::string statePath() { return stateDir() + "/state.json"; }

QJsonObject loadState() {
    QFile f(QString::fromStdString(statePath()));
    if (!f.open(QIODevice::ReadOnly)) return {};
    QJsonDocument d = QJsonDocument::fromJson(f.readAll());
    return d.isObject() ? d.object() : QJsonObject();
}

bool saveState(const QJsonObject& st) {
    QDir().mkpath(QString::fromStdString(stateDir()));
    QFile f(QString::fromStdString(statePath()));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(QJsonDocument(st).toJson(QJsonDocument::Indented));
    f.close();
    chmod(statePath().c_str(), 0600);   // the owner's private key lives here
    return true;
}

std::string compact(const QJsonObject& o) {
    return QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString();
}

// Blocking HTTP with a deadline. Returns the body; *status = HTTP code, 0 when nothing came back.
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

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// The signed owner envelope, built EXACTLY the way the agent verifies it (verifyOwnerMessage):
// canonical bytes = compact JSON of the whole envelope with _logos.signature absent and
// _logos.signing_key present; signature = ECDSA-secp256k1 over SHA-256 of those bytes.
std::string buildEnvelope(const std::string& text, const ECIESKeypair& owner, int64_t nonce) {
    QJsonObject env;
    env["message"] = QString::fromStdString(text);
    QJsonObject logos;
    logos["signing_key"] = QString::fromStdString(owner.publicKeyHex);
    logos["nonce"] = static_cast<double>(nonce);
    env["_logos"] = logos;
    std::string canonical = compact(env);
    std::vector<uint8_t> bytes(canonical.begin(), canonical.end());
    logos["signature"] = QString::fromStdString(signMessage(bytes, owner.privateKeyHex));
    env["_logos"] = logos;
    return compact(env);
}

// The agent's check, reproduced here for selftest: verify the envelope's signature the way
// verifyOwnerMessage does and hand back the message.
bool verifyEnvelope(const std::string& raw, std::string& messageOut, std::string& keyOut) {
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

ECIESKeypair ownerKeyFrom(const QJsonObject& st) {
    ECIESKeypair k;
    k.publicKeyHex = st["owner_pub"].toString().toStdString();
    k.privateKeyHex = st["owner_priv"].toString().toStdString();
    return k;
}

int usage() {
    err() << "usage:\n"
             "  pilot-owner init [--import <priv hex>]\n"
             "  pilot-owner pair <card.json> <agent account id> [--relay http://127.0.0.1:8645]\n"
             "  pilot-owner send \"<text>\"\n"
             "  pilot-owner listen [--since <secs>] [--follow]\n"
             "  pilot-owner status\n"
             "  pilot-owner selftest\n";
    err().flush();
    return 2;
}

int cmdInit(const QStringList& args) {
    QJsonObject st = loadState();
    ECIESKeypair kp;
    int imp = args.indexOf("--import");
    if (imp >= 0 && imp + 1 < args.size()) {
        // Import: derive the public key by encrypting-decrypting is not needed; the agent side
        // only ever sees the public key, so ask the caller for both halves.
        err() << "--import expects '<priv hex>:<pub hex>' (the pair the agent was bound to)\n";
        QStringList parts = args[imp + 1].split(':');
        if (parts.size() != 2) return 2;
        kp.privateKeyHex = parts[0].toStdString();
        kp.publicKeyHex = parts[1].toStdString();
    } else if (st.contains("owner_pub") && !args.contains("--force")) {
        out() << "owner key already exists: " << st["owner_pub"].toString() << "\n"
              << "(use --force to replace it — the agent bound to the old key will stop listening to you)\n";
        out().flush();
        return 0;
    } else {
        kp = generateECIESKeypair();
    }
    st["owner_pub"] = QString::fromStdString(kp.publicKeyHex);
    st["owner_priv"] = QString::fromStdString(kp.privateKeyHex);
    if (!st.contains("last_nonce")) st["last_nonce"] = 0.0;
    if (!saveState(st)) { err() << "cannot write " << QString::fromStdString(statePath()) << "\n"; return 1; }
    out() << "owner public key: " << QString::fromStdString(kp.publicKeyHex) << "\n"
          << "bind it to the agent (one of):\n"
          << "  logoscore call pilot metaConfigure owner.npk " << QString::fromStdString(kp.publicKeyHex) << "\n"
          << "  PILOT_OWNER_NPK=" << QString::fromStdString(kp.publicKeyHex) << " pilot deploy\n"
          << "state: " << QString::fromStdString(statePath()) << " (mode 0600; holds the private key)\n";
    out().flush();
    return 0;
}

int cmdPair(const QStringList& args) {
    if (args.size() < 2) return usage();
    QFile f(args[0]);
    if (!f.open(QIODevice::ReadOnly)) { err() << "cannot read card " << args[0] << "\n"; return 1; }
    QJsonDocument card = QJsonDocument::fromJson(f.readAll());
    if (!card.isObject()) { err() << "card is not a JSON object\n"; return 1; }
    QJsonObject logos = card.object()["_logos"].toObject();
    QString agentKey = logos["signing_key"].toString();
    if (agentKey.isEmpty()) { err() << "card has no _logos.signing_key\n"; return 1; }
    QJsonObject st = loadState();
    st["agent_key"] = agentKey;
    st["agent_enc_key"] = logos["enc_key"].toString();
    st["agent_name"] = card.object()["name"].toString();
    st["account"] = args[1];
    st["topic"] = "/pilot/1/owner-" + args[1] + "/proto";
    int r = args.indexOf("--relay");
    st["relay"] = (r >= 0 && r + 1 < args.size()) ? args[r + 1]
                : (st.contains("relay") ? st["relay"].toString() : QString("http://127.0.0.1:8645"));
    if (!saveState(st)) { err() << "cannot write state\n"; return 1; }
    out() << "paired with agent " << st["agent_name"].toString() << "\n"
          << "  agent signing key: " << agentKey.left(24) << "…\n"
          << "  owner topic:       " << st["topic"].toString() << "\n"
          << "  relay:             " << st["relay"].toString() << "\n";
    out().flush();
    return 0;
}

bool ready(const QJsonObject& st, const char* what) {
    if (st["owner_priv"].toString().isEmpty()) { err() << what << ": run `pilot-owner init` first\n"; return false; }
    if (st["agent_key"].toString().isEmpty() || st["topic"].toString().isEmpty()) {
        err() << what << ": run `pilot-owner pair <card.json> <account id>` first\n"; return false;
    }
    return true;
}

int cmdSend(const QStringList& args) {
    if (args.isEmpty()) return usage();
    QJsonObject st = loadState();
    if (!ready(st, "send")) return 1;
    ECIESKeypair owner = ownerKeyFrom(st);
    int64_t last = static_cast<int64_t>(st["last_nonce"].toDouble());
    int64_t nonce = std::max<int64_t>(last + 1, nowMs());
    std::string envelope = buildEnvelope(args.join(' ').toStdString(), owner, nonce);
    std::vector<uint8_t> plain(envelope.begin(), envelope.end());
    std::string sealed;
    try {
        sealed = eciesSerialize(eciesEncrypt(st["agent_key"].toString().toStdString(), plain));
    } catch (const std::exception& e) {
        err() << "encryption to the agent key failed: " << e.what() << "\n"; return 1;
    }
    QJsonObject msg;
    msg["payload"] = QString::fromLatin1(QByteArray::fromStdString(sealed).toBase64());
    msg["contentTopic"] = st["topic"].toString();
    msg["timestamp"] = static_cast<double>(nowNs());
    int status = 0;
    QByteArray body = http("POST", st["relay"].toString() + "/relay/v1/auto/messages",
                           QJsonDocument(msg).toJson(QJsonDocument::Compact), 15000, &status);
    if (status < 200 || status >= 300) {
        err() << "relay refused the publish (HTTP " << status << "): " << QString::fromUtf8(body.left(200)) << "\n";
        return 1;
    }
    st["last_nonce"] = static_cast<double>(nonce);
    saveState(st);
    out() << "sent (nonce " << nonce << ", " << sealed.size() << " bytes sealed) on " << st["topic"].toString() << "\n";
    out().flush();
    return 0;
}

// One store read: every message on the owner topic since `sinceNs`, oldest first. Payloads the
// owner key can open are the agent's replies; the rest (our own sends, sealed to the agent) skip.
int pollOnce(QJsonObject& st, int64_t sinceNs, bool& sawAny) {
    QUrl url(st["relay"].toString() + "/store/v3/messages");
    QUrlQuery q;
    q.addQueryItem("includeData", "true");
    q.addQueryItem("contentTopics", st["topic"].toString());
    q.addQueryItem("startTime", QString::number(static_cast<qlonglong>(sinceNs)));
    q.addQueryItem("pageSize", "100");
    q.addQueryItem("ascending", "true");
    url.setQuery(q);
    int status = 0;
    QByteArray body = http("GET", url.toString(QUrl::FullyEncoded), {}, 10000, &status);
    if (status != 200) { err() << "relay store answered HTTP " << status << "\n"; return -1; }
    QJsonDocument doc = QJsonDocument::fromJson(body);
    QJsonArray seen = st["seen"].toArray();
    ECIESKeypair owner = ownerKeyFrom(st);
    int printed = 0;
    for (const QJsonValue& v : doc.object().value("messages").toArray()) {
        QJsonObject m = v.toObject();
        QString hash = m.value("messageHash").toString();
        if (hash.isEmpty() || seen.contains(hash)) continue;
        seen.append(hash);
        sawAny = true;
        QJsonObject msg = m.value("message").toObject();
        std::string payload = QByteArray::fromBase64(msg.value("payload").toString().toLatin1()).toStdString();
        try {
            std::vector<uint8_t> plain = eciesDecrypt(owner.privateKeyHex, eciesDeserialize(payload));
            std::string text(plain.begin(), plain.end());
            qint64 tsNs = static_cast<qint64>(msg.value("timestamp").toDouble());
            QString when = tsNs > 0 ? QDateTime::fromMSecsSinceEpoch(tsNs / 1000000, Qt::UTC).toString("HH:mm:ss")
                                    : QString("--:--:--");
            out() << "[" << when << "] agent: " << QString::fromStdString(text) << "\n";
            ++printed;
        } catch (...) {
            // Not for us: our own sealed send, or a payload sealed to someone else.
        }
    }
    while (seen.size() > 2000) seen.removeFirst();
    st["seen"] = seen;
    saveState(st);
    out().flush();
    return printed;
}

int cmdListen(const QStringList& args) {
    QJsonObject st = loadState();
    if (!ready(st, "listen")) return 1;
    int sinceSecs = 900;
    int s = args.indexOf("--since");
    if (s >= 0 && s + 1 < args.size()) sinceSecs = args[s + 1].toInt();
    bool follow = args.contains("--follow");
    int64_t sinceNs = nowNs() - static_cast<int64_t>(sinceSecs) * 1000000000LL;
    int total = 0;
    do {
        bool sawAny = false;
        int n = pollOnce(st, sinceNs, sawAny);
        if (n > 0) total += n;
        if (!follow) break;
        // Overlap the next window by a minute so a late-stored message is not skipped; seen
        // hashes keep the overlap idempotent.
        sinceNs = nowNs() - 60LL * 1000000000LL;
        std::this_thread::sleep_for(std::chrono::seconds(5));
    } while (true);
    if (!follow && total == 0) out() << "(no reply from the agent in the last " << sinceSecs << " s)\n";
    out().flush();
    return 0;
}

int cmdStatus() {
    QJsonObject st = loadState();
    out() << "state:  " << QString::fromStdString(statePath()) << "\n"
          << "owner:  " << (st.contains("owner_pub") ? st["owner_pub"].toString() : QString("(not initialised)")) << "\n"
          << "agent:  " << (st.contains("agent_key") ? st["agent_key"].toString().left(24) + "…" : QString("(not paired)")) << "\n"
          << "topic:  " << st["topic"].toString() << "\n"
          << "relay:  " << st["relay"].toString() << "\n"
          << "nonce:  " << static_cast<qint64>(st["last_nonce"].toDouble()) << "\n";
    out().flush();
    return 0;
}

// No network: prove that what `send` builds is what the agent accepts (signature verifies over
// the canonical bytes, a tampered message does not, ECIES to the agent key round-trips, the
// agent's reply sealed to the owner key round-trips).
int cmdSelftest() {
    ECIESKeypair owner = generateECIESKeypair();
    ECIESKeypair agent = generateECIESKeypair();
    std::string env = buildEnvelope("/balance", owner, 1725500000000LL);
    std::string msg, key;
    if (!verifyEnvelope(env, msg, key) || msg != "/balance" || key != owner.publicKeyHex) {
        err() << "selftest FAIL: envelope does not verify\n"; return 1;
    }
    std::string tampered = env;
    size_t p = tampered.find("/balance");
    tampered.replace(p, 8, "/approve");
    if (verifyEnvelope(tampered, msg, key)) { err() << "selftest FAIL: tampered envelope verified\n"; return 1; }
    std::vector<uint8_t> plain(env.begin(), env.end());
    std::string sealed = eciesSerialize(eciesEncrypt(agent.publicKeyHex, plain));
    std::vector<uint8_t> opened = eciesDecrypt(agent.privateKeyHex, eciesDeserialize(sealed));
    if (std::string(opened.begin(), opened.end()) != env) { err() << "selftest FAIL: ECIES round trip\n"; return 1; }
    std::string reply = "Balance: 150 LEZ";
    std::vector<uint8_t> rb(reply.begin(), reply.end());
    std::string rsealed = eciesSerialize(eciesEncrypt(owner.publicKeyHex, rb));
    std::vector<uint8_t> ropened = eciesDecrypt(owner.privateKeyHex, eciesDeserialize(rsealed));
    if (std::string(ropened.begin(), ropened.end()) != reply) { err() << "selftest FAIL: reply round trip\n"; return 1; }
    bool wrongKey = true;
    try { eciesDecrypt(agent.privateKeyHex, eciesDeserialize(rsealed)); } catch (...) { wrongKey = false; }
    if (wrongKey) { err() << "selftest FAIL: a reply sealed to the owner opened with the agent key\n"; return 1; }
    out() << "selftest OK: signed envelope verifies, tamper detected, ECIES both ways, wrong key rejected\n";
    out().flush();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QStringList args = app.arguments();
    if (args.size() < 2) return usage();
    QString cmd = args[1];
    QStringList rest = args.mid(2);
    if (cmd == "init") return cmdInit(rest);
    if (cmd == "pair") return cmdPair(rest);
    if (cmd == "send") return cmdSend(rest);
    if (cmd == "listen") return cmdListen(rest);
    if (cmd == "status") return cmdStatus();
    if (cmd == "selftest") return cmdSelftest();
    return usage();
}
