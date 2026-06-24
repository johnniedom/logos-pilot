#include "pilot_impl.h"
#include "pilot_a2a.h"
#include "pilot_crypto.h"
#include "pilot_skill.h"
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
#include <QJsonArray>
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

// Unwrap the FLAT skill args from the A2A message envelope. The transport wraps the real
// args as {role, parts:[{type:text,text:<args>}]} (see agentTask), but every skill reads
// flat top-level keys (obj["path"], obj["recipient"], ...). So we pull parts[0].text — an
// object, or a JSON string we parse — and pass THAT to dispatch. We fall back to the
// message object itself only when there is no parts/text (flat callers and older tests).
static QJsonObject a2aSkillArgs(const QJsonObject& message) {
    QJsonArray parts = message.value("parts").toArray();
    if (!parts.isEmpty()) {
        QJsonValue text = parts.at(0).toObject().value("text");
        if (text.isObject()) return text.toObject();
        if (text.isString()) {
            QJsonDocument d = QJsonDocument::fromJson(text.toString().toUtf8());
            if (d.isObject()) return d.object();
        }
    }
    return message;
}

// FIX 2 — RISKY classification. An A2A peer is a STRANGER, not the owner. A skill that touches
// local files, the agent's messaging identity, or reads agent state must NEVER auto-run at a
// stranger's request: storage-* would read/exfiltrate local files and leak our stored-file
// inventory; messaging-* would send/relay/forge messages under OUR identity (open relay /
// impersonation); program-* would act on-chain as us. We do NOT try to "secure" these skills —
// we route them to the OWNER GATE (input-required + sendToOwner) regardless of amount and run
// NOTHING until/unless the owner acts. wallet-send (moves funds) has its own spend-backed gate
// just below and is handled there, so it is intentionally NOT matched here. SAFE skills (ping,
// capabilities, and the a2aServiceCatalog() set — agent-ask) auto-run before we reach this.
static bool a2aRiskyOwnerGated(const QString& skill) {
    return skill.startsWith("storage-") ||
           skill.startsWith("messaging-") ||
           skill.startsWith("program-");
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
        // The requester puts its ECIES public key here; we store it and encrypt EVERY
        // reply (replyToPeer) to it. ONE ECIES keypair is used for all A2A reply
        // encryption on both legs (requester decrypts with its agentEciesPriv_).
        const QString senderEcies = logosExt["sender_ecies"].toString();
        const QString replyTopic = logosExt["reply_topic"].toString();
        if (taskId.isEmpty() || skill.isEmpty() || senderNpk.isEmpty())
            return a2aRpcError(rpcId, -32602, "missing task id, skill, or sender");

        // IDEMPOTENCY (Waku is at-least-once): a redelivered tasks/send must NOT re-run a
        // skill or re-execute a transfer (double-spend). If this task id was already seen
        // in a working/terminal state OR it already carries a linked spend request, reply
        // with the CACHED result (mirrors tasks/sendSubscribe) instead of INSERT OR REPLACE
        // + re-dispatch. We re-process only a row that does not yet exist.
        {
            std::string priorState = a2aCol(db_, taskId.toStdString(), "state");
            std::string priorSpend = a2aCol(db_, taskId.toStdString(), "spend_request_id");
            bool alreadyHandled = !priorSpend.empty() ||
                priorState == "working" || priorState == "input-required" ||
                priorState == "completed" || priorState == "failed" || priorState == "canceled";
            if (alreadyHandled) {
                std::string cached = a2aCol(db_, taskId.toStdString(), "result_json");
                QJsonDocument rd = QJsonDocument::fromJson(QByteArray::fromStdString(cached));
                return a2aRpcTask(rpcId, taskId, QString::fromStdString(priorState),
                    rd.isObject() ? QJsonValue(rd.object()) : QJsonValue());
            }
        }

        // Persist the full message envelope for fidelity, but dispatch the FLAT inner args
        // (params.message.parts[0].text) so parameterized skills find their top-level keys.
        QJsonObject messageObj = params["message"].toObject();
        QJsonObject argsObj = a2aSkillArgs(messageObj);
        std::string argsStr = QJsonDocument(argsObj).toJson(QJsonDocument::Compact).toStdString();
        std::string paramsStr =
            QJsonDocument(messageObj).toJson(QJsonDocument::Compact).toStdString();
        std::string ts = a2aNow();
        sqlite3_stmt* ins = nullptr;
        sqlite3_prepare_v2(db_,
            "INSERT OR REPLACE INTO inbound_tasks "
            "(id, sender_npk, sender_ecies, reply_topic, skill, params_json, state, created_at, updated_at) "
            "VALUES (?, ?, ?, ?, ?, ?, 'accepted', ?, ?);", -1, &ins, nullptr);
        sqlite3_bind_text(ins, 1, taskId.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 2, senderNpk.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 3, senderEcies.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 4, replyTopic.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 5, skill.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 6, paramsStr.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 7, ts.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 8, ts.c_str(), -1, SQLITE_TRANSIENT);
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

        // SAFE SERVICE skills (asker pays the doer): we AUTO-RUN the real skill for a
        // stranger and get PAID for it. SAFE means pure compute with no side effects on this
        // agent (no local files, no messaging identity, no funds) — today: agent-ask. The
        // requester pays us — so there is NO spend, NO owner gate, and we NEVER transfer money
        // to the requester. The serviced set is the SINGLE-SOURCE SAFE catalog (FIX 4a + FIX 2)
        // that ALSO drives agentCard()'s pricing, so we never advertise an autonomous price for
        // a skill we don't auto-service here (and vice versa), and a RISKY skill can never leak
        // into this auto-run path (it is owner-gated below). Drive working -> completed only if
        // the real dispatch succeeded, else failed with the real error (honesty). The A2A skill
        // id (dashes) maps back to the registry name (dots); underscores are preserved.
        bool serviced = false;
        for (const auto& svc : a2aServiceCatalog())
            if (skill == QString::fromUtf8(svc.id)) { serviced = true; break; }
        if (serviced) {
            inboundTaskSetState(taskId.toStdString(), "working", "");
            std::string regName = skill.toStdString();
            for (auto& ch : regName) if (ch == '-') ch = '.';
            std::string result = registry_
                ? registry_->dispatch(regName, argsStr)
                : std::string("{\"error\":\"registry not initialized\"}");
            QJsonDocument rd = QJsonDocument::fromJson(QByteArray::fromStdString(result));
            // HONEST SUCCESS CONTRACT (a2aResultIsSuccess, single-source in pilot_a2a.cpp): a
            // task is 'completed' ONLY on an EXPLICIT, positive success signal — we never pay
            // for unproven or failed work. A result reading {"joined":false}/{"success":false},
            // status in {failed,error}, or an opaque/ambiguous shape (bare string, empty object/
            // array, unparseable) is 'failed' (no pay). Ambiguity -> 'failed', NEVER 'completed'.
            std::string state = a2aResultIsSuccess(result) ? "completed" : "failed";
            inboundTaskSetState(taskId.toStdString(), state, result);
            QJsonValue rv = rd.isObject() ? QJsonValue(rd.object())
                          : (rd.isArray() ? QJsonValue(rd.array())
                                          : QJsonValue(QString::fromStdString(result)));
            return a2aRpcTask(rpcId, taskId, QString::fromStdString(state), rv);
        }

        // AGENT-SPENDING skill: performing wallet-send spends the AGENT'S OWN funds. A
        // requester is NOT the owner and is NOT authenticated as such, so an autonomous
        // below-threshold inbound wallet-send would let ANY peer move our funds. We
        // therefore owner-gate EVERY inbound wallet-send regardless of amount — never
        // auto-execute an agent-spend at a stranger's request. (The below-threshold
        // autonomous path stays for the OWNER's own outbound asker-pays-doer loop, not
        // here.) The recipient is the payee named in the task params — NEVER the requester
        // (senderNpk). The requester is the asker, not a payee.
        if (skill == "wallet-send") {
            int64_t amount = static_cast<int64_t>(argsObj["amount"].toDouble());
            std::string recipient = argsObj["recipient"].toString().toStdString();
            if (recipient.empty()) {
                inboundTaskSetState(taskId.toStdString(), "failed",
                    "{\"error\":\"wallet-send missing recipient\"}");
                return a2aRpcError(rpcId, -32602, "wallet-send missing recipient");
            }

            std::string sid = createSpendRequest(recipient, amount,
                "A2A wallet-send task " + taskId.toStdString() + " from " + senderNpk.toStdString());

            // Link the spend request to the task up front so resumeInboundTask can find
            // the task on owner approve/reject/expiry regardless of which gate we take.
            std::string tsLink = a2aNow();
            sqlite3_stmt* link = nullptr;
            sqlite3_prepare_v2(db_,
                "UPDATE inbound_tasks SET spend_request_id=?, updated_at=? WHERE id=?;", -1, &link, nullptr);
            sqlite3_bind_text(link, 1, sid.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(link, 2, tsLink.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(link, 3, taskId.toUtf8().constData(), -1, SQLITE_TRANSIENT);
            sqlite3_step(link);
            sqlite3_finalize(link);

            // EVERY inbound wallet-send -> owner-approval gate. Unlike the OUTBOUND
            // asker-pays-doer loop (the owner's own agent decided to ask and pays the
            // doer's published payout), an inbound wallet-send is a stranger asking us to
            // move OUR funds; there is no autonomous below-threshold path here. Park the
            // spend HELD and ask the owner.
            std::string ts2 = a2aNow();
            sqlite3_stmt* up = nullptr;
            sqlite3_prepare_v2(db_,
                "UPDATE inbound_tasks SET state='input-required', updated_at=? WHERE id=?;",
                -1, &up, nullptr);
            sqlite3_bind_text(up, 1, ts2.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(up, 2, taskId.toUtf8().constData(), -1, SQLITE_TRANSIENT);
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

        // RISKY skills (storage-*, messaging-*, program-*): an A2A peer is a STRANGER, so we
        // NEVER auto-run these (that would read/exfiltrate local files, relay/forge messages
        // under our identity, or act on-chain as us). There is no agent spend to gate here
        // (the requester pays US), so unlike wallet-send we open NO spend request — we simply
        // park the task at input-required and ask the OWNER, dispatching NOTHING. No file is
        // read, no message is sent, no program runs until/unless the owner acts. The peer can
        // poll (tasks/sendSubscribe) or withdraw (tasks/cancel) while it waits. This closes the
        // exfil / open-relay / stranger-action findings by ROUTING through owner approval
        // rather than executing. (Owner EXECUTION of an approved non-spend risky task is a
        // follow-up; the SECURITY guarantee — never autonomous — holds today.)
        if (a2aRiskyOwnerGated(skill)) {
            std::string ts2 = a2aNow();
            sqlite3_stmt* up = nullptr;
            sqlite3_prepare_v2(db_,
                "UPDATE inbound_tasks SET state='input-required', updated_at=? WHERE id=?;",
                -1, &up, nullptr);
            sqlite3_bind_text(up, 1, ts2.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(up, 2, taskId.toUtf8().constData(), -1, SQLITE_TRANSIENT);
            sqlite3_step(up);
            sqlite3_finalize(up);

            sendToOwner("Peer agent requests a privileged skill (owner approval required):\n"
                "Skill: " + skill.toStdString() +
                "\nFrom: " + senderNpk.toStdString() +
                "\nThis skill touches local files, this agent's messaging identity, or on-chain "
                "state, so it will NOT run autonomously for a stranger.\nTask id: " +
                taskId.toStdString());
            QJsonObject pend; pend["reason"] = QString("requires owner approval (privileged skill)");
            return a2aRpcTask(rpcId, taskId, "input-required", pend);
        }

        // Anything else (e.g. wallet-balance, which would leak our balance to a stranger, or
        // an unknown skill): honest 'unsupported'. We never fabricate a result, never create a
        // spend, never pay the requester. (program-* is caught by the risky owner-gate above.)
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
        "SELECT id, sender_ecies, reply_topic FROM inbound_tasks "
        "WHERE spend_request_id=? AND state='input-required';", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, spendRequestId.c_str(), -1, SQLITE_TRANSIENT);
    std::string taskId, senderEcies, replyTopic;
    if (sqlite3_step(st) == SQLITE_ROW) {
        taskId = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        if (sqlite3_column_text(st, 1))
            senderEcies = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
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
    // Encrypt the reply to the requester's ECIES key (it decrypts with agentEciesPriv_).
    replyToPeer(replyTopic, senderEcies,
                QJsonDocument(note).toJson(QJsonDocument::Compact).toStdString());
}

// Encrypted, retrying publish to a peer topic. Returns true only if the delivery
// module accepted the message (same honesty contract as deliverToOwner). "Accepted"
// means handed to the network, not read by the peer.
bool PilotImpl::replyToPeer(const std::string& topic, const std::string& recipientKey,
                            const std::string& json) {
    if (!logosAPI_ || topic.empty()) return false;

    // SIGN THE REPLY (FIX 1 — the BLOCKER). Encrypting the reply to the requester's PUBLIC
    // key authenticates NOTHING: the reply topic and that public key are PUBLIC, so any
    // observer could encrypt a forged {"status":{"state":"completed"}} and force the asker to
    // pay. So we SIGN the reply's canonical bytes with our ECIES private key (the same ES256K
    // scheme as the Agent Card) and attach _logos.signing_key + _logos.signature to the
    // envelope. The canonical bytes are the envelope WITH _logos.signing_key present and
    // WITHOUT _logos.signature, so the asker reproduces them by removing the signature and
    // verifies against the doer's pinned signing_key. We NEVER fabricate a signature: with no
    // private key (or a non-object body) we leave the reply unsigned and the asker will
    // (correctly) refuse to settle on it.
    std::string toSend = json;
    if (!agentEciesPriv_.empty() && !agentEciesPub_.empty()) {
        QJsonDocument d = QJsonDocument::fromJson(QByteArray::fromStdString(json));
        if (d.isObject()) {
            QJsonObject env = d.object();
            QJsonObject logos = env.value("_logos").toObject();
            logos.remove("signature");                                  // canonical excludes the sig
            logos["signing_key"] = QString::fromStdString(agentEciesPub_);
            env["_logos"] = logos;
            std::string canonical = QJsonDocument(env).toJson(QJsonDocument::Compact).toStdString();
            try {
                std::vector<uint8_t> bytes(canonical.begin(), canonical.end());
                logos["signature"] = QString::fromStdString(signMessage(bytes, agentEciesPriv_));
                env["_logos"] = logos;
                toSend = QJsonDocument(env).toJson(QJsonDocument::Compact).toStdString();
            } catch (const std::exception&) {
                // Honest failure: send unsigned rather than fabricate a signature.
            }
        }
    }

    std::string payload;
    try {
        std::vector<uint8_t> plain(toSend.begin(), toSend.end());
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
    // Encrypt the reply to the requester's ECIES key (the single A2A reply keypair),
    // NOT its wallet/messaging npk. The requester decrypts with agentEciesPriv_.
    std::string senderEcies = logosExt["sender_ecies"].toString().toStdString();
    if (!replyTopic.empty() && !senderEcies.empty())
        replyToPeer(replyTopic, senderEcies, reply);
}
