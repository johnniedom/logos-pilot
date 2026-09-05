// pilot-owner — the owner's side of a Pilot agent's owner channel, as a separate program.
//
// A thin console front-end over OwnerClient (owner_client.h), the same library the Basecamp
// plugin's module (pilot_owner, pilot-owner/module) compiles in. The agent's own crypto
// (pilot_crypto.cpp) is compiled in too, so what this tool seals and signs is exactly what the
// agent opens and checks.
//
//   pilot-owner init [--import <priv hex>:<pub hex>] [--force]   make (or import) the owner keypair
//   pilot-owner pair <card.json or JSON text> <agent account id>  learn the agent's key + topic
//               [--relay http://127.0.0.1:8645]
//   pilot-owner send "<text>"                                      sign, seal, publish
//   pilot-owner listen [--since <secs>] [--follow]                 poll the store, print what is ours
//   pilot-owner status                                             what this client knows (no secrets)
//   pilot-owner selftest                                           the library's pure pieces, no network
//
// State: $PILOT_OWNER_HOME (default ~/.pilot-owner)/state.json, mode 0600.

#include "owner_client.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTextStream>

#include <sys/stat.h>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

QTextStream& out() { static QTextStream s(stdout); return s; }
QTextStream& err() { static QTextStream s(stderr); return s; }
QString qs(const std::string& s) { return QString::fromStdString(s); }

int usage() {
    err() << "usage:\n"
             "  pilot-owner init [--import <priv hex>:<pub hex>] [--force]\n"
             "  pilot-owner pair <card.json> <agent account id> [--relay http://127.0.0.1:8645]\n"
             "  pilot-owner send \"<text>\"\n"
             "  pilot-owner listen [--since <secs>] [--follow]\n"
             "  pilot-owner status\n"
             "  pilot-owner selftest\n";
    err().flush();
    return 2;
}

int cmdInit(const QStringList& args) {
    OwnerClient c;
    std::string pub, e;
    int imp = args.indexOf("--import");
    if (imp >= 0) {
        if (imp + 1 >= args.size()) { err() << "--import expects '<priv hex>:<pub hex>'\n"; return 2; }
        if (!c.importKey(args[imp + 1].toStdString(), pub, e)) { err() << qs(e) << "\n"; return 1; }
    } else {
        bool created = false;
        if (!c.init(args.contains("--force"), pub, created, e)) { err() << qs(e) << "\n"; return 1; }
        if (!created) {
            out() << "owner key already exists: " << qs(pub) << "\n"
                  << "(use --force to replace it — the agent bound to the old key will stop listening to you)\n";
            out().flush();
            return 0;
        }
    }
    out() << "owner public key: " << qs(pub) << "\n"
          << "bind it to the agent (one of):\n"
          << "  logoscore call pilot metaConfigure owner.npk " << qs(pub) << "\n"
          << "  PILOT_OWNER_NPK=" << qs(pub) << " pilot deploy\n"
          << "state: " << qs(c.statePath()) << " (mode 0600; holds the private key)\n";
    out().flush();
    return 0;
}

int cmdPair(const QStringList& args) {
    if (args.size() < 2) return usage();
    OwnerClient c;
    int r = args.indexOf("--relay");
    std::string relay = (r >= 0 && r + 1 < args.size()) ? args[r + 1].toStdString() : std::string();
    std::string e;
    if (!c.pair(args[0].toStdString(), args[1].toStdString(), relay, e)) { err() << qs(e) << "\n"; return 1; }
    out() << "paired with agent " << qs(c.field("agent_name")) << "\n"
          << "  agent signing key: " << qs(c.field("agent_key")).left(24) << "…\n"
          << "  owner topic:       " << qs(c.field("topic")) << "\n"
          << "  relay:             " << qs(c.field("relay")) << "\n";
    out().flush();
    return 0;
}

int cmdSend(const QStringList& args) {
    if (args.isEmpty()) return usage();
    OwnerClient c;
    int64_t nonce = 0; size_t bytes = 0; std::string e;
    if (!c.send(args.join(' ').toStdString(), nonce, bytes, e)) { err() << qs(e) << "\n"; return 1; }
    out() << "sent (nonce " << static_cast<qlonglong>(nonce) << ", " << static_cast<qulonglong>(bytes)
          << " bytes sealed) on " << qs(c.field("topic")) << "\n";
    out().flush();
    return 0;
}

