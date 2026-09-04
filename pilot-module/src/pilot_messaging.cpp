#include "pilot_impl.h"
#include "pilot_crypto.h"
// Generated per-build; typed client for delivery_module (see pilot_impl.h).
#include "logos_sdk.h"
#include <sqlite3.h>
#include <sstream>
#include <random>
#include <chrono>
#include <cctype>
#include <QString>
#include <QVariant>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QByteArray>

// Messaging over Logos Messaging (delivery_module): direct messages to a peer's inbox, groups
// on their own topic, and — since 2026-09-04 — the RECEIVE side. Before that date a message,
// a group invite or a shared file key reached the peer's inbox, was decrypted, and was thrown
// away for not being a signed A2A request (handleInboundA2A). The sender saw {"sent":true};
// nothing arrived anywhere, and the two-agent test never asked (it asserted the send only).
// Received items are recorded in received_messages and read back by messagingInbox().
//
// Wire shapes (all ECIES-sealed to the recipient's ENCRYPTION key, the one the card advertises
// as _logos.enc_key — the inbox topic is named after that key, so a message sealed to any
// other key, e.g. the wallet viewing key, reaches nobody):
//   direct        {"type":"direct","from":<sender key>,"message":<text>}
//   group_invite  {"type":"group_invite","group_id":..,"topic":..,"key":<64 hex>,"from":..}
//   file_share    {"type":"file_share","cid":..,"key":<file key>,"from":..,"label":..}
// Group messages ride the group topic sealed with AES-256-GCM under the invite's key:
//   {"v":1,"iv":<24 hex>,"tag":<32 hex>,"ct":<base64 of {"from":..,"message":..}>}
// "from" is the sender's CLAIM: none of these carry a signature (A2A task requests do). The
// inbox shows who a message says it is from; it does not prove it.

