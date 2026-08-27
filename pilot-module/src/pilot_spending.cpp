#include "pilot_impl.h"
// Generated per-build; typed client for lez_core (see pilot_impl.h).
#include "logos_sdk.h"
#include <sqlite3.h>
#include <sstream>
#include <chrono>
#include <random>
#include <cstring>
#include <vector>
#include <QString>
#include <QVariant>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

namespace {
constexpr const char* kWalletModule = "lez_core";
// Keep in step with kWalletSyncTimeoutMs in pilot_identity.cpp. Catching the wallet up to
// chain head took a measured 37.2s for ~3500 blocks and grows with the chain, so a short
// ceiling silently truncates the sync. Deliberately generous: it is a ceiling, not a wait.
constexpr int kWalletSyncTimeoutMs = 600000;   // 10 minutes
}  // namespace

static std::string amountToHexLE(int64_t amount) {
    uint8_t bytes[16] = {};
    for (int i = 0; i < 8 && amount > 0; i++) {
        bytes[i] = static_cast<uint8_t>(amount & 0xFF);
        amount >>= 8;
    }
    char hex[33];
    for (int i = 0; i < 16; i++)
        snprintf(hex + i * 2, 3, "%02x", bytes[i]);
    hex[32] = '\0';
    return std::string(hex);
}

// Execute a private (shielded) transfer, choosing the right wallet method by recipient form:
//   - recipient is a keys JSON ({nullifier_public_key, viewing_public_key}) -> transfer_private
//     (external payee whose public keys we were given, e.g. from an Agent Card)
//   - recipient is a plain 32-byte account id hex -> transfer_private_owned
//     (a private account this wallet owns)
// A bare account id cannot be shielded-paid without the payee's keys, so id form is
// only valid for owned recipients.
static std::string doPrivateTransfer(LezCore& wallet, const std::string& fromId,
                                     const std::string& recipient, int64_t amount) {
    const bool hasKeys = recipient.find("nullifier_public_key") != std::string::npos
                      || recipient.find("viewing_public_key") != std::string::npos;
    // A shielded transfer generates a real RISC0 proof when RISC0_DEV_MODE=0: ~44 min wall on a
    // dev box (dev mode returns in seconds). The old 120s cap made every real-proof transfer
    // report TX_FAILED at the 2-min mark while proving was still in flight. 60 min covers the
    // documented ~44 min with margin; it is only a ceiling, so dev-mode transfers are unaffected.
    const std::string amountHex = amountToHexLE(amount);
    return hasKeys
        ? wallet.transfer_private(fromId, recipient, amountHex, nullptr, 3600000)
        : wallet.transfer_private_owned(fromId, recipient, amountHex, nullptr, 3600000);
}

// A transfer result is JSON: {"error":"...","success":bool,"tx_hash":"..."} in a string
// (empty on transport failure). Parse the success flag explicitly — substring-matching is
// wrong because the "error" key matches even when error is empty, and a real error message
// may not contain the word "fail".
static bool transferSucceeded(const std::string& result) {
    QJsonDocument d = QJsonDocument::fromJson(QString::fromStdString(result).toUtf8());
    return d.isObject() && d.object().value("success").toBool();
}

// Parse the tx_hash the transfer result already carries ({"error","success","tx_hash"}, see
// above); previously discarded. Empty on unparseable/missing.
static std::string transferTxHash(const std::string& result) {
    QJsonDocument d = QJsonDocument::fromJson(QString::fromStdString(result).toUtf8());
    return d.isObject() ? d.object().value("tx_hash").toString().toStdString() : std::string();
}

static std::string generateId() {
    auto now = std::chrono::high_resolution_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    std::mt19937_64 rng(ns);
    std::ostringstream ss;
    ss << std::hex << rng();
    return ss.str();
}

static std::string currentTimestamp() {
    auto now = std::chrono::system_clock::now();
    return std::to_string(std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());
}

