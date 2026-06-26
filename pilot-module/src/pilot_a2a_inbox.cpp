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

// Authenticate an INBOUND A2A request (H2): require a valid ES256K signature in _logos.signature
// over the canonical request bytes (envelope minus _logos.signature) produced by the request's
// _logos.signing_key, then TOFU-pin (_logos.sender_npk -> signing_key) in a DEDICATED
// pinned_request_identities table — NEVER the card pin (pinned_identities) — so a request can
// never poison the card/payout pin (P4). Mirrors verifyCardStatus's write-before-read TOFU (P5).
// db==nullptr -> signature-only (bare unit harness) with *firstContact=true. Returns true only
// for a signed, pin-consistent request; ambiguity -> false (drop).
bool verifyInboundRequest(const QJsonObject& req, sqlite3* db, bool* firstContact) {
    if (firstContact) *firstContact = false;
    QJsonObject logos = req["_logos"].toObject();
    std::string key = logos["signing_key"].toString().toStdString();
    std::string sig = logos["signature"].toString().toStdString();
    std::string npk = logos["sender_npk"].toString().toStdString();
    if (key.empty() || sig.empty()) return false;
    QJsonObject env = req;
    QJsonObject l2 = env["_logos"].toObject();
    l2.remove("signature");                                   // canonical excludes the signature
    env["_logos"] = l2;
    std::string canonical = QJsonDocument(env).toJson(QJsonDocument::Compact).toStdString();
    std::vector<uint8_t> bytes(canonical.begin(), canonical.end());
    if (!verifySignature(bytes, sig, key)) return false;      // (message, signatureHex, publicKeyHex)
    if (!db) { if (firstContact) *firstContact = true; return true; }   // bare harness: signature-only
    if (npk.empty()) return true;                             // signed, nothing to pin
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS pinned_request_identities ("
        "npk TEXT PRIMARY KEY, signing_key TEXT NOT NULL, first_seen TEXT NOT NULL);",
        nullptr, nullptr, nullptr);
    std::string ts = a2aNow();
    sqlite3_stmt* ins = nullptr;
    if (sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO pinned_request_identities (npk, signing_key, first_seen) "
            "VALUES (?, ?, ?);", -1, &ins, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(ins, 1, npk.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 2, key.c_str(),  -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 3, ts.c_str(),   -1, SQLITE_TRANSIENT);
        sqlite3_step(ins);
    }
    sqlite3_finalize(ins);
    if (firstContact) *firstContact = (sqlite3_changes(db) > 0);   // newly pinned by this message
    std::string pinned;
    sqlite3_stmt* sel = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT signing_key FROM pinned_request_identities WHERE npk = ?;",
            -1, &sel, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(sel, 1, npk.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(sel) == SQLITE_ROW && sqlite3_column_text(sel, 0))
            pinned = reinterpret_cast<const char*>(sqlite3_column_text(sel, 0));
    }
    sqlite3_finalize(sel);
    return !pinned.empty() && pinned == key;   // TOFU: first-seen key wins; mismatch -> drop
}

// Injection-safe, trust-tagged sender label for an owner prompt (L4). PURE function of the H2
// verdict — NO DB access (the verdict already came from verifyInboundRequest). A stranger's very
// first request is never shown as a "known peer".
QString a2aSenderDisplay(bool authenticated, bool firstContact, const QString& senderNpk) {
    std::string flat = a2aFlattenForPrompt(senderNpk.toStdString());
    if (!authenticated) return QString::fromStdString("UNVERIFIED " + flat);
    if (firstContact)   return QString::fromStdString("authenticated, first contact " + flat);
    return QString::fromStdString("authenticated known peer " + flat);
}

// Inbound wallet-send owner-approval prompt (M4): includes the payee `recipient` (previously
// omitted — the single most security-relevant field) and an optional `reason`, matching
// walletSend's own owner message. Flattens skill/recipient/reason; senderDisplay is pre-sanitized.
std::string a2aWalletSendApprovalMessage(const std::string& skill, const std::string& senderDisplay,
                                         int64_t amount, const std::string& recipient,
                                         const std::string& reason, const std::string& spendId) {
    std::string r = a2aFlattenForPrompt(reason);
    return "Peer agent task needs approval:\nSkill: " + a2aFlattenForPrompt(skill) +
           "\nFrom: " + senderDisplay +
           "\nAmount: " + std::to_string(amount) + " LEZ" +
           "\nTo: " + a2aFlattenForPrompt(recipient) +
           (r.empty() ? std::string() : ("\nReason: " + r)) +
           "\nExpires: 60 min\n/approve " + spendId + "\n/reject " + spendId;
}