namespace {

std::string genGroupId() {
    std::random_device rd;
    std::mt19937_64 rng(rd());
    std::ostringstream ss;
    ss << std::hex << rng();
    return ss.str();
}

std::string nowSecs() {
    return std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

// 32 random bytes as 64 hex chars: the key half of aesKeyToHex's key:iv:tag format.
std::string genGroupKeyHex() { return aesKeyToHex(generateFileKey()).substr(0, 64); }

bool isHex(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

std::string trimQuotes(const std::string& m) {
    size_t start = m.find_first_not_of(" \t\r\n\"'");
    size_t end = m.find_last_not_of(" \t\r\n\"'");
    if (start == std::string::npos) return "";
    return m.substr(start, end - start + 1);
}

std::string compact(const QJsonObject& o) {
    return QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString();
}

QString hexOf(const std::vector<uint8_t>& v) {
    return QString::fromLatin1(
        QByteArray(reinterpret_cast<const char*>(v.data()), static_cast<int>(v.size())).toHex());
}

}  // namespace

// ---- pure helpers (declared in pilot_impl.h; unit-tested without a delivery module) ----------

std::string pilotRecipientKey(const std::string& recipient) {
    QJsonDocument d = QJsonDocument::fromJson(QByteArray::fromStdString(recipient));
    if (d.isObject()) {
        QJsonObject o = d.object();
        QJsonObject logos = o.value("_logos").toObject();
        if (logos.contains("enc_key")) return logos["enc_key"].toString().toStdString();
        if (o.contains("enc_key")) return o["enc_key"].toString().toStdString();
        if (o.contains("viewing_public_key")) return o["viewing_public_key"].toString().toStdString();
    }
    return recipient;
}

std::vector<std::string> pilotParseMembers(const std::string& membersJson) {
    std::vector<std::string> out;
    QJsonDocument d = QJsonDocument::fromJson(QByteArray::fromStdString(membersJson));
    if (d.isArray()) {
        for (const QJsonValue& v : d.array()) {
            std::string m;
            if (v.isString()) m = trimQuotes(v.toString().toStdString());
            else if (v.isObject()) m = pilotRecipientKey(compact(v.toObject()));
            if (!m.empty()) out.push_back(m);
        }
        return out;
    }
    // Legacy: a comma list, with or without brackets and quotes ("a,b" / ["a","b"] unparsed).
    std::string s = membersJson;
    if (!s.empty() && s.front() == '[') s.erase(s.begin());
    if (!s.empty() && s.back() == ']') s.pop_back();
    std::istringstream stream(s);
    std::string member;
    while (std::getline(stream, member, ',')) {
        member = trimQuotes(member);
        if (!member.empty()) out.push_back(member);
    }
    return out;
}

std::string pilotSealGroupMessage(const std::string& groupKeyHex, const std::string& from,
                                  const std::string& message) {
    if (groupKeyHex.size() != 64 || !isHex(groupKeyHex)) return "";
    // A fresh 12-byte IV per message: generateFileKey's random iv, nothing else of it.
    std::string ivHex = aesKeyToHex(generateFileKey()).substr(65, 24);
    AESKey k;
    try { k = aesKeyFromHex(groupKeyHex + ":" + ivHex + ":" + std::string(32, '0')); }
    catch (...) { return ""; }
    QJsonObject inner;
    inner["from"] = QString::fromStdString(from);
    inner["message"] = QString::fromStdString(message);
    std::string plain = compact(inner);
    std::vector<uint8_t> ct;
    try { ct = aesEncrypt(std::vector<uint8_t>(plain.begin(), plain.end()), k); }
    catch (...) { return ""; }
    QJsonObject env;
    env["v"] = 1;
    env["iv"] = hexOf(k.iv);
    env["tag"] = hexOf(k.tag);
    env["ct"] = QString::fromLatin1(
        QByteArray(reinterpret_cast<const char*>(ct.data()), static_cast<int>(ct.size())).toBase64());
    return compact(env);
}

bool pilotOpenGroupMessage(const std::string& groupKeyHex, const std::string& payload,
                           std::string& fromOut, std::string& messageOut) {
    if (groupKeyHex.size() != 64 || !isHex(groupKeyHex)) return false;
    QJsonDocument d = QJsonDocument::fromJson(QByteArray::fromStdString(payload));
    if (!d.isObject()) return false;
    QJsonObject env = d.object();
    std::string iv = env["iv"].toString().toStdString();
    std::string tag = env["tag"].toString().toStdString();
    if (iv.size() != 24 || tag.size() != 32 || !isHex(iv) || !isHex(tag)) return false;
    QByteArray ct = QByteArray::fromBase64(env["ct"].toString().toLatin1());
    if (ct.isEmpty()) return false;
    try {
        AESKey k = aesKeyFromHex(groupKeyHex + ":" + iv + ":" + tag);
        std::vector<uint8_t> plain = aesDecrypt(std::vector<uint8_t>(ct.begin(), ct.end()), k);
        QJsonDocument in = QJsonDocument::fromJson(
            QByteArray(reinterpret_cast<const char*>(plain.data()), static_cast<int>(plain.size())));
        if (!in.isObject()) return false;
        fromOut = in.object()["from"].toString().toStdString();
        messageOut = in.object()["message"].toString().toStdString();
        return true;
    } catch (...) {
        return false;   // wrong key: the GCM tag does not verify
    }
}

// ---- receive side -----------------------------------------------------------------------------

void PilotImpl::recordInbound(const std::string& kind, const std::string& sender,
                              const std::string& groupId, const std::string& body,
                              const std::string& topic) {
    if (!db_) return;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "INSERT INTO received_messages (kind, sender, group_id, body, topic, received_at) "
            "VALUES (?, ?, ?, ?, ?, ?);", -1, &st, nullptr) != SQLITE_OK) return;
    std::string ts = nowSecs();
    sqlite3_bind_text(st, 1, kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, sender.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, groupId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, body.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, topic.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 6, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    // Bounded: anyone who knows our key can write here. Keep the newest 2000.
    sqlite3_exec(db_,
        "DELETE FROM received_messages WHERE id NOT IN "
        "(SELECT id FROM received_messages ORDER BY id DESC LIMIT 2000);",
        nullptr, nullptr, nullptr);
}

std::string PilotImpl::groupKeyFor(const std::string& groupId, std::string* topicOut,
                                   bool requireJoined) {
    if (!db_ || groupId.empty()) return "";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT key_hex, topic, joined FROM messaging_groups WHERE group_id = ?;",
            -1, &st, nullptr) != SQLITE_OK) return "";
    sqlite3_bind_text(st, 1, groupId.c_str(), -1, SQLITE_TRANSIENT);
    std::string key, topic;
    int joined = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        if (sqlite3_column_text(st, 0)) key = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        if (sqlite3_column_text(st, 1)) topic = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        joined = sqlite3_column_int(st, 2);
    }
    sqlite3_finalize(st);
    if (key.empty()) return "";
    if (requireJoined && joined == 0) return "";
    if (topicOut) *topicOut = topic;
    return key;
}