// Advance an outbound A2A task linked to `spendId` to a terminal state on the owner's
// decision (M6): 'paid' when the transfer went through, 'pay-failed' otherwise. Only a row
// actually waiting on this spend ('awaiting-approval'/'settling') is moved — we never
// overwrite an already-terminal 'paid'/'pay-failed'/'canceled' or touch an unrelated row.
// A plain owner spend with no linked outbound task is a harmless no-op (0 rows).
static void advanceLinkedOutboundTask(sqlite3* db, const std::string& spendId, const char* outState) {
    if (!db || spendId.empty()) return;
    sqlite3_stmt* u = nullptr;
    sqlite3_prepare_v2(db,
        "UPDATE outbound_tasks SET state=?, updated_at=? WHERE spend_request_id=? "
        "AND state IN ('awaiting-approval','settling');", -1, &u, nullptr);
    std::string ts = currentTimestamp();
    sqlite3_bind_text(u, 1, outState, -1, SQLITE_STATIC);
    sqlite3_bind_text(u, 2, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(u, 3, spendId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(u);
    sqlite3_finalize(u);
}

std::string PilotImpl::createSpendRequest(const std::string& recipient, int64_t amount, const std::string& reason) {
    if (!db_) return "{\"error\": \"not initialized\"}";

    std::string id = generateId();
    std::string now = currentTimestamp();
    int64_t expiresAt = std::stoll(now) + 3600;

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "INSERT INTO spend_requests (id, recipient, amount, reason, state, created_at, updated_at, expires_at) "
        "VALUES (?, ?, ?, ?, 'CREATED', ?, ?, ?);",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, recipient.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, amount);
    sqlite3_bind_text(stmt, 4, reason.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, now.c_str(), -1, SQLITE_TRANSIENT);
    std::string expStr = std::to_string(expiresAt);
    sqlite3_bind_text(stmt, 7, expStr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return id;
}

// Sum of amounts committed within the current rolling period. COMPLETED/EXECUTING/
// APPROVED are the states that have spent (or are committed to spend) real tokens;
// CREATED/HELD/NOTIFIED are still gated and TX_FAILED/REJECTED/EXPIRED never moved
// funds. TX_UNKNOWN (a crash-stranded EXECUTING reconciled by reconcileExecutingSpends,
// L7) is ALSO counted: its funds MAY have moved, so we conservatively keep it against the
// budget rather than free money that may already be spent (no automatic retirement —
// PM3-F3, see §3.13). Shared by walletSend and the inbound A2A auto-approve gate.
int64_t PilotImpl::periodSpent() {
    if (!db_) return 0;
    int64_t periodStart = std::stoll(currentTimestamp()) - spendPeriodSeconds_;
    std::string psStr = std::to_string(periodStart);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT COALESCE(SUM(amount), 0) FROM spend_requests "
        "WHERE state IN ('COMPLETED', 'EXECUTING', 'APPROVED', 'TX_UNKNOWN') AND created_at > ?;",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, psStr.c_str(), -1, SQLITE_TRANSIENT);
    int64_t total = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        total = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return total;
}

// Run the real on-chain transfer for a spend request that has cleared its gate
// (owner-approved via approveSpend, or auto-approved below threshold by the A2A
// inbox). EXECUTING -> private transfer -> COMPLETED/TX_FAILED. Returns true iff the
// transfer succeeded. No owner notification, no inbound-task wiring — callers own
// those. Without a wallet the request is honestly marked TX_FAILED (no fake success).
bool PilotImpl::executeSpend(const std::string& requestId) {
    if (!db_) return false;

    std::string recipient, state;
    int64_t amount = 0;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT recipient, amount, state FROM spend_requests WHERE id = ?;", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, requestId.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); return false; }
    recipient = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    amount = sqlite3_column_int64(stmt, 1);
    state = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    sqlite3_finalize(stmt);

    // Idempotency: never re-run a request that already reached (or is mid-) execution.
    if (state == "COMPLETED") return true;
    if (state == "EXECUTING" || state == "TX_FAILED" ||
        state == "REJECTED" || state == "EXPIRED" || state == "TX_UNKNOWN")
        return false;

    auto setSpendState = [&](const char* st) {
        std::string now = currentTimestamp();
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db_,
            "UPDATE spend_requests SET state = ?, updated_at = ? WHERE id = ?;", -1, &s, nullptr);
        sqlite3_bind_text(s, 1, st, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, now.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, requestId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(s);
        sqlite3_finalize(s);
    };

    setSpendState("EXECUTING");

    if (!isContextReady()) { setSpendState("TX_FAILED"); return false; }

    // transfer_private returns once the SEQUENCER ACCEPTS THE TX INTO ITS MEMPOOL — not once it
    // executes. wallet-ffi sets success:true on send_transaction() alone (wallet/src/lib.rs,
    // send_privacy_preserving_tx_with_pre_check), so "COMPLETED" below means submitted, not
    // settled: a tx later rejected at block production still reads COMPLETED here, and even a
    // good tx is only in a block up to one BLOCK_TIME later — a balance read straight after
    // this row turns COMPLETED is a read from inside that interval (measured 2026-08-26: row
    // COMPLETED 17:19:46Z, block with the tx 17:20:02Z, balance 99 then 94).
    // The one rejection this path has actually produced — InvalidInput("Nullifier already
    // seen") for pay-by-keys to a recipient with on-chain history, measured 2026-08-01 — was a
    // LEZ bug fixed upstream in #268; the pinned lez_core rev carries the fix, and pay-by-keys
    // to a recipient WITH history measured 98->96 on 2026-08-16 and 100->94 (1 + 5 LEZ, two
    // txs, each in the next block) in the two-agent run of 2026-08-26.
    // Confirming execution from here would need a working tx query (the sequencer's
    // getTransaction answers null for every hash, real or bogus), so COMPLETED is the strongest
    // claim this module can currently make.
    // The hash is unknowable until the call returns — there is no pre-broadcast hash to persist,
    // and a crash strictly before this write leaves the spend stranded in EXECUTING with NO hash
    // (reconcileExecutingSpends surfaces those). Persist tx_hash atomically with the terminal state.
    // Catch the wallet up to chain head BEFORE spending. walletBalance() has always synced
    // before answering; this path never did, so the balance an owner is shown and the notes a
    // transfer can actually spend were answers from two different views of the chain — which is
    // exactly the shape of "reports 100, then fails with InsufficientFundsError". A spend must
    // not be built on a staler picture than the number that justified it.
    {
        logos::CallError herr;
        int64_t head = modules().lez_core.get_current_block_height(&herr);
        if (herr.code.empty())
            modules().lez_core.sync_to_block(head, nullptr, kWalletSyncTimeoutMs);
    }

    std::string result = doPrivateTransfer(modules().lez_core, agentAccountId_, recipient, amount);
    bool ok = transferSucceeded(result);
    std::string txHash = transferTxHash(result);
    std::string now = currentTimestamp();
    sqlite3_stmt* term = nullptr;
    sqlite3_prepare_v2(db_,
        "UPDATE spend_requests SET state=?, tx_hash=?, updated_at=? WHERE id=?;", -1, &term, nullptr);
    sqlite3_bind_text(term, 1, ok ? "COMPLETED" : "TX_FAILED", -1, SQLITE_STATIC);
    sqlite3_bind_text(term, 2, txHash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(term, 3, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(term, 4, requestId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(term);
    sqlite3_finalize(term);
    return ok;
}

bool PilotImpl::approveSpend(const std::string& requestId) {
    // executeSpend (the real transfer) and sendToOwner both tolerate a missing wallet/transport
    // (honest TX_FAILED / no-op), so the approval FSM + outbound-task advancement still run when
    // the module context isn't wired. Gate only on the DB, so an unavailable transport never silently
    // swallows an owner approval (and the path stays unit-testable).
    if (!db_) return false;

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT recipient, amount, state, expires_at FROM spend_requests WHERE id = ?;",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, requestId.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return false;
    }

    std::string recipient = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    int64_t amount = sqlite3_column_int64(stmt, 1);
    std::string state = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    std::string expiresAt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    sqlite3_finalize(stmt);

    if (state != "HELD" && state != "NOTIFIED") return false;

    int64_t nowEpoch = std::stoll(currentTimestamp());
    if (!expiresAt.empty() && nowEpoch > std::stoll(expiresAt)) {
        std::string ts = std::to_string(nowEpoch);
        sqlite3_prepare_v2(db_,
            "UPDATE spend_requests SET state = 'EXPIRED', updated_at = ? WHERE id = ?;",
            -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, requestId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        sendToOwner("Spend request " + requestId + " has expired.");
        return false;
    }

    std::string now = currentTimestamp();
    sqlite3_prepare_v2(db_,
        "UPDATE spend_requests SET state = 'APPROVED', updated_at = ? WHERE id = ?;",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, requestId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // APPROVED -> EXECUTING -> COMPLETED/TX_FAILED, via the shared executor (also used
    // by the A2A inbox auto-approve path) so there is one transfer code path.
    bool ok = executeSpend(requestId);

    if (ok) {
        sendToOwner("Transaction " + requestId + " completed: " + std::to_string(amount) + " LEZ sent to " + recipient);
    } else {
        sendToOwner("Transaction " + requestId + " FAILED");
    }

    // If this spend backs an inbound peer task, drive that task to its terminal state.
    resumeInboundTask(requestId, ok, ok ? "owner approved; executed" : "owner approved; execution failed");
    // If this spend backs an OUTBOUND A2A payment held for approval, advance that task too
    // (M6): 'paid' on a real transfer, 'pay-failed' otherwise — never orphan it.
    advanceLinkedOutboundTask(db_, requestId, ok ? "paid" : "pay-failed");
    return ok;
}

bool PilotImpl::rejectSpend(const std::string& requestId) {
    if (!db_) return false;

    std::string now = currentTimestamp();
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "UPDATE spend_requests SET state = 'REJECTED', updated_at = ? WHERE id = ? AND state IN ('HELD', 'NOTIFIED');",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, requestId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    bool changed = sqlite3_changes(db_) > 0;
    if (changed) {
        sendToOwner("Transaction " + requestId + " rejected.");
        resumeInboundTask(requestId, false, "owner rejected");
        // An outbound A2A payment the owner rejected never moves money: 'pay-failed' (M6).
        advanceLinkedOutboundTask(db_, requestId, "pay-failed");
    }
    return changed;
}

// Quietly release a HELD/NOTIFIED spend (e.g. a peer withdrew its task via tasks/cancel) so a
// later owner /approve can no longer move funds for it (L3). Unlike rejectSpend this does NOT
// notify the owner (a "Transaction ... rejected." message for a peer-withdrawn task would be
// misleading peer-driven spam, P7) and does NOT resumeInboundTask (the task is already canceled).
bool PilotImpl::releaseHeldSpend(const std::string& requestId) {
    if (!db_) return false;
    std::string now = currentTimestamp();
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "UPDATE spend_requests SET state = 'REJECTED', updated_at = ? "
        "WHERE id = ? AND state IN ('HELD', 'NOTIFIED');", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, requestId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return sqlite3_changes(db_) > 0;
}

// Proactively cancel any pending request whose 60-minute approval window has
// passed. Without this the deadline shown to the owner ("Expires: 60 min") is
// only enforced lazily at approve-time — a stale request would otherwise sit in
// the pending list forever and be re-announced on restart. Returns the count.
// SQL verified against real SQLite in /tmp/test_expiry.cpp (red->green).
int PilotImpl::expireStaleSpends() {
    if (!db_) return 0;
    long long now = std::stoll(currentTimestamp());

    // Only states still awaiting the owner can expire; APPROVED/EXECUTING/etc. are
    // mid-flight and COMPLETED/REJECTED are terminal.
    static const char* PENDING =
        "state IN ('CREATED','HELD','NOTIFIED') "
        "AND expires_at != '' AND CAST(expires_at AS INTEGER) < ?";

    std::vector<std::string> ids;
    std::string selSql = std::string("SELECT id FROM spend_requests WHERE ") + PENDING + ";";
    sqlite3_stmt* sel = nullptr;
    sqlite3_prepare_v2(db_, selSql.c_str(), -1, &sel, nullptr);
    sqlite3_bind_int64(sel, 1, now);
    while (sqlite3_step(sel) == SQLITE_ROW)
        ids.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(sel, 0)));
    sqlite3_finalize(sel);
    if (ids.empty()) return 0;

    std::string updSql =
        std::string("UPDATE spend_requests SET state='EXPIRED', updated_at=? WHERE ") + PENDING + ";";
    sqlite3_stmt* upd = nullptr;
    sqlite3_prepare_v2(db_, updSql.c_str(), -1, &upd, nullptr);
    std::string nowStr = std::to_string(now);
    sqlite3_bind_text(upd, 1, nowStr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(upd, 2, now);
    sqlite3_step(upd);
    sqlite3_finalize(upd);

    // Tell the owner once per cancelled request (best-effort; no-op if no channel),
    // and drive any linked inbound A2A task to failed.
    for (const auto& id : ids) {
        sendToOwner("Spend request " + id + " expired before approval and was cancelled.");
        resumeInboundTask(id, false, "approval expired");
        // An ABOVE-THRESHOLD outbound A2A payment parks its task in 'awaiting-approval' behind
        // a HELD spend. If that spend EXPIRES unapproved the money never moves, so the linked
        // outbound task must also reach a terminal state — exactly as approveSpend/rejectSpend
        // drive it. Without this it orphans in 'awaiting-approval' forever (M6).
        advanceLinkedOutboundTask(db_, id, "pay-failed");
    }
    return static_cast<int>(ids.size());
}

std::string PilotImpl::getPendingSpends() {
    if (!db_) return "{\"error\": \"not initialized\"}";

    expireStaleSpends();   // never present a request that has already timed out

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT id, recipient, amount, reason, state, created_at, expires_at FROM spend_requests "
        "WHERE state IN ('CREATED', 'HELD', 'NOTIFIED') ORDER BY created_at;",
        -1, &stmt, nullptr);

    QJsonArray arr;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        QJsonObject obj;
        obj["id"] = QString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
        obj["recipient"] = QString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
        obj["amount"] = static_cast<double>(sqlite3_column_int64(stmt, 2));
        obj["reason"] = QString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)));
        obj["state"] = QString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)));
        obj["created_at"] = QString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5)));
        obj["expires_at"] = QString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6)));
        arr.append(obj);
    }
    sqlite3_finalize(stmt);

    QJsonObject root;
    root["pending"] = arr;
    return QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString();
}