// H3 — abuse limits on the SAFE auto-run (priced) skills (agent-ask). H2 already requires every
// inbound request to be signed and TOFU-pinned, so a flood is now attributable to a pinned npk;
// these caps bound the unpaid LLM cost a single pinned sender can inflict, and the size of any one
// request, BEFORE we run a billable completion. (Moving the LLM off the delivery thread is L6.)
static const size_t kA2AMaxServiceBytes     = 8192;   // max serviced-skill args payload (~prompt)
static const long   kA2AServiceWindowSec    = 60;     // rolling rate-limit window (seconds)
static const int    kA2AServiceMaxPerWindow = 10;     // max serviced requests per sender per window

// M3 — resource caps / bounding for the inbound task path. kA2AMaxParamsBytes gates the FULL
// stored message envelope (params_json) and is deliberately > the 8192 H3 serviced-args cap so
// the per-skill arg gate still fires first for serviced skills. kA2AInboundMaxPerWindow is the
// GLOBAL per-sender tasks/send flood cap across ALL skills (> the 10 agent-ask sublimit) and
// reuses kA2AServiceWindowSec. The TTL + row-cap bound how many TERMINAL inbound rows persist.
static const size_t kA2AMaxParamsBytes      = 16384;  // max params_json (full message envelope) bytes
static const int    kA2AInboundMaxPerWindow = 30;     // max tasks/send per sender per window (any skill)
static const long   kA2AInboundTaskTTLSec   = 86400;  // terminal inbound rows older than this are swept
static const int    kA2AInboundTaskMaxRows  = 5000;   // hard backstop on retained terminal inbound rows

// Count this sender's requests whose created_at is within the window (inclusive of the current
// row, which was already inserted). Pure read over inbound_tasks. When `skill` is EMPTY this
// counts ALL of the sender's tasks/send in the window (the M3 global per-sender flood gate);
// otherwise it is the per-skill H3 serviced sublimit. The bind index is tracked at runtime so
// the empty-skill form binds npk@1, since@2 (no `AND skill=?` clause / placeholder) while the
// per-skill form binds npk@1, skill@2, since@3.
static int a2aRecentSkillCount(sqlite3* db, const std::string& npk, const std::string& skill,
                               long sinceEpoch) {
    if (!db) return 0;
    std::string sql = "SELECT COUNT(*) FROM inbound_tasks WHERE sender_npk=? ";
    if (!skill.empty()) sql += "AND skill=? ";
    sql += "AND CAST(created_at AS INTEGER) >= ?;";
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr);
    int idx = 1;
    sqlite3_bind_text(st, idx++, npk.c_str(), -1, SQLITE_TRANSIENT);
    if (!skill.empty()) sqlite3_bind_text(st, idx++, skill.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, idx++, sinceEpoch);
    int n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

// M3 — bound how many TERMINAL inbound_tasks rows persist. (1) TTL sweep: drop terminal rows
// older than the TTL; in-flight rows (accepted/working/input-required) are NEVER touched. (2)
// Row-cap backstop: keep only the newest kA2AInboundTaskMaxRows terminal rows. Both deletes are
// built with std::to_string and use the subquery `id NOT IN (SELECT ... LIMIT k)` form (NOT a
// bare DELETE ... LIMIT) so they run on stock SQLite, which ships without
// SQLITE_ENABLE_UPDATE_DELETE_LIMIT. [FIX-C/FIX-F] PK is `id`.
void a2aEvictOldInboundTasks(sqlite3* db, long nowEpoch) {
    if (!db) return;
    // (1) TTL sweep — terminal rows older than the TTL. In-flight rows never touched.
    {
        std::string sql =
            "DELETE FROM inbound_tasks WHERE state IN ('completed','failed','canceled') "
            "AND CAST(created_at AS INTEGER) < " + std::to_string(nowEpoch - kA2AInboundTaskTTLSec) + ";";
        sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
    }
    // (2) Row-cap backstop — keep the newest kA2AInboundTaskMaxRows terminal rows.
    {
        std::string sql =
            "DELETE FROM inbound_tasks WHERE state IN ('completed','failed','canceled') "
            "AND id NOT IN (SELECT id FROM inbound_tasks WHERE state IN ('completed','failed','canceled') "
            "ORDER BY CAST(created_at AS INTEGER) DESC LIMIT " + std::to_string(kA2AInboundTaskMaxRows) + ");";
        sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
    }
}