bool PilotImpl::handleInboundApplication(const std::string& plainJson) {
    QJsonDocument d = QJsonDocument::fromJson(QByteArray::fromStdString(plainJson));
    if (!d.isObject()) return false;
    QJsonObject o = d.object();
    if (o.contains("jsonrpc")) return false;     // an A2A request — not ours to handle
    std::string type = o["type"].toString().toStdString();
    std::string from = o["from"].toString().toStdString();
    if (type.empty()) {                          // pre-typed wire shapes from older builds
        if (o.contains("cid") && o.contains("key")) type = "file_share";
        else if (o.contains("message")) type = "direct";
    }

    if (type == "direct") {
        std::string msg = o["message"].toString().toStdString();
        if (!msg.empty()) recordInbound("direct", from, "", msg, "inbox");
        return true;
    }

    if (type == "group_invite") {
        std::string gid = o["group_id"].toString().toStdString();
        std::string key = o["key"].toString().toStdString();
        std::string topic = o["topic"].toString().toStdString();
        // An invite without a usable key would create a group we can never read: drop it.
        if (gid.empty() || key.size() != 64 || !isHex(key)) return true;
        if (topic.empty()) topic = "/pilot/1/group-" + gid + "/proto";
        if (db_) {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db_,
                    "INSERT OR IGNORE INTO messaging_groups "
                    "(group_id, topic, key_hex, created_by, joined, created_at) "
                    "VALUES (?, ?, ?, ?, 0, ?);", -1, &st, nullptr) == SQLITE_OK) {
                std::string ts = nowSecs();
                sqlite3_bind_text(st, 1, gid.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 2, topic.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 3, key.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 4, from.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 5, ts.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(st);
                sqlite3_finalize(st);
            }
        }
        recordInbound("group_invite", from, gid, "invited to group " + gid, topic);
        return true;
    }

    if (type == "file_share") {
        std::string cid = o["cid"].toString().toStdString();
        std::string key = o["key"].toString().toStdString();
        std::string label = o["label"].toString().toStdString();
        if (cid.empty() || key.empty()) return true;
        // INSERT OR IGNORE: a share can add a key for a CID we do not hold; it can never
        // replace the key of a file WE uploaded (a stranger must not be able to brick our
        // own downloads).
        std::string lbl = "shared by " + (from.empty() ? std::string("unknown") : from)
                          + (label.empty() ? std::string() : ": " + label);
        if (db_) {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db_,
                    "INSERT OR IGNORE INTO stored_files (cid, label, file_key_encrypted, timestamp, size_bytes) "
                    "VALUES (?, ?, ?, ?, 0);", -1, &st, nullptr) == SQLITE_OK) {
                std::string ts = nowSecs();
                sqlite3_bind_text(st, 1, cid.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 2, lbl.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 3, key.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 4, ts.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(st);
                sqlite3_finalize(st);
            }
        }
        recordInbound("file_share", from, "",
                      "shared file " + cid + (label.empty() ? std::string() : " (" + label + ")"),
                      "inbox");
        return true;
    }

    return true;   // some other plain object: not an RPC, nothing we know how to keep — dropped
}