bool PilotImpl::setSpendingLimits(int64_t perTransaction, int64_t perPeriod, int64_t periodSeconds) {
    spendLimitPerTx_ = perTransaction;
    spendLimitPerPeriod_ = perPeriod;
    spendPeriodSeconds_ = periodSeconds;

    if (!db_) return false;

    auto upsert = [&](const char* key, int64_t val) {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_,
            "INSERT OR REPLACE INTO config (key, value) VALUES (?, ?);",
            -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
        std::string v = std::to_string(val);
        sqlite3_bind_text(stmt, 2, v.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    };

    upsert("spend_limit_per_tx", perTransaction);
    upsert("spend_limit_per_period", perPeriod);
    upsert("spend_period_seconds", periodSeconds);
    return true;
}

// Drive a freshly-created (CREATED) spend request into the owner-approval flow and
// report honestly. HELD first, then notify; only advance to NOTIFIED if the owner
// notification actually delivered. If it failed, the request stays HELD (so it is
// re-announced on recovery and never silently lost) and we flag notification_failed
// instead of claiming the owner was prompted. approveSpend accepts HELD or NOTIFIED,
// so either way the request remains approvable — fixing the per-period path that used
// to leave requests stuck in CREATED (unapprovable).
std::string PilotImpl::holdForApproval(const std::string& reqId, const std::string& ownerMsg,
                                       const std::string& heldMessage) {
    auto setState = [&](const char* st) {
        if (!db_) return;
        std::string now = currentTimestamp();
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db_,
            "UPDATE spend_requests SET state = ?, updated_at = ? WHERE id = ?;", -1, &s, nullptr);
        sqlite3_bind_text(s, 1, st, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, now.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, reqId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(s);
        sqlite3_finalize(s);
    };

    setState("HELD");
    bool notified = sendToOwner(ownerMsg);
    if (notified) setState("NOTIFIED");   // else stay HELD: recovery will re-announce it

    QJsonObject res;
    res["status"] = QString("held");
    res["request_id"] = QString::fromStdString(reqId);
    res["message"] = QString::fromStdString(heldMessage);
    if (!notified) res["notification_failed"] = true;
    return QJsonDocument(res).toJson(QJsonDocument::Compact).toStdString();
}

std::string PilotImpl::walletSend(const std::string& recipient, int64_t amount, const std::string& reason) {
    if (!isContextReady() || agentAccountId_.empty())
        return "{\"error\": \"not initialized\"}";

    // Check per-period spending limit
    if (db_) {
        int64_t periodTotal = periodSpent();

        if (periodTotal + amount > spendLimitPerPeriod_) {
            std::string reqId = createSpendRequest(recipient, amount, reason);
            return holdForApproval(reqId,
                "Period limit exceeded (" + std::to_string(periodTotal) + "/" +
                std::to_string(spendLimitPerPeriod_) + " LEZ). Approval required.\n/approve " +
                reqId + "\n/reject " + reqId,
                "Period spending limit exceeded");
        }
    }

    if (amount > spendLimitPerTx_) {
        std::string reqId = createSpendRequest(recipient, amount, reason);
        std::string msg = "Approval needed:\nAmount: " + std::to_string(amount) +
            " LEZ\nTo: " + recipient + "\nReason: " + reason +
            "\nExpires: 60 min\n/approve " + reqId + "\n/reject " + reqId;
        return holdForApproval(reqId, msg, "Awaiting owner approval");
    }

    // Below both limits -> execute autonomously through the shared executor
    // (CREATED -> EXECUTING -> COMPLETED/TX_FAILED). Same transfer path as the
    // owner-approved (approveSpend) and A2A auto-approve flows.
    std::string reqId = createSpendRequest(recipient, amount, reason);
    bool ok = executeSpend(reqId);

    QJsonObject res;
    res["status"] = ok ? QString("completed") : QString("failed");
    res["request_id"] = QString::fromStdString(reqId);
    return QJsonDocument(res).toJson(QJsonDocument::Compact).toStdString();
}

// L7 — a clean run always drives EXECUTING->terminal synchronously, so any spend still EXECUTING
// at startup was crash-stranded. The wallet exposes NO status-by-hash query and a stranded
// EXECUTING never captured a hash, so we cannot auto-resolve COMPLETED/TX_FAILED: move it to the
// terminal TX_UNKNOWN (still budget-counted by periodSpent), un-hang its linked outbound
// ('settling'->'pay-unresolved') and inbound ('input-required'->failed) tasks, and surface ONCE
// to the owner so a human can verify on chain. No-op without db_. SCOPE: resolves ONLY EXECUTING;
// a row stranded 'settling' against a CREATED spend (crash after settle-COMMIT, before
// executeSpend) is a separate known residual (see §3.13 follow-up).
void PilotImpl::reconcileExecutingSpends() {
    if (!db_) return;
    struct Row { std::string id, recipient, txHash; int64_t amount; };
    std::vector<Row> rows;
    sqlite3_stmt* sel = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT id, recipient, amount, tx_hash FROM spend_requests WHERE state='EXECUTING';",
        -1, &sel, nullptr);
    while (sqlite3_step(sel) == SQLITE_ROW) {
        Row r;
        if (sqlite3_column_text(sel, 0)) r.id = reinterpret_cast<const char*>(sqlite3_column_text(sel, 0));
        if (sqlite3_column_text(sel, 1)) r.recipient = reinterpret_cast<const char*>(sqlite3_column_text(sel, 1));
        r.amount = sqlite3_column_int64(sel, 2);
        if (sqlite3_column_text(sel, 3)) r.txHash = reinterpret_cast<const char*>(sqlite3_column_text(sel, 3));
        if (!r.id.empty()) rows.push_back(r);
    }
    sqlite3_finalize(sel);

    for (const auto& r : rows) {
        std::string now = currentTimestamp();
        sqlite3_stmt* u = nullptr;
        sqlite3_prepare_v2(db_,
            "UPDATE spend_requests SET state='TX_UNKNOWN', updated_at=? WHERE id=? AND state='EXECUTING';",
            -1, &u, nullptr);
        sqlite3_bind_text(u, 1, now.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(u, 2, r.id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(u);
        sqlite3_finalize(u);
        if (sqlite3_changes(db_) == 0) continue;   // never double-surface

        advanceLinkedOutboundTask(db_, r.id, "pay-unresolved");
        resumeInboundTask(r.id, false,
            "interrupted mid-execution before restart; funds may or may not have moved - verify on chain");
        std::string msg = "Spend " + r.id + " was interrupted mid-execution (amount " +
            std::to_string(r.amount) + " LEZ to " + r.recipient + ").";
        if (!r.txHash.empty()) msg += " tx_hash: " + r.txHash + ".";
        msg += " The wallet exposes no transaction-status query, so the outcome cannot be "
               "auto-reconciled — the funds MAY or MAY NOT have moved. The amount stays counted "
               "against the budget until you confirm on chain.";
        sendToOwner(msg);
    }
}

void PilotImpl::recoverPendingTransactions() {
    if (!db_ || !isContextReady()) return;

    expireStaleSpends();   // don't re-announce requests that already timed out while we were down

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT id, state, recipient, amount FROM spend_requests WHERE state NOT IN "
        "('COMPLETED', 'REJECTED', 'TX_FAILED', 'EXPIRED', 'TX_UNKNOWN');",
        -1, &stmt, nullptr);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        std::string state = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));

        if (state == "HELD" || state == "NOTIFIED") {
            int64_t amount = sqlite3_column_int64(stmt, 3);
            std::string recipient = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            sendToOwner("Pending approval (recovered):\nAmount: " + std::to_string(amount) +
                " LEZ\nTo: " + recipient + "\n/approve " + id + "\n/reject " + id);
        }
    }
    sqlite3_finalize(stmt);
}