// One store read, printed. Returns how many replies were new, -1 when the relay did not answer.
int printReplies(OwnerClient& c, int64_t sinceNs) {
    std::vector<OwnerReply> replies; std::string e;
    if (!c.poll(sinceNs, replies, e)) { err() << qs(e) << "\n"; err().flush(); return -1; }
    for (const OwnerReply& r : replies) {
        QString when = r.timestampNs > 0
            ? QDateTime::fromMSecsSinceEpoch(r.timestampNs / 1000000, Qt::UTC).toString("HH:mm:ss")
            : QString("--:--:--");
        out() << "[" << when << "] agent: " << qs(r.text) << "\n";
    }
    out().flush();
    return static_cast<int>(replies.size());
}

int cmdListen(const QStringList& args) {
    OwnerClient c;
    std::string why;
    if (!c.ready(why)) { err() << "listen: " << qs(why) << "\n"; return 1; }
    int sinceSecs = 900;
    int s = args.indexOf("--since");
    if (s >= 0 && s + 1 < args.size()) sinceSecs = args[s + 1].toInt();
    bool follow = args.contains("--follow");
    int64_t sinceNs = OwnerClient::nowNs() - static_cast<int64_t>(sinceSecs) * 1000000000LL;
    int total = 0;
    do {
        int n = printReplies(c, sinceNs);
        if (n > 0) total += n;
        if (!follow) break;
        // Overlap the next window by a minute so a late-stored message is not skipped; seen
        // hashes keep the overlap idempotent.
        sinceNs = OwnerClient::nowNs() - 60LL * 1000000000LL;
        std::this_thread::sleep_for(std::chrono::seconds(5));
    } while (true);
    if (!follow && total == 0) out() << "(no reply from the agent in the last " << sinceSecs << " s)\n";
    out().flush();
    return 0;
}

int cmdStatus() {
    OwnerClient c;
    std::string pub = c.field("owner_pub"), agent = c.field("agent_key");
    out() << "state:  " << qs(c.statePath()) << "\n"
          << "owner:  " << (pub.empty() ? QString("(not initialised)") : qs(pub)) << "\n"
          << "agent:  " << (agent.empty() ? QString("(not paired)") : qs(agent).left(24) + "…") << "\n"
          << "topic:  " << qs(c.field("topic")) << "\n"
          << "relay:  " << qs(c.field("relay")) << "\n"
          << "nonce:  " << static_cast<qlonglong>(c.lastNonce()) << "\n";
    out().flush();
    return 0;
}