void PilotImpl::handleInboundGroupMessage(const std::string& topic, const std::string& payload) {
    const std::string prefix = "/pilot/1/group-";
    const std::string suffix = "/proto";
    if (topic.size() <= prefix.size() + suffix.size()) return;
    std::string gid = topic.substr(prefix.size(), topic.size() - prefix.size() - suffix.size());
    // Only a group the owner JOINED is read (invited-but-not-joined is not polled either).
    std::string key = groupKeyFor(gid, nullptr, true);
    if (key.empty()) return;
    std::string from, msg;
    if (!pilotOpenGroupMessage(key, payload, from, msg)) return;   // wrong key / junk: drop
    if (!from.empty() && from == a2aSelfEncKey()) return;          // our own send, echoed by the store
    recordInbound("group", from, gid, msg, topic);
}

std::string PilotImpl::messagingInbox() {
    if (!db_) return "{\"error\": \"not initialized\"}";
    sqlite3_stmt* st = nullptr;
    QJsonArray arr;
    if (sqlite3_prepare_v2(db_,
            "SELECT id, kind, sender, group_id, body, topic, received_at FROM received_messages "
            "ORDER BY id DESC LIMIT 200;", -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            auto text = [&](int i) {
                const unsigned char* t = sqlite3_column_text(st, i);
                return QString::fromUtf8(t ? reinterpret_cast<const char*>(t) : "");
            };
            QJsonObject m;
            m["id"] = static_cast<qint64>(sqlite3_column_int64(st, 0));
            m["kind"] = text(1);
            m["from"] = text(2);
            m["group_id"] = text(3);
            m["message"] = text(4);
            m["topic"] = text(5);
            m["received_at"] = text(6);
            arr.append(m);
        }
        sqlite3_finalize(st);
    }
    QJsonObject res;
    res["messages"] = arr;
    res["count"] = arr.size();
    return compact(res);
}

// ---- send side --------------------------------------------------------------------------------

std::string PilotImpl::messagingSend(const std::string& recipient, const std::string& message) {
    if (!isContextReady()) return "{\"error\": \"not initialized\"}";
    initDeliveryModule();

    std::string self = a2aSelfEncKey();
    if (self.empty()) self = agentNpk_;

    // "group:<id>" -> the group's topic, sealed under the group key.
    if (recipient.rfind("group:", 0) == 0) {
        std::string gid = recipient.substr(6);
        std::string topic;
        std::string key = groupKeyFor(gid, &topic, true);
        if (key.empty()) return "{\"error\": \"unknown or unjoined group\"}";
        std::string sealed = pilotSealGroupMessage(key, self, message);
        if (sealed.empty()) return "{\"error\": \"group key unusable\"}";
        modules().delivery_module.send(
            topic, std::vector<uint8_t>(sealed.begin(), sealed.end()), nullptr, kDeliveryFireAndForgetMs);
        QJsonObject result;
        result["sent"] = true;
        result["group_id"] = QString::fromStdString(gid);
        result["topic"] = QString::fromStdString(topic);
        result["encrypted"] = true;
        return compact(result);
    }

    // Direct: to the peer's inbox, which is named after the peer's ENCRYPTION key. Accept the
    // bare key, the peer's card (uses _logos.enc_key) or an npk blob (viewing_public_key).
    std::string recipientKey = pilotRecipientKey(recipient);
    QJsonObject payload;
    payload["type"] = QString("direct");
    payload["from"] = QString::fromStdString(self);
    payload["message"] = QString::fromStdString(message);
    std::string payloadStr = compact(payload);
    std::vector<uint8_t> plainBytes(payloadStr.begin(), payloadStr.end());

    std::string encPayload;
    try {
        ECIESCiphertext encrypted = eciesEncrypt(recipientKey, plainBytes);
        encPayload = eciesSerialize(encrypted);
    } catch (const std::exception& e) {
        QJsonObject err;
        err["error"] = QString::fromStdString(std::string("encryption failed: ") + e.what());
        return compact(err);
    }

    std::string topic = "/pilot/1/inbox-" + recipientKey + "/proto";
    modules().delivery_module.send(
        topic, std::vector<uint8_t>(encPayload.begin(), encPayload.end()), nullptr, kDeliveryFireAndForgetMs);

    QJsonObject result;
    result["sent"] = true;
    result["recipient"] = QString::fromStdString(recipientKey);
    result["topic"] = QString::fromStdString(topic);
    result["encrypted"] = true;
    return compact(result);
}

