#include "pilot_impl.h"
#include "pilot_crypto.h"
#include "logos_api.h"
#include "logos_api_client.h"
#include "logos_mode.h"
#include <sqlite3.h>
#include <chrono>
#include <thread>
#include <vector>
#include <QString>
#include <QVariant>
#include <QJsonDocument>
#include <QJsonObject>
#include <QByteArray>
#include <QDebug>

// Inbound A2A task server. A peer agent sends a JSON-RPC task to our inbox; we run
// it through a lifecycle (accepted -> working -> input-required -> completed/failed/
// canceled) and reply. Safe skills (ping, capabilities) auto-complete; costly skills
// are held behind owner approval (the spending FSM). The pure state machine in
// processInboundRequest is proven red->green against real SQLite (/tmp/a2a/test_inbox.cpp)
// and re-run in tests/test_a2a_inbox.cpp.

static const Timeout INBOX_TIMEOUT(15000);

static std::string a2aNow() {
    auto n = std::chrono::system_clock::now();
    return std::to_string(std::chrono::duration_cast<std::chrono::seconds>(n.time_since_epoch()).count());
}

static std::string a2aCol(sqlite3* db, const std::string& id, const char* c) {
    std::string sql = std::string("SELECT ") + c + " FROM inbound_tasks WHERE id=?;";
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr);
    sqlite3_bind_text(st, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    std::string v;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
        v = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return v;
}

static std::string a2aRpcError(const QString& id, int code, const QString& msg) {
    QJsonObject e; e["code"] = code; e["message"] = msg;
    QJsonObject r; r["jsonrpc"] = QString("2.0"); r["id"] = id; r["error"] = e;
    return QJsonDocument(r).toJson(QJsonDocument::Compact).toStdString();
}

static std::string a2aRpcTask(const QString& rpcId, const QString& taskId,
                              const QString& state, const QJsonValue& result) {
    QJsonObject status; status["state"] = state;
    QJsonObject task; task["id"] = taskId; task["status"] = status;
    if (!result.isNull()) task["result"] = result;
    QJsonObject r; r["jsonrpc"] = QString("2.0"); r["id"] = rpcId; r["result"] = task;
    return QJsonDocument(r).toJson(QJsonDocument::Compact).toStdString();
}