void PilotImpl::inboundTaskSetState(const std::string& taskId, const std::string& state,
                                    const std::string& resultJson) {
    if (!db_) return;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_,
        "UPDATE inbound_tasks SET state=?, result_json=?, updated_at=? "
        "WHERE id=? AND state NOT IN ('completed','failed','canceled');", -1, &st, nullptr);
    std::string ts = a2aNow();
    sqlite3_bind_text(st, 1, state.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, resultJson.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, taskId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

std::string PilotImpl::processInboundRequest(const std::string& requestJson,
                                             const std::string& authenticatedNpk,
                                             bool senderFirstContact) {
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
        // OWNERSHIP BINDING (H2): only the original requester may cancel. Checked BEFORE the
        // -32002 terminal guard so a non-owner learns nothing about task state. Empty
        // authenticatedNpk == direct pure-FSM unit call -> binding skipped.
        if (!authenticatedNpk.empty() &&
            a2aCol(db_, taskId.toStdString(), "sender_npk") != authenticatedNpk)
            return a2aRpcError(rpcId, -32003, "unauthorized");
        if (state == "completed" || state == "failed" || state == "canceled")
            return a2aRpcError(rpcId, -32002, "task not cancelable");
        // Mark canceled FIRST (L3): resumeInboundTask only touches 'input-required', so the
        // canceled terminal is never clobbered; L5's monotonic guard double-protects it.
        inboundTaskSetState(taskId.toStdString(), "canceled", "");
        // L3 — quietly release any HELD spend a linked inbound wallet-send parked, so a later
        // owner /approve cannot move funds for a withdrawn task. releaseHeldSpend (NOT
        // rejectSpend) -> no misleading owner "rejected" notification (P7). Empty sid -> no-op.
        std::string sid = a2aCol(db_, taskId.toStdString(), "spend_request_id");
        if (!sid.empty()) releaseHeldSpend(sid);
        return a2aRpcTask(rpcId, taskId, "canceled", QJsonValue());
    }

    if (method == "tasks/sendSubscribe") {
        const QString taskId = req["params"].toObject()["id"].toString();
        std::string state = a2aCol(db_, taskId.toStdString(), "state");
        if (state.empty()) return a2aRpcError(rpcId, -32001, "task not found");
        // OWNERSHIP BINDING (H2): only the original requester may read a task's result_json,
        // so a third party can't exfiltrate the paid-for answer. Empty authenticatedNpk ==
        // direct pure-FSM unit call -> binding skipped.
        if (!authenticatedNpk.empty() &&
            a2aCol(db_, taskId.toStdString(), "sender_npk") != authenticatedNpk)
            return a2aRpcError(rpcId, -32003, "unauthorized");
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
        // M3 — reject an oversized FULL message envelope BEFORE we store or dispatch it, so a
        // single huge task can never bloat the row or the billable path. Placed AFTER the
        // idempotency cache check so a redelivery re-evaluates identically; the cap is larger
        // than the H3 serviced-args cap so a serviced skill still trips its own arg gate first.
        if (paramsStr.size() > kA2AMaxParamsBytes)
            return a2aRpcError(rpcId, -32005, "request exceeds size limit");
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

        // M3 — GLOBAL per-sender flood gate across ALL skills (not just the serviced sublimit).
        // H2 already requires every request to be signed + TOFU-pinned, so a flood is
        // attributable to one npk; this bounds the rows + work a single pinned sender can force
        // regardless of skill. Guarded by a non-empty authenticatedNpk so pure-FSM unit calls
        // (empty npk) skip it, mirroring the H3 serviced gate. The serviced agent-ask sublimit
        // (10) still fires first for that skill because 10 < kA2AInboundMaxPerWindow (30).
        if (!authenticatedNpk.empty()) {
            long since = std::stol(a2aNow()) - kA2AServiceWindowSec;
            if (a2aRecentSkillCount(db_, authenticatedNpk, "", since) > kA2AInboundMaxPerWindow) {
                inboundTaskSetState(taskId.toStdString(), "failed",
                    "{\"error\":\"rate limit exceeded; retry later\"}");
                return a2aRpcError(rpcId, -32006, "rate limit exceeded");
            }
        }
        // Opportunistic maintenance: sweep terminal inbound rows past the TTL / row cap on each
        // accepted send. `ts` is the insert epoch already computed above.
        a2aEvictOldInboundTasks(db_, std::stol(ts));

        if (skill == "ping") {
            inboundTaskSetState(taskId.toStdString(), "working", "");
            QJsonObject pong; pong["pong"] = true; pong["ts"] = QString::fromStdString(ts);
            std::string res = QJsonDocument(pong).toJson(QJsonDocument::Compact).toStdString();
            inboundTaskSetState(taskId.toStdString(), "completed", res);
            return a2aRpcTask(rpcId, taskId, "completed", pong);
        }
        if (skill == "capabilities") {
            inboundTaskSetState(taskId.toStdString(), "working", "");
            std::string cardStr = buildCard();
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
            // H3 — bound abuse of the billable LLM path BEFORE running it. An oversized request is
            // refused outright; an authenticated sender that exceeds its per-window quota is
            // rate-limited (unauthenticated callers can no longer reach here at all, post-H2). This
            // turns the previously-unmetered free-drain into a bounded, per-identity cost.
            if (argsStr.size() > kA2AMaxServiceBytes) {
                inboundTaskSetState(taskId.toStdString(), "failed",
                    "{\"error\":\"request exceeds size limit\"}");
                return a2aRpcError(rpcId, -32005, "request exceeds size limit");
            }
            if (!authenticatedNpk.empty()) {
                long since = std::stol(a2aNow()) - kA2AServiceWindowSec;
                if (a2aRecentSkillCount(db_, authenticatedNpk, skill.toStdString(), since)
                        > kA2AServiceMaxPerWindow) {
                    inboundTaskSetState(taskId.toStdString(), "failed",
                        "{\"error\":\"rate limit exceeded; retry later\"}");
                    return a2aRpcError(rpcId, -32006, "rate limit exceeded");
                }
            }
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
            // Re-read the PERSISTED state (L5): a re-entrant terminal (e.g. a tasks/cancel that
            // arrived during the dispatch) may have won the monotonic UPDATE, so reply with what
            // actually stuck and ship rv=null when our terminal lost the race.
            std::string persisted = a2aCol(db_, taskId.toStdString(), "state");
            QJsonValue rv = (persisted == state)
                ? (rd.isObject() ? QJsonValue(rd.object())
                   : (rd.isArray() ? QJsonValue(rd.array())
                                   : QJsonValue(QString::fromStdString(result))))
                : QJsonValue();
            return a2aRpcTask(rpcId, taskId, QString::fromStdString(persisted), rv);
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
            std::string reason = argsObj["reason"].toString().toStdString();   // M4: shown to owner
            if (recipient.empty()) {
                inboundTaskSetState(taskId.toStdString(), "failed",
                    "{\"error\":\"wallet-send missing recipient\"}");
                return a2aRpcError(rpcId, -32602, "wallet-send missing recipient");
            }

            sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);  // L8: create+link+input-required+HELD as one unit
            std::string sid = createSpendRequest(recipient, amount,
                "A2A wallet-send task " + taskId.toStdString() + " from " +
                a2aFlattenForPrompt(senderNpk.toStdString()));

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
            sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);  // L8: owner notification (RPC) runs after COMMIT

            sendToOwner(a2aWalletSendApprovalMessage(
                skill.toStdString(),
                a2aSenderDisplay(!authenticatedNpk.empty(), senderFirstContact, senderNpk).toStdString(),
                amount, recipient, reason, sid));
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
                "Skill: " + a2aFlattenForPrompt(skill.toStdString()) +
                "\nFrom: " + a2aSenderDisplay(!authenticatedNpk.empty(), senderFirstContact, senderNpk).toStdString() +
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

    // M3 — boot maintenance, AFTER the recover sweep above so a row it just failed (whose
    // created_at is unchanged) is only swept if it was already older than the TTL. Prune terminal
    // inbound rows past the TTL / row cap, and LRU-trim the discovered_agents card cache (TOFU
    // pins + in-flight outbound cards excluded).
    a2aEvictOldInboundTasks(db_, std::stol(a2aNow()));
    a2aEvictDiscoveryCache(db_);
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
    if (agentEncPriv_.empty() && agentEciesPriv_.empty()) return;
    // L1: decrypt with the dedicated ENCRYPTION key first (the key our card advertises as
    // _logos.enc_key and that peers encrypt to), then fall back to the legacy SIGNING key for a
    // pre-split peer that still encrypts to _logos.signing_key. Undecryptable -> drop (inaction).
    std::string request;
    if (!a2aTryDecrypt(encryptedPayload, request)) return;

    QJsonDocument reqDoc = QJsonDocument::fromJson(QByteArray::fromStdString(request));
    if (!reqDoc.isObject()) return;
    QJsonObject reqObj = reqDoc.object();

    // AUTHENTICATE every network request before dispatch (H2). Drop unsigned / forged-signature /
    // pin-mismatch requests (ambiguity -> inaction). firstContact feeds the owner prompt's trust
    // label so a stranger's very first request is never shown as a known peer (L4).
    bool firstContact = false;
    if (!verifyInboundRequest(reqObj, db_, &firstContact)) return;

    QJsonObject logosExt = reqObj["_logos"].toObject();
    std::string authNpk = logosExt["sender_npk"].toString().toStdString();
    std::string reply = processInboundRequest(request, authNpk, firstContact);

    std::string replyTopic = logosExt["reply_topic"].toString().toStdString();
    // Encrypt the reply to the requester's ECIES key (the single A2A reply keypair),
    // NOT its wallet/messaging npk. The requester decrypts with agentEciesPriv_.
    std::string senderEcies = logosExt["sender_ecies"].toString().toStdString();
    if (!replyTopic.empty() && !senderEcies.empty())
        replyToPeer(replyTopic, senderEcies, reply);
}