// No network. Proves the library's pure pieces: what `send` builds is what the agent accepts
// (signature over the canonical bytes; a tampered message fails), ECIES both ways, the wrong
// key is rejected, nonces only go up, a store body yields exactly the replies sealed to the
// owner (own sends and duplicates skipped), and the state file round-trips at mode 0600.
int selftest() {
    auto fail = [](const std::string& what) {
        err() << "selftest FAIL: " << qs(what) << "\n"; err().flush(); return 1;
    };

    ECIESKeypair owner = generateECIESKeypair();
    ECIESKeypair agent = generateECIESKeypair();

    // 1. envelope: verifies, tamper detected
    std::string env = OwnerClient::buildEnvelope("/balance", owner, 1725500000000LL);
    std::string msg, key;
    if (!OwnerClient::verifyEnvelope(env, msg, key) || msg != "/balance" || key != owner.publicKeyHex)
        return fail("envelope does not verify");
    std::string tampered = env;
    tampered.replace(tampered.find("/balance"), 8, "/approve");
    if (OwnerClient::verifyEnvelope(tampered, msg, key)) return fail("tampered envelope verified");

    // 2. ECIES to the agent and back; a reply to the owner and back; wrong key rejected
    std::string sealed = OwnerClient::sealForAgent(env, agent.publicKeyHex);
    std::vector<uint8_t> opened = eciesDecrypt(agent.privateKeyHex, eciesDeserialize(sealed));
    if (std::string(opened.begin(), opened.end()) != env) return fail("ECIES round trip to the agent");
    std::string reply = "Balance: 150 LEZ";
    std::vector<uint8_t> rb(reply.begin(), reply.end());
    std::string rsealed = eciesSerialize(eciesEncrypt(owner.publicKeyHex, rb));
    std::vector<uint8_t> ropened = eciesDecrypt(owner.privateKeyHex, eciesDeserialize(rsealed));
    if (std::string(ropened.begin(), ropened.end()) != reply) return fail("reply round trip");
    bool wrongKey = true;
    try { eciesDecrypt(agent.privateKeyHex, eciesDeserialize(rsealed)); } catch (...) { wrongKey = false; }
    if (wrongKey) return fail("a reply sealed to the owner opened with the agent key");

    // 3. nonces: strictly increasing, never below the clock; the topic shape
    if (OwnerClient::nextNonce(5, 3) != 6) return fail("nextNonce did not step past the last nonce");
    if (OwnerClient::nextNonce(5, 100) != 100) return fail("nextNonce did not follow the clock");
    if (OwnerClient::topicFor("abc") != "/pilot/1/owner-abc/proto") return fail("topicFor");

    // 4. a store body: one reply to the owner, one of our own sends (sealed to the agent), and
    //    the owner's reply repeated under the same hash -> exactly one reply, both hashes seen,
    //    nothing new on a second pass
    QJsonObject own; own["payload"] = QString::fromLatin1(QByteArray::fromStdString(sealed).toBase64()); own["timestamp"] = 1.0e18;
    QJsonObject rep; rep["payload"] = QString::fromLatin1(QByteArray::fromStdString(rsealed).toBase64()); rep["timestamp"] = 2.0e18;
    QJsonObject m1; m1["messageHash"] = "h-reply"; m1["message"] = rep;
    QJsonObject m2; m2["messageHash"] = "h-own";   m2["message"] = own;
    QJsonObject m3; m3["messageHash"] = "h-reply"; m3["message"] = rep;
    QJsonObject body; body["messages"] = QJsonArray{m1, m2, m3};
    std::string bodyText = QJsonDocument(body).toJson(QJsonDocument::Compact).toStdString();
    std::vector<std::string> seen;
    std::vector<OwnerReply> got = OwnerClient::repliesFromStoreBody(bodyText, owner.privateKeyHex, seen);
    if (got.size() != 1 || got[0].text != reply || got[0].hash != "h-reply")
        return fail("store body did not yield exactly the owner's reply");
    if (seen.size() != 2) return fail("seen hashes should hold both messages once");
    got = OwnerClient::repliesFromStoreBody(bodyText, owner.privateKeyHex, seen);
    if (!got.empty()) return fail("a second pass over the same body must yield nothing new");
    std::string url = OwnerClient::storeUrl("http://127.0.0.1:8645", "/pilot/1/owner-abc/proto", 42);
    if (url.find("/store/v3/messages?") == std::string::npos || url.find("startTime=42") == std::string::npos ||
        url.find("owner-abc") == std::string::npos) return fail("storeUrl");

    // 5. state file: key, import validation, pairing from JSON text, status without secrets, mode 0600
    std::string dir = QDir::tempPath().toStdString() + "/pilot-owner-selftest-" + std::to_string(OwnerClient::nowMs());
    OwnerClient c(dir);
    std::string pub, e; bool created = false;
    if (!c.init(false, pub, created, e) || !created || pub.size() < 66) return fail("init did not make a key: " + e);
    std::string pub2;
    if (!c.init(false, pub2, created, e) || created || pub2 != pub) return fail("a second init must keep the key");
    if (c.importKey(agent.privateKeyHex + ":" + owner.publicKeyHex, pub2, e)) return fail("import accepted a mismatched pair");
    if (!c.importKey(owner.privateKeyHex + ":" + owner.publicKeyHex, pub2, e) || pub2 != owner.publicKeyHex)
        return fail("import rejected a matching pair: " + e);
    std::string why;
    if (c.ready(why)) return fail("ready before pairing");
    QJsonObject logos; logos["signing_key"] = qs(agent.publicKeyHex); logos["enc_key"] = "enc";
    QJsonObject card; card["name"] = "Pilot-Test"; card["_logos"] = logos;
    if (!c.pair(QJsonDocument(card).toJson(QJsonDocument::Compact).toStdString(), " acct1 ", "http://relay:8645/", e))
        return fail("pair from JSON text: " + e);
    if (!c.ready(why) || c.field("topic") != "/pilot/1/owner-acct1/proto" || c.field("relay") != "http://relay:8645" ||
        c.field("agent_name") != "Pilot-Test") return fail("pairing state");
    struct stat sb {};
    if (stat(c.statePath().c_str(), &sb) != 0 || (sb.st_mode & 0777) != 0600) return fail("state file is not mode 0600");
    QJsonObject st = QJsonDocument::fromJson(QByteArray::fromStdString(c.statusJson())).object();
    if (!st["paired"].toBool() || !st["initialised"].toBool() || st.contains("owner_priv")) return fail("statusJson");
    QDir(qs(dir)).removeRecursively();

    out() << "selftest OK: signed envelope verifies, tamper detected, ECIES both ways, wrong key rejected, "
             "nonces monotonic, store body parsed and de-duplicated, state file 0600 with pairing\n";
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
    if (cmd == "selftest") return selftest();
    return usage();
}