void PilotImpl::inboundTaskSetState(const std::string& taskId, const std::string& state,
                                    const std::string& resultJson) {
    if (!db_) return;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_,
        "UPDATE inbound_tasks SET state=?, result_json=?, updated_at=? WHERE id=?;", -1, &st, nullptr);
    std::string ts = a2aNow();
    sqlite3_bind_text(st, 1, state.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, resultJson.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, taskId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

std::string PilotImpl::processInboundRequest(const std::string& requestJson) {
    if (!db_) return a2aRpcError("", -32603, "agent not initialized");

    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(requestJson));
    if (!doc.isObject()) return a2aRpcError("", -32700, "parse error");
    QJsonObject req = doc.object();
    const QString rpcId = req["id"].toString();
    const QString method = req["method"].toString();

    if (method == "tasks/cancel") {
        const QString taskId = req["params"].toObject()["id"].toString();
        std::string state = a2aCol(db_, taskId.toStdString(), "state");
        if (state.empty()) return a2aRpcError(rpcId, -32001, "task not found");
        if (state == "completed" || state == "failed" || state == "canceled")
            return a2aRpcError(rpcId, -32002, "task not cancelable");
        inboundTaskSetState(taskId.toStdString(), "canceled", "");
        return a2aRpcTask(rpcId, taskId, "canceled", QJsonValue());
    }

    if (method == "tasks/sendSubscribe") {
        const QString taskId = req["params"].toObject()["id"].toString();
        std::string state = a2aCol(db_, taskId.toStdString(), "state");
        if (state.empty()) return a2aRpcError(rpcId, -32001, "task not found");
        std::string result = a2aCol(db_, taskId.toStdString(), "result_json");
        QJsonDocument rd = QJsonDocument::fromJson(QByteArray::fromStdString(result));
        return a2aRpcTask(rpcId, taskId, QString::fromStdString(state),
            rd.isObject() ? QJsonValue(rd.object()) : QJsonValue());
    }

    if (method == "tasks/send") {
        QJsonObject params = req["params"].toObject();
        QJsonObject logosExt = req["_logos"].toObject();
        const QString taskId = params["id"].toString();
        const QString skill = params["metadata"].toObject()["skill"].toString();
        const QString senderNpk = logosExt["sender_npk"].toString();
        const QString replyTopic = logosExt["reply_topic"].toString();
        if (taskId.isEmpty() || skill.isEmpty() || senderNpk.isEmpty())
            return a2aRpcError(rpcId, -32602, "missing task id, skill, or sender");

        std::string paramsStr =
            QJsonDocument(params["message"].toObject()).toJson(QJsonDocument::Compact).toStdString();
        std::string ts = a2aNow();
        sqlite3_stmt* ins = nullptr;
        sqlite3_prepare_v2(db_,
            "INSERT OR REPLACE INTO inbound_tasks "
            "(id, sender_npk, reply_topic, skill, params_json, state, created_at, updated_at) "
            "VALUES (?, ?, ?, ?, ?, 'accepted', ?, ?);", -1, &ins, nullptr);
        sqlite3_bind_text(ins, 1, taskId.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 2, senderNpk.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 3, replyTopic.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 4, skill.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 5, paramsStr.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 6, ts.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 7, ts.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(ins);
        sqlite3_finalize(ins);

        if (skill == "ping") {
            inboundTaskSetState(taskId.toStdString(), "working", "");
            QJsonObject pong; pong["pong"] = true; pong["ts"] = QString::fromStdString(ts);
            std::string res = QJsonDocument(pong).toJson(QJsonDocument::Compact).toStdString();
            inboundTaskSetState(taskId.toStdString(), "completed", res);
            return a2aRpcTask(rpcId, taskId, "completed", pong);
        }
        if (skill == "capabilities") {
            inboundTaskSetState(taskId.toStdString(), "working", "");
            std::string cardStr = agentCard();
            inboundTaskSetState(taskId.toStdString(), "completed", cardStr);
            QJsonDocument cd = QJsonDocument::fromJson(QByteArray::fromStdString(cardStr));
            return a2aRpcTask(rpcId, taskId, "completed",
                cd.isObject() ? QJsonValue(cd.object()) : QJsonValue(QString::fromStdString(cardStr)));
        }

        static const char* kCostly[] = {"wallet-send", "storage-upload", "storage-download",
            "storage-share", "messaging-send", "program-query", "program-call", "program-deploy"};
        bool costly = false;
        for (const char* c : kCostly) if (skill == c) { costly = true; break; }
        if (costly) {
            QJsonObject msg = params["message"].toObject();
            int64_t amount = 10;
            std::string recipient = senderNpk.toStdString();
            if (skill == "wallet-send") {
                amount = static_cast<int64_t>(msg["amount"].toDouble());
                recipient = msg["recipient"].toString().toStdString();
            } else if (skill == "program-deploy") amount = 100;
            else if (skill == "messaging-send") amount = 1;
            else if (skill == "storage-upload") amount = 10;
            else if (skill == "storage-download" || skill == "storage-share") amount = 5;

            std::string sid = createSpendRequest(recipient, amount,
                "A2A task " + taskId.toStdString() + ": " + skill.toStdString() +
                " from " + senderNpk.toStdString());
            std::string ts2 = a2aNow();
            sqlite3_stmt* up = nullptr;
            sqlite3_prepare_v2(db_,
                "UPDATE inbound_tasks SET state='input-required', spend_request_id=?, updated_at=? WHERE id=?;",
                -1, &up, nullptr);
            sqlite3_bind_text(up, 1, sid.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(up, 2, ts2.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(up, 3, taskId.toUtf8().constData(), -1, SQLITE_TRANSIENT);
            sqlite3_step(up);
            sqlite3_finalize(up);

            sqlite3_stmt* hold = nullptr;
            sqlite3_prepare_v2(db_,
                "UPDATE spend_requests SET state='HELD', updated_at=? WHERE id=?;", -1, &hold, nullptr);
            sqlite3_bind_text(hold, 1, ts2.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(hold, 2, sid.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(hold);
            sqlite3_finalize(hold);

            sendToOwner("Peer agent task needs approval:\nSkill: " + skill.toStdString() +
                "\nFrom: " + senderNpk.toStdString() + "\nAmount: " + std::to_string(amount) +
                " LEZ\nExpires: 60 min\n/approve " + sid + "\n/reject " + sid);
            QJsonObject pend; pend["reason"] = QString("awaiting owner approval");
            return a2aRpcTask(rpcId, taskId, "input-required", pend);
        }

        inboundTaskSetState(taskId.toStdString(), "failed", "{\"error\":\"unsupported skill\"}");
        return a2aRpcError(rpcId, -32004, "unsupported skill");
    }

    return a2aRpcError(rpcId, -32601, "method not found");
}

// In-flight tasks that died with the previous process can't resume; fail them
// honestly so peers aren't left waiting. input-required tasks survive (their linked
// spend request drives them via approve/reject/expiry).
void PilotImpl::inboundTasksRecover() {
    if (!db_) return;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_,
        "UPDATE inbound_tasks SET state='failed', result_json='{\"error\":\"agent restarted\"}', "
        "updated_at=? WHERE state IN ('accepted','working');", -1, &st, nullptr);
    std::string ts = a2aNow();
    sqlite3_bind_text(st, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

// Owner decision (or expiry) on a linked spend request -> drive the held peer task
// to a terminal state and notify the peer on its reply topic.
void PilotImpl::resumeInboundTask(const std::string& spendRequestId, bool approved,
                                  const std::string& detail) {
    if (!db_ || spendRequestId.empty()) return;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT id, sender_npk, reply_topic FROM inbound_tasks "
        "WHERE spend_request_id=? AND state='input-required';", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, spendRequestId.c_str(), -1, SQLITE_TRANSIENT);
    std::string taskId, senderNpk, replyTopic;
    if (sqlite3_step(st) == SQLITE_ROW) {
        taskId = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        senderNpk = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        replyTopic = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    }
    sqlite3_finalize(st);
    if (taskId.empty()) return;   // a plain owner spend, no linked peer task

    const std::string state = approved ? "completed" : "failed";
    inboundTaskSetState(taskId, state, "{\"detail\":\"" + detail + "\"}");

    QJsonObject status; status["state"] = QString::fromStdString(state);
    QJsonObject task; task["id"] = QString::fromStdString(taskId);
    task["status"] = status; task["detail"] = QString::fromStdString(detail);
    QJsonObject note; note["jsonrpc"] = QString("2.0");
    note["method"] = QString("tasks/statusUpdate"); note["params"] = task;
    replyToPeer(replyTopic, senderNpk,
                QJsonDocument(note).toJson(QJsonDocument::Compact).toStdString());
}

// Encrypted, retrying publish to a peer topic. Returns true only if the delivery
// module accepted the message (same honesty contract as deliverToOwner). "Accepted"
// means handed to the network, not read by the peer.
bool PilotImpl::replyToPeer(const std::string& topic, const std::string& recipientKey,
                            const std::string& json) {
    if (!logosAPI_ || topic.empty()) return false;
    std::string payload;
    try {
        std::vector<uint8_t> plain(json.begin(), json.end());
        ECIESCiphertext ct = eciesEncrypt(recipientKey, plain);
        payload = eciesSerialize(ct);
    } catch (...) {
        qWarning() << "[pilot] replyToPeer: encryption failed";
        return false;
    }
    for (int attempt = 0; attempt < 3; ++attempt) {
        auto* delivery = logosAPI_->getClient("delivery_module");
        if (delivery && delivery->isConnected()) {
            QVariant r = delivery->invokeRemoteMethod(
                "delivery_module", "send",
                QString::fromStdString(topic), QString::fromStdString(payload), INBOX_TIMEOUT);
            if (!r.isNull() && !r.toString().isEmpty()) return true;
        }
        if (attempt < 2) std::this_thread::sleep_for(std::chrono::milliseconds(250 * (attempt + 1)));
    }
    qWarning() << "[pilot] replyToPeer: delivery failed after 3 attempts";
    return false;
}

// Raw encrypted payload from our inbox topic -> decrypt -> state machine -> reply.
// Undecryptable/malformed input is dropped (ambiguity defaults to inaction).
void PilotImpl::handleInboundA2A(const std::string& encryptedPayload) {
    if (agentEciesPriv_.empty()) return;
    std::string request;
    try {
        ECIESCiphertext ct = eciesDeserialize(encryptedPayload);
        std::vector<uint8_t> plain = eciesDecrypt(agentEciesPriv_, ct);
        request.assign(plain.begin(), plain.end());
    } catch (...) { return; }

    std::string reply = processInboundRequest(request);

    QJsonDocument reqDoc = QJsonDocument::fromJson(QByteArray::fromStdString(request));
    if (!reqDoc.isObject()) return;
    QJsonObject logosExt = reqDoc.object()["_logos"].toObject();
    std::string replyTopic = logosExt["reply_topic"].toString().toStdString();
    std::string senderNpk = logosExt["sender_npk"].toString().toStdString();
    if (!replyTopic.empty() && !senderNpk.empty())
        replyToPeer(replyTopic, senderNpk, reply);
}