bool PilotImpl::messagingJoin(const std::string& groupId) {
    // Joining is a local decision about a group we were INVITED to (we hold its key). The
    // subscribe is fire-and-forget: its ACK rides the reply channel the delivery host drops,
    // and the pull path (agentPollTopics) reads joined groups regardless.
    std::string topic;
    std::string key = groupKeyFor(groupId, &topic, false);
    if (key.empty()) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE messaging_groups SET joined = 1 WHERE group_id = ?;",
                           -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, groupId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    if (isContextReady()) {
        initDeliveryModule();
        modules().delivery_module.subscribe(topic, nullptr, kDeliveryFireAndForgetMs);
    }
    return true;
}

std::string PilotImpl::messagingCreateGroup(const std::string& membersJson) {
    if (!isContextReady() || !db_) return "{\"error\": \"not initialized\"}";
    initDeliveryModule();

    std::string self = a2aSelfEncKey();
    if (self.empty()) self = agentNpk_;
    std::string groupId = genGroupId();
    std::string topic = "/pilot/1/group-" + groupId + "/proto";
    std::string key = genGroupKeyHex();

    // The creator is a member from the start (joined = 1); the key lives only in pilot.db.
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_,
                "INSERT OR REPLACE INTO messaging_groups "
                "(group_id, topic, key_hex, created_by, joined, created_at) VALUES (?, ?, ?, ?, 1, ?);",
                -1, &st, nullptr) == SQLITE_OK) {
            std::string ts = nowSecs();
            sqlite3_bind_text(st, 1, groupId.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, topic.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 3, key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 4, self.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 5, ts.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }
    // Fire-and-forget, like every other delivery call: the ACK is unreliable (host reply drop)
    // and used to abort group creation for no reason.
    modules().delivery_module.subscribe(topic, nullptr, kDeliveryFireAndForgetMs);

    // Each invite carries the group key, so each is sealed to that member alone. A plaintext
    // invite (the pre-2026-09-04 shape) would have handed the key to every relay on the path.
    QJsonObject invite;
    invite["type"] = QString("group_invite");
    invite["group_id"] = QString::fromStdString(groupId);
    invite["topic"] = QString::fromStdString(topic);
    invite["key"] = QString::fromStdString(key);
    invite["from"] = QString::fromStdString(self);
    std::string inviteStr = compact(invite);
    std::vector<uint8_t> inviteBytes(inviteStr.begin(), inviteStr.end());

    int invited = 0, failed = 0;
    for (const std::string& member : pilotParseMembers(membersJson)) {
        std::string memberKey = pilotRecipientKey(member);
        std::string sealed;
        try { sealed = eciesSerialize(eciesEncrypt(memberKey, inviteBytes)); }
        catch (...) { ++failed; continue; }
        std::string memberTopic = "/pilot/1/inbox-" + memberKey + "/proto";
        modules().delivery_module.send(
            memberTopic, std::vector<uint8_t>(sealed.begin(), sealed.end()), nullptr, kDeliveryFireAndForgetMs);
        ++invited;
    }

    QJsonObject result;
    result["group_id"] = QString::fromStdString(groupId);
    result["topic"] = QString::fromStdString(topic);
    result["invited"] = invited;
    if (failed) result["invites_failed"] = failed;
    result["encrypted"] = true;
    return compact(result);
}
