#include "pilot_impl.h"
#include "pilot_a2a.h"
#include "pilot_crypto.h"
#include "logos_api.h"
#include "logos_api_client.h"
#include "logos_mode.h"
#include <sqlite3.h>
#include <sstream>
#include <chrono>
#include <random>
#include <vector>

#include <QString>
#include <QVariant>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QCryptographicHash>

static const Timeout RPC_TIMEOUT(15000);

// M3 — LRU cap on the heavy discovered_agents card_json cache (see a2aEvictDiscoveryCache).
static const int kA2ADiscoveredAgentsMax = 1000;

static std::string extractEncryptionKey(const std::string& addr) {
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(addr));
    if (doc.isObject() && doc.object().contains("viewing_public_key"))
        return doc.object()["viewing_public_key"].toString().toStdString();
    return addr;
}

// HONEST SUCCESS CONTRACT for an auto-serviced inbound A2A skill result (see pilot_a2a.h).
// A task is 'completed' ONLY on an EXPLICIT, positive success signal — we NEVER pay for
// unproven or failed work. Previously the dispatcher marked any object without an "error" key
// (and any non-empty string) as success, so {"joined":false}/{"success":false} or an opaque
// string was paid as 'completed'. This closes that hole: ambiguity -> failed, never completed.
bool a2aResultIsSuccess(const std::string& result) {
    QJsonDocument rd = QJsonDocument::fromJson(QByteArray::fromStdString(result));
    if (rd.isObject()) {
        QJsonObject ro = rd.object();
        const QString status = ro.value("status").toString();
        bool negative =
            ro.contains("error") ||
            (ro.contains("success") && !ro.value("success").toBool(true)) ||
            (ro.contains("joined")  && !ro.value("joined").toBool(true))  ||
            (ro.contains("ok")      && !ro.value("ok").toBool(true))      ||
            status == QStringLiteral("failed") ||
            status == QStringLiteral("error");
        return !negative && !ro.isEmpty();
    }
    if (rd.isArray())
        return !rd.array().isEmpty();
    return false;   // bare string / scalar / empty / unparseable -> opaque -> failed (no pay)
}

// Single source of truth for the SAFE A2A asker-pays-doer services (FIX 4a + FIX 2).
// agentCard() builds _logos.pricing from this, and processInboundRequest() decides which
// inbound skills it AUTO-SERVICES for a stranger from this, so an advertised-as-autonomous
// price can never name a skill we don't auto-service and an auto-serviced skill can never be
// silently free.
//
// SAFE-ONLY (FIX 2): every entry is pure compute with no side effects on this agent — no
// local-file access, no use of our messaging identity, no fund movement — so it is safe to
// run for an UNKNOWN peer with NO owner involvement. The RISKY families (storage-*,
// messaging-*, wallet-send, program-*) are deliberately ABSENT: an A2A peer requesting one is
// routed to the OWNER GATE by processInboundRequest (a2aRiskyOwnerGated), so it is neither
// priced-as-autonomous nor auto-serviced. program-call/program-deploy are also unsupported
// over A2A. A price of 0 would mean "serviced but explicitly free" (none today).
const std::vector<A2AService>& a2aServiceCatalog() {
    static const std::vector<A2AService> kCatalog = {
        {"agent-ask",                5},   // SAFE: LLM Q&A, pure compute (no files/msg/funds)
    };
    return kCatalog;
}

static std::string genUuid() {
    std::random_device rd;
    std::mt19937_64 rng(rd());
    std::ostringstream ss;
    ss << std::hex << rng() << "-" << rng();
    return ss.str();
}

static std::string nowTimestamp() {
    auto now = std::chrono::system_clock::now();
    return std::to_string(std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());
}

// Flatten a peer-controlled string before it goes into an owner prompt (L4/shared): strip CR/LF
// so a malicious npk/skill/recipient/reason can't inject extra lines (e.g. a fake "/approve").
std::string a2aFlattenForPrompt(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out += (c == '\n' || c == '\r') ? ' ' : c;
    return out;
}

// Sign an OUTBOUND A2A request envelope (H2). Same ES256K-over-canonical-bytes scheme as
// replyToPeer / the Agent Card: publish our ECIES public key as _logos.signing_key, drop any
// prior _logos.signature, sign the compact bytes (signing_key present, signature absent), then
// re-attach _logos.signature. verifyInboundRequest on the doer reproduces these exact bytes. We
// NEVER fabricate a signature: with no key (or a signing failure) the request goes out unsigned
// and a hardened doer will (correctly) drop it.
std::string signA2AEnvelope(QJsonObject env, const std::string& eciesPub, const std::string& eciesPriv) {
    if (eciesPub.empty() || eciesPriv.empty())
        return QJsonDocument(env).toJson(QJsonDocument::Compact).toStdString();   // unsigned (no key)
    QJsonObject logos = env["_logos"].toObject();
    logos["signing_key"] = QString::fromStdString(eciesPub);
    logos.remove("signature");                                                   // canonical excludes the sig
    env["_logos"] = logos;
    std::string canonical = QJsonDocument(env).toJson(QJsonDocument::Compact).toStdString();
    std::vector<uint8_t> bytes(canonical.begin(), canonical.end());
    try {
        logos["signature"] = QString::fromStdString(signMessage(bytes, eciesPriv));
        env["_logos"] = logos;
    } catch (const std::exception&) {
        // Honest failure: leave the request unsigned rather than fabricate a signature.
    }
    return QJsonDocument(env).toJson(QJsonDocument::Compact).toStdString();
}

// Returns "valid" / "invalid" / "unsigned" / "unbound" for a received Agent Card.
// This checks AUTHENTICITY, not just integrity (M3). A card is "valid" only when:
//   1. it carries a signature, AND
//   2. it publishes its own identity key (_logos.signing_key — the key the card claims
//      as its signer, bound alongside its _logos.npk identity and encryption material),
//      AND
//   3. signature.publicKey == that published identity key (the signature was produced
//      by the card's OWN identity key, not some other key), AND
//   4. the signature verifies over the canonical card bytes (card minus "signature")
//      using that BOUND identity key.
// Rejecting on (3) is what defeats impersonation: an attacker who re-signs a genuine
// card under their own key (and sets signature.publicKey to it) leaves the card's
// published signing_key unchanged, so signature.publicKey != signing_key -> "invalid".
// A signed card that publishes no identity key cannot be bound -> "unbound" (not valid).
// Legacy/unsigned cards are flagged "unsigned" for interop, never "valid".
// NOTE: this single-arg form is authenticity relative ONLY to the card's self-declared
// identity key, so a from-scratch forgery (victim npk, attacker signing_key+payout) still
// reads "valid" here. The DB-aware overload below BINDS the verdict to a first-contact TOFU
// pin (npk -> signing_key) so such a swap is rejected; payout-bearing callers use that one.
// External linkage (not static) so the unit tests can drive both directly.
QString verifyCardStatus(const QJsonObject& card) {
    if (!card.contains("signature")) return QStringLiteral("unsigned");
    QJsonObject sig = card["signature"].toObject();
    QString sigPub = sig["publicKey"].toString();
    QString val = sig["value"].toString();
    if (sigPub.isEmpty() || val.isEmpty()) return QStringLiteral("invalid");

    // The card's bound identity key — the public key it publishes as its own signer.
    // Without it there is nothing to authenticate against, so the card is "unbound".
    QString identityKey = card["_logos"].toObject()["signing_key"].toString();
    if (identityKey.isEmpty()) return QStringLiteral("unbound");

    // Authenticity gate: the signature MUST come from the card's published identity key.
    // A different signer (impersonation) is rejected before we even verify the bytes.
    if (sigPub != identityKey) return QStringLiteral("invalid");

    QJsonObject unsignedCard = card;
    unsignedCard.remove("signature");
    std::string canonical = QJsonDocument(unsignedCard).toJson(QJsonDocument::Compact).toStdString();
    std::vector<uint8_t> bytes(canonical.begin(), canonical.end());
    // Verify with the BOUND identity key, never the attacker-supplied signature.publicKey.
    return verifySignature(bytes, val.toStdString(), identityKey.toStdString())
        ? QStringLiteral("valid") : QStringLiteral("invalid");
}

// Identity-BOUND card status (M3 + TOFU). Runs the self-consistency check above, then —
// only for a card it already deems 'valid' — binds that verdict to a FIRST-CONTACT pin of
// (_logos.npk -> _logos.signing_key) in pinned_identities. The first signing_key ever seen
// for an npk is recorded permanently; any later card presenting that SAME npk under a
// DIFFERENT signing_key is rejected as 'invalid', even when it is internally self-consistent
// (the attacker signs with its own key and sets signature.publicKey to it). This is what
// defeats a from-scratch forgery that reuses a victim's npk with the attacker's own
// signing_key + payout: the pure check alone reads 'valid' for it (signature matches its
// own key), but the pin from the genuine first contact does not, so the swap is refused.
// Pinning is INSERT OR IGNORE, so the genuine first contact wins and cannot be overwritten.
QString verifyCardStatus(const QJsonObject& card, sqlite3* db) {
    QString base = verifyCardStatus(card);
    if (!db || base != QStringLiteral("valid")) return base;

    QJsonObject logos = card["_logos"].toObject();
    std::string npk = logos["npk"].toString().toStdString();
    std::string key = logos["signing_key"].toString().toStdString();
    // Nothing to bind on (no payment identity / no signer) -> keep the base verdict.
    if (npk.empty() || key.empty()) return base;

    // Defensive create so a pre-migration DB (or a bare unit-test DB) still pins. A no-op
    // on the already-created table, so it never bumps the schema mid-iteration in callers.
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS pinned_identities ("
        "npk TEXT PRIMARY KEY, signing_key TEXT NOT NULL, first_seen TEXT NOT NULL);",
        nullptr, nullptr, nullptr);

    // First contact wins: INSERT OR IGNORE pins THIS key only when the npk is not yet
    // pinned. Doing the write before the read closes any check-then-pin window.
    std::string ts = nowTimestamp();
    sqlite3_stmt* ins = nullptr;
    if (sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO pinned_identities (npk, signing_key, first_seen) VALUES (?, ?, ?);",
            -1, &ins, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(ins, 1, npk.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 2, key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 3, ts.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(ins);
    }
    sqlite3_finalize(ins);

    // Read the AUTHORITATIVE pinned key for this npk and require the card to match it.
    std::string pinned;
    sqlite3_stmt* sel = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT signing_key FROM pinned_identities WHERE npk = ?;", -1, &sel, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(sel, 1, npk.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(sel) == SQLITE_ROW && sqlite3_column_text(sel, 0))
            pinned = reinterpret_cast<const char*>(sqlite3_column_text(sel, 0));
    }
    sqlite3_finalize(sel);

    // Bound to IDENTITY, not just signer-key consistency: a signing_key that differs from
    // the npk's first-seen key is a payout-swap impersonation -> 'invalid'.
    return (!pinned.empty() && pinned == key) ? QStringLiteral("valid") : QStringLiteral("invalid");
}

std::string PilotImpl::buildCard() {
    if (agentNpk_.empty()) return "{\"error\": \"not initialized\"}";

    // A2A messaging identity is the ECIES/ECDH key (the one we HOLD the private half of and can
    // decrypt with). L1 key separation: the inbox we advertise — and that requesters encrypt+
    // address tasks to — is keyed on the dedicated ENCRYPTION key (a2aSelfEncKey(), advertised as
    // _logos.enc_key), NOT the SIGNING key and NOT the wallet npk. The signing key stays published
    // as _logos.signing_key for TOFU/authenticity; a pre-split agent (no enc key yet) falls back
    // to its signing ECIES key here. Payment identity (_logos.npk / _logos.payout) stays the npk.
    std::string encKey = a2aSelfEncKey();
    std::string inbox = "/pilot/1/inbox-" + encKey + "/proto";

    QJsonObject card;
    card["name"] = QString("Pilot Agent");
    card["description"] = QString("Sovereign AI agent on LEZ with wallet, storage, and messaging");
    card["url"] = QString::fromStdString("waku:" + inbox);
    card["version"] = QString("1.0.0");
    card["documentationUrl"] = QString("https://github.com/johnniedom/pilot");

    QJsonObject capabilities;
    capabilities["streaming"] = true;
    capabilities["pushNotifications"] = true;
    capabilities["stateTransitionHistory"] = true;
    card["capabilities"] = capabilities;

    QJsonArray defaultModes;
    defaultModes.append(QString("application/json"));
    card["defaultInputModes"] = defaultModes;
    card["defaultOutputModes"] = defaultModes;

    QJsonArray jsonMode;
    jsonMode.append(QString("application/json"));
    QJsonArray jsonOctetIn;
    jsonOctetIn.append(QString("application/json"));
    jsonOctetIn.append(QString("application/octet-stream"));
    QJsonArray jsonOctetOut;
    jsonOctetOut.append(QString("application/json"));
    jsonOctetOut.append(QString("application/octet-stream"));
    QJsonArray textJsonIn;
    textJsonIn.append(QString("application/json"));
    textJsonIn.append(QString("text/plain"));

    // A skill is advertised as autonomous IFF it is in the SAFE service catalog (the single
    // source of truth). Everything else this agent can do touches local files, its messaging
    // identity, or funds, so over A2A it is OWNER-GATED — advertised honestly as
    // "owner-approval" and never priced as autonomous. The access tag is derived from the
    // catalog so the card can never claim a skill is autonomous that the inbound dispatch
    // would actually owner-gate (or vice versa).
    auto isSafeAutonomous = [](const char* id) {
        for (const auto& svc : a2aServiceCatalog())
            if (QString::fromUtf8(svc.id) == QString::fromUtf8(id)) return true;
        return false;
    };
    auto mkSkill = [&](const char* id, const char* name, const char* desc,
                       const QJsonArray& in, const QJsonArray& out) {
        QJsonObject s;
        s["id"] = QString(id);
        s["name"] = QString(name);
        s["description"] = QString(desc);
        s["inputModes"] = in;
        s["outputModes"] = out;
        s["x_access"] = isSafeAutonomous(id) ? QString("autonomous") : QString("owner-approval");
        return s;
    };

    QJsonArray skills;
    // SAFE, autonomous, priced (the autonomous-pay demo exercises this one).
    skills.append(mkSkill("agent-ask", "Agent Ask",
        "Answers a prompt with the agent's LLM (pure compute; no files, messaging, or funds)",
        textJsonIn, jsonMode));
    // RISKY family — advertised honestly as owner-gated (x_access=owner-approval), never
    // auto-run for a stranger and never priced as autonomous.
    skills.append(mkSkill("wallet-balance", "Wallet Balance",
        "Returns the agent's current shielded token balance (owner-gated)", jsonMode, jsonMode));
    skills.append(mkSkill("wallet-send", "Wallet Send",
        "Sends LEZ tokens to a recipient (owner-gated; spends the agent's funds)", jsonMode, jsonMode));
    skills.append(mkSkill("storage-upload", "Storage Upload",
        "Encrypts and uploads a file to Logos Storage (owner-gated; reads a local path)", jsonOctetIn, jsonMode));
    skills.append(mkSkill("storage-download", "Storage Download",
        "Retrieves and decrypts a file from Logos Storage (owner-gated)", jsonMode, jsonOctetOut));
    skills.append(mkSkill("storage-share", "Storage Share",
        "Shares access to a stored file with another Logos identity (owner-gated)", jsonMode, jsonMode));
    skills.append(mkSkill("storage-list", "Storage List",
        "Lists the files this agent has stored (owner-gated; reveals local inventory)", jsonMode, jsonMode));
    skills.append(mkSkill("messaging-send", "Messaging Send",
        "Sends an encrypted message as this agent (owner-gated; uses our messaging identity)", textJsonIn, jsonMode));
    skills.append(mkSkill("messaging-join", "Messaging Join",
        "Joins a Logos messaging group as this agent (owner-gated)", jsonMode, jsonMode));
    skills.append(mkSkill("messaging-create_group", "Messaging Create Group",
        "Creates a Logos messaging group as this agent (owner-gated)", jsonMode, jsonMode));
    skills.append(mkSkill("program-query", "Program Query",
        "Reads state from a LEZ program as this agent (owner-gated)", jsonMode, jsonMode));
    // program-call / program-deploy are NOT advertised: they are unsupported over A2A
    // (the inbound dispatch returns 'unsupported skill'), so advertising/pricing them would
    // be dishonest. They are absent from a2aServiceCatalog() for the same reason.
    card["skills"] = skills;

    QJsonObject auth;
    QJsonArray schemes;
    schemes.append(QString("ecies"));
    auth["schemes"] = schemes;
    auth["credentials"] = QString::fromStdString("npk:" + agentNpk_);
    card["authentication"] = auth;

    QJsonObject logos;
    logos["npk"] = QString::fromStdString(agentNpk_);
    logos["inbox_topic"] = QString::fromStdString(inbox);
    logos["transport"] = QString("waku");

    // Pricing is derived from the SINGLE-SOURCE service catalog (FIX 4a), so what we charge
    // for is exactly what we service inbound — never a price for an unsupported skill, never
    // a serviced skill silently free. Only priced (>0) entries are advertised; a 0-price
    // entry would be "serviced but explicitly free" and is intentionally omitted here.
    QJsonObject pricing;
    for (const auto& svc : a2aServiceCatalog())
        if (svc.price > 0)
            pricing[QString::fromUtf8(svc.id)] = static_cast<double>(svc.price);
    logos["pricing"] = pricing;

    logos["payment"] = QString("lez");
    logos["payment_timing"] = QString("on-acceptance");

    // Payout account (M5): the shielded LEZ recipient a requester transfers the price
    // to. This is the agent's private-account PUBLIC KEYS blob (the same
    // {nullifier_public_key, viewing_public_key} JSON get_private_account_keys returns).
    // A requester pays it via doPrivateTransfer, which sees those key fields and routes
    // to transfer_private (external payee) — so this value round-trips straight through
    // the payment path. It is deliberately NOT the waku messaging id (paying that would
    // mis-target / TX_FAIL), and NOT a bare owned account id (a bare id can only be paid
    // by its OWNER via transfer_private_owned, never by an external requester).
    logos["payout"] = QString::fromStdString(agentNpk_);

    // Signing key (M3): the public key that signs THIS card. verifyCardStatus binds the
    // signature to this published identity key, so a card re-signed under a different
    // key (impersonation) is rejected even if its signature is internally valid. Kept EXACTLY
    // as agentEciesPub_ so every peer's TOFU pin (npk -> signing_key) survives L1 unchanged.
    logos["signing_key"] = QString::fromStdString(agentEciesPub_);
    // L1: the INDEPENDENT encryption/routing key peers encrypt A2A traffic to (== the inbox we
    // subscribe). A pre-split peer reading this card has no enc_key and routes to signing_key
    // instead (a2aRoutingKeyFor falls back). Signed over below, so it is authenticated.
    logos["enc_key"] = QString::fromStdString(encKey);
    card["_logos"] = logos;

    // Sign the canonical card bytes (the card WITHOUT the signature field) so a
    // recipient can verify authenticity (spec.md:63). ECDSA/secp256k1 over the
    // agent's existing ECIES key material. QJsonObject serializes keys in a
    // deterministic (sorted) order, so the recipient reproduces these exact bytes
    // by removing "signature" and re-serializing compact.
    if (!agentEciesPriv_.empty()) {
        std::string canonical = QJsonDocument(card).toJson(QJsonDocument::Compact).toStdString();
        try {
            std::vector<uint8_t> canonicalBytes(canonical.begin(), canonical.end());
            std::string sigHex = signMessage(canonicalBytes, agentEciesPriv_);
            QJsonObject signature;
            signature["alg"] = QString("ES256K");          // ECDSA secp256k1 + SHA-256
            signature["publicKey"] = QString::fromStdString(agentEciesPub_);
            signature["value"] = QString::fromStdString(sigHex);
            card["signature"] = signature;
        } catch (const std::exception&) {
            // Honest failure: leave the card unsigned rather than fabricate a signature.
        }
    }

    std::string cardStr = QJsonDocument(card).toJson(QJsonDocument::Compact).toStdString();
    return cardStr;
}

std::string PilotImpl::agentCard() {
    // Public discovery skill: build the signed card, then PUBLISH it to the shared discovery
    // topic so peers can find us. The inbound 'capabilities' request instead calls buildCard()
    // and replies WITHOUT this network write (I2). The !agentNpk_.empty() guard preserves the
    // prior behavior of never broadcasting a not-initialized stub (the old early-return sat
    // before this publish).
    std::string cardStr = buildCard();
    if (!agentNpk_.empty() && logosAPI_) {
        auto* delivery = logosAPI_->getClient("delivery_module");
        if (delivery && delivery->isConnected()) {
            delivery->invokeRemoteMethod(
                "delivery_module", "send",
                QString("/pilot/1/discovery/proto"),
                QString::fromStdString(cardStr), RPC_TIMEOUT);
        }
    }
    return cardStr;
}

// L2 — verify-before-cache guard for discovered_agents. agentDiscover keys every network card
// on _logos.npk; a raw INSERT OR REPLACE lets a forged same-npk card (attacker signing_key,
// verifies 'invalid' against the TOFU pin) EVICT a genuine last-known-valid row, after which
// settlement reads the forgery, fails verification, and records pay-failed (denial-of-payment).
// Caches `card` UNLESS doing so would overwrite an existing row whose stored card_json still
// verifies 'valid' with a card that does NOT itself verify 'valid'. A 'valid' card always
// (re)writes its own row; first contact (no prior valid row) always caches (unsigned/interop
// peers stay discoverable); verifyCardStatus(card,db) TOFU-pins on first contact as a side effect.
//
// KNOWN LIMITATION (PM3-F2, non-fund-loss): on a TRUE first contact where an attacker's forged
// same-npk card is processed BEFORE the genuine card in a single discovery pass, the forgery is
// TOFU-pinned and cached as the 'valid' row; the genuine card then verifies 'invalid' against the
// attacker pin and is refused, making the forgery sticky. This is the pre-existing TOFU
// first-contact race, NOT a new payment risk: H1 (discoveredPayoutFor requires payout==_logos.npk)
// and the pinned-key reply-signature binding both still refuse payment to a forged key.
//
// Returns true iff a row was written. db==nullptr or npk-less card -> false.
bool a2aCacheDiscoveredCard(sqlite3* db, const QJsonObject& card,
                            const std::string& topic, const std::string& lastSeen) {
    if (!db) return false;
    std::string npk = card["_logos"].toObject()["npk"].toString().toStdString();
    if (npk.empty()) return false;

    if (verifyCardStatus(card, db) != QStringLiteral("valid")) {
        bool existingValid = false;
        sqlite3_stmt* chk = nullptr;
        if (sqlite3_prepare_v2(db,
                "SELECT card_json FROM discovered_agents WHERE npk = ?;", -1, &chk, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(chk, 1, npk.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(chk) == SQLITE_ROW && sqlite3_column_text(chk, 0)) {
                std::string exStr = reinterpret_cast<const char*>(sqlite3_column_text(chk, 0));
                QJsonDocument exDoc = QJsonDocument::fromJson(QByteArray::fromStdString(exStr));
                if (exDoc.isObject() && verifyCardStatus(exDoc.object(), db) == QStringLiteral("valid"))
                    existingValid = true;
            }
        }
        sqlite3_finalize(chk);
        if (existingValid) return false;   // L2: never let a non-valid card evict a validated row
    }

    std::string cardJson = QJsonDocument(card).toJson(QJsonDocument::Compact).toStdString();
    sqlite3_stmt* ins = nullptr;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO discovered_agents (npk, card_json, topic, last_seen) VALUES (?, ?, ?, ?);",
            -1, &ins, nullptr) != SQLITE_OK) { sqlite3_finalize(ins); return false; }
    sqlite3_bind_text(ins, 1, npk.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 2, cardJson.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 3, topic.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 4, lastSeen.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(ins);
    sqlite3_finalize(ins);
    return true;
}

std::string PilotImpl::agentDiscover(const std::string& topic) {
    if (!logosAPI_) return "{\"error\": \"not initialized\"}";

    std::string discoveryTopic = topic.empty()
        ? "/pilot/1/discovery/proto"
        : "/pilot/1/discovery-" + topic + "/proto";

    QJsonArray agents;

    // 1. Check local cache first
    if (db_) {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_,
            "SELECT npk, card_json FROM discovered_agents WHERE topic = ? ORDER BY last_seen DESC;",
            -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, discoveryTopic.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string cardStr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            QJsonDocument cardDoc = QJsonDocument::fromJson(QByteArray::fromStdString(cardStr));
            if (cardDoc.isObject()) {
                QJsonObject obj = cardDoc.object();
                // DB-aware: also PINS this identity on first contact (TOFU) so a later card
                // reusing the npk under a different signing_key is later rejected.
                obj["signature_status"] = verifyCardStatus(obj, db_);
                agents.append(obj);
            }
        }
        sqlite3_finalize(stmt);
    }

    // 2. Try network discovery via delivery_module
    auto* delivery = logosAPI_->getClient("delivery_module");
    if (delivery && delivery->isConnected()) {
        delivery->invokeRemoteMethod(
            "delivery_module", "subscribe",
            QString::fromStdString(discoveryTopic), RPC_TIMEOUT);

        QVariant storeResult = delivery->invokeRemoteMethod(
            "delivery_module", "storeQuery",
            QString::fromStdString(discoveryTopic), RPC_TIMEOUT);

        if (!storeResult.isNull()) {
            QJsonDocument netDoc = QJsonDocument::fromJson(storeResult.toString().toUtf8());
            QJsonArray netAgents = netDoc.isArray() ? netDoc.array() : QJsonArray();

            // 3. Cache network results in SQLite
            auto now = std::chrono::system_clock::now();
            std::string ts = std::to_string(
                std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());

            for (const auto& val : netAgents) {
                if (!val.isObject()) continue;
                QJsonObject agentCard = val.toObject();
                // Key the cache on the card's real payment identity (_logos.npk), NOT its display
                // name: every Pilot card hardcodes name "Pilot Agent", so keying on name collapses
                // two distinct agents into one row (breaking the two-agent scenario). matchedCardLogos
                // also resolves by _logos.npk, so this keeps the cache key and the lookup key aligned.
                QString npk = agentCard["_logos"].toObject()["npk"].toString();
                if (npk.isEmpty()) continue;

                // L2: verify-before-cache. Never let a non-valid card (e.g. a forged same-npk
                // card that verifies 'invalid' against the TOFU pin) EVICT a last-known-valid row
                // — that would make settlement read the forgery, fail verification, and record
                // pay-failed (denial-of-payment). The helper also TOFU-pins on first contact.
                if (db_)
                    a2aCacheDiscoveredCard(db_, agentCard, discoveryTopic, ts);

                // Flag signature validity on the returned card (cache above kept the
                // unannotated original so it can be re-verified later). DB-aware so first
                // contact PINS (npk -> signing_key) for TOFU payout binding.
                agentCard["signature_status"] = verifyCardStatus(agentCard, db_);

                bool found = false;
                QString cardNpk = agentCard["_logos"].toObject()["npk"].toString();
                for (const auto& existing : agents)
                    if (existing.toObject()["_logos"].toObject()["npk"].toString() == cardNpk) { found = true; break; }
                if (!found)
                    agents.append(agentCard);
            }
        }
    }

    // M3 — opportunistically LRU-trim the discovered_agents card cache after a discovery pass
    // (it is the table this method grows). Pins and in-flight-outbound cards are spared.
    if (db_) a2aEvictDiscoveryCache(db_);

    QJsonObject res;
    res["agents"] = agents;
    res["count"] = agents.size();
    res["topic"] = QString::fromStdString(discoveryTopic);
    if (agents.isEmpty())
        res["note"] = QString("no agents found — subscribed for live cards");
    return QJsonDocument(res).toJson(QJsonDocument::Compact).toStdString();
}

std::string PilotImpl::agentTask(const std::string& agentAddress, const std::string& skill, const std::string& paramsJson) {
    if (!logosAPI_) return "{\"error\": \"not initialized\"}";

    std::string taskId = genUuid();
    std::string replyTopic = "/pilot/1/reply-" + taskId + "/proto";

    auto* delivery = logosAPI_->getClient("delivery_module");
    if (!delivery || !delivery->isConnected()) return "{\"error\": \"delivery module unavailable\"}";

    QVariant subResult = delivery->invokeRemoteMethod(
        "delivery_module", "subscribe",
        QString::fromStdString(replyTopic), RPC_TIMEOUT);
    if (subResult.isNull())
        return "{\"error\": \"failed to subscribe to reply topic\"}";

    // Read the declared price for this skill from the target's discovered Agent Card and
    // record a PENDING outbound task BEFORE we send, so the reply consumer (messageReceived
    // -> handleA2AReply -> settleOutboundReply) can settle the price autonomously the
    // moment the peer accepts. ON CONFLICT(id) DO NOTHING: a resubmit of the same task id
    // must NOT reset the row back to 'submitted' and re-arm a second payment for work the
    // first reply already settled (the settling-claim keys off 'submitted').
    int64_t price = discoveredPriceFor(agentAddress, skill);
    if (db_) {
        std::string ts = nowTimestamp();
        sqlite3_stmt* ins = nullptr;
        sqlite3_prepare_v2(db_,
            "INSERT INTO outbound_tasks "
            "(id, agent_address, skill, price, reply_topic, state, created_at, updated_at) "
            "VALUES (?, ?, ?, ?, ?, 'submitted', ?, ?) "
            "ON CONFLICT(id) DO NOTHING;", -1, &ins, nullptr);
        sqlite3_bind_text(ins, 1, taskId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 2, agentAddress.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 3, skill.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(ins, 4, price);
        sqlite3_bind_text(ins, 5, replyTopic.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 6, ts.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 7, ts.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(ins);
        sqlite3_finalize(ins);
    }

    QJsonDocument paramsDoc = QJsonDocument::fromJson(QByteArray::fromStdString(paramsJson));
    QJsonValue textValue = paramsDoc.isObject() ? QJsonValue(paramsDoc.object()) :
        (paramsDoc.isArray() ? QJsonValue(paramsDoc.array()) :
         QJsonValue(QString::fromStdString(paramsJson)));

    QJsonObject textPart;
    textPart["type"] = QString("text");
    textPart["text"] = textValue;
    QJsonArray parts;
    parts.append(textPart);

    QJsonObject message;
    message["role"] = QString("user");
    message["parts"] = parts;

    QJsonObject metadata;
    metadata["skill"] = QString::fromStdString(skill);

    QJsonObject params;
    params["id"] = QString::fromStdString(taskId);
    params["message"] = message;
    params["metadata"] = metadata;

    QJsonObject logosExt;
    logosExt["sender_npk"] = QString::fromStdString(agentNpk_);
    // L1: our dedicated ENCRYPTION public key (a2aSelfEncKey()), the key the doer encrypts EVERY
    // reply to; we decrypt those replies with agentEncPriv_ (a2aTryDecrypt also falls back to the
    // legacy signing key). Without this the doer has no key it can encrypt a reply we can read
    // back (M4). Pre-split self falls back to the signing ECIES key inside a2aSelfEncKey().
    logosExt["sender_ecies"] = QString::fromStdString(a2aSelfEncKey());
    logosExt["reply_topic"] = QString::fromStdString(replyTopic);
    logosExt["timestamp"] = QString::fromStdString(nowTimestamp());

    QJsonObject request;
    request["jsonrpc"] = QString("2.0");
    request["method"] = QString("tasks/send");
    request["id"] = QString::fromStdString(taskId);
    request["params"] = params;
    request["_logos"] = logosExt;

    // Route+encrypt to the doer's ECIES key (its published _logos.signing_key / inbox id),
    // NOT the wallet viewing key: the doer can only decrypt what is encrypted to the key it
    // HOLDS the private half of. a2aRoutingKeyFor resolves it from the discovered card.
    std::string routingKey = a2aRoutingKeyFor(agentAddress);
    if (routingKey.empty())
        return "{\"error\": \"no valid Agent Card resolves a messaging key for this peer "
               "(the address is a wallet/npk blob the doer cannot decrypt with); "
               "discover its card first\"}";

    std::string requestStr = signA2AEnvelope(request, agentEciesPub_, agentEciesPriv_);   // H2: sign outbound request
    std::vector<uint8_t> plainBytes(requestStr.begin(), requestStr.end());
    std::string encPayload;
    try {
        ECIESCiphertext encrypted = eciesEncrypt(routingKey, plainBytes);
        encPayload = eciesSerialize(encrypted);
    } catch (const std::exception& e) {
        return "{\"error\": \"encryption failed: " + std::string(e.what()) + "\"}";
    }

    std::string inboxTopic = "/pilot/1/inbox-" + routingKey + "/proto";
    delivery->invokeRemoteMethod(
        "delivery_module", "send",
        QString::fromStdString(inboxTopic),
        QString::fromStdString(encPayload), RPC_TIMEOUT);

    QJsonObject status;
    status["state"] = QString("submitted");
    QJsonObject logosReply;
    logosReply["reply_topic"] = QString::fromStdString(replyTopic);
    logosReply["price"] = static_cast<double>(price);   // declared LEZ price to settle on acceptance
    logosReply["payment"] = QString(price > 0 ? "pending-on-acceptance" : "none-declared");
    QJsonObject result;
    result["id"] = QString::fromStdString(taskId);
    result["status"] = status;
    result["_logos"] = logosReply;
    return QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString();
}

std::string PilotImpl::agentSubscribe(const std::string& agentAddress, const std::string& taskId) {
    if (!logosAPI_) return "{\"error\": \"not initialized\"}";

    // Subscribe where the server actually emits: the reply topic. The previous
    // "/pilot/1/task-<id>/proto" topic was never published to, so updates were lost.
    std::string replyTopic = "/pilot/1/reply-" + taskId + "/proto";

    auto* delivery = logosAPI_->getClient("delivery_module");
    if (!delivery || !delivery->isConnected()) return "{\"error\": \"delivery module unavailable\"}";

    QVariant result = delivery->invokeRemoteMethod(
        "delivery_module", "subscribe",
        QString::fromStdString(replyTopic), RPC_TIMEOUT);
    if (result.isNull())
        return "{\"error\": \"subscribe failed\"}";

    QJsonObject rpcParams;
    rpcParams["id"] = QString::fromStdString(taskId);

    QJsonObject logosExt;
    logosExt["sender_npk"] = QString::fromStdString(agentNpk_);
    // Tell the server where to send the status update — without this the inbound
    // server's reply (handleInboundA2A) has no topic and the update is dropped.
    logosExt["reply_topic"] = QString::fromStdString(replyTopic);
    // L1: same dedicated ENCRYPTION reply key as the original task (a2aSelfEncKey()): the doer
    // encrypts the subscribe reply to it and we decrypt with agentEncPriv_ (a2aTryDecrypt falls
    // back to the legacy signing key for a pre-split doer).
    logosExt["sender_ecies"] = QString::fromStdString(a2aSelfEncKey());
    logosExt["timestamp"] = QString::fromStdString(nowTimestamp());

    QJsonObject request;
    request["jsonrpc"] = QString("2.0");
    request["method"] = QString("tasks/sendSubscribe");
    request["id"] = QString::fromStdString(genUuid());
    request["params"] = rpcParams;
    request["_logos"] = logosExt;

    // Same ECIES routing as agentTask: encrypt+address to the doer's signing_key/inbox key
    // (resolved from its card), never the wallet viewing key it cannot decrypt with.
    std::string routingKey = a2aRoutingKeyFor(agentAddress);
    if (routingKey.empty())
        return "{\"error\": \"no valid Agent Card resolves a messaging key for this peer "
               "(the address is a wallet/npk blob the doer cannot decrypt with); "
               "discover its card first\"}";

    std::string reqStr = signA2AEnvelope(request, agentEciesPub_, agentEciesPriv_);   // H2: sign outbound request
    std::vector<uint8_t> subPlain(reqStr.begin(), reqStr.end());
    std::string subPayload;
    try {
        ECIESCiphertext subEnc = eciesEncrypt(routingKey, subPlain);
        subPayload = eciesSerialize(subEnc);
    } catch (const std::exception& e) {
        return "{\"error\": \"encryption failed: " + std::string(e.what()) + "\"}";
    }

    std::string inboxTopic = "/pilot/1/inbox-" + routingKey + "/proto";
    delivery->invokeRemoteMethod(
        "delivery_module", "send",
        QString::fromStdString(inboxTopic),
        QString::fromStdString(subPayload), RPC_TIMEOUT);

    QJsonObject res;
    res["subscribed"] = true;
    res["task_id"] = QString::fromStdString(taskId);
    res["topic"] = QString::fromStdString(replyTopic);
    return QJsonDocument(res).toJson(QJsonDocument::Compact).toStdString();
}

bool PilotImpl::agentCancel(const std::string& agentAddress, const std::string& taskId) {
    if (!logosAPI_) return false;

    auto* delivery = logosAPI_->getClient("delivery_module");
    if (!delivery || !delivery->isConnected()) return false;

    QJsonObject rpcParams;
    rpcParams["id"] = QString::fromStdString(taskId);

    QJsonObject logosExt;
    logosExt["sender_npk"] = QString::fromStdString(agentNpk_);
    logosExt["timestamp"] = QString::fromStdString(nowTimestamp());

    QJsonObject request;
    request["jsonrpc"] = QString("2.0");
    request["method"] = QString("tasks/cancel");
    request["id"] = QString::fromStdString(genUuid());
    request["params"] = rpcParams;
    request["_logos"] = logosExt;

    // Same ECIES routing as agentTask: encrypt+address to the doer's signing_key/inbox key
    // (resolved from its card), never the wallet viewing key it cannot decrypt with.
    std::string routingKey = a2aRoutingKeyFor(agentAddress);
    if (routingKey.empty()) return false;   // unroutable (npk/wallet blob, no card) -> don't dead-drop

    std::string cancelStr = signA2AEnvelope(request, agentEciesPub_, agentEciesPriv_);   // H2: sign outbound request
    std::vector<uint8_t> cancelPlain(cancelStr.begin(), cancelStr.end());
    std::string cancelPayload;
    try {
        ECIESCiphertext cancelEnc = eciesEncrypt(routingKey, cancelPlain);
        cancelPayload = eciesSerialize(cancelEnc);
    } catch (const std::exception& e) {
        return false;
    }

    std::string inboxTopic = "/pilot/1/inbox-" + routingKey + "/proto";
    delivery->invokeRemoteMethod(
        "delivery_module", "send",
        QString::fromStdString(inboxTopic),
        QString::fromStdString(cancelPayload), RPC_TIMEOUT);

    // Stop listening on the reply topic (where the server emits). The old
    // "/pilot/1/task-<id>/proto" topic is no longer used, so there is nothing to drop.
    std::string replyTopic = "/pilot/1/reply-" + taskId + "/proto";
    delivery->invokeRemoteMethod(
        "delivery_module", "unsubscribe",
        QString::fromStdString(replyTopic), RPC_TIMEOUT);

    // Mark the local outbound task canceled so a late reply can't trigger a payment.
    if (db_) {
        std::string ts = nowTimestamp();
        sqlite3_stmt* upd = nullptr;
        sqlite3_prepare_v2(db_,
            "UPDATE outbound_tasks SET state='canceled', updated_at=? WHERE id=? AND state='submitted';",
            -1, &upd, nullptr);
        sqlite3_bind_text(upd, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(upd, 2, taskId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(upd);
        sqlite3_finalize(upd);
    }

    return true;
}

// Find the discovered Agent Card for `agentAddress` by its PAYMENT identity (_logos.npk).
// Callers pass the doer's npk-ish address; we reduce both that address and each card's
// _logos.npk through extractEncryptionKey and compare, so the match keys on ONE canonical
// payment-identity field with no loose substring fallbacks (a substring of the npk JSON
// would let an unrelated card's price/payout/signing_key bleed in). NOTE: this matches on
// the PAYMENT identity (npk), distinct from the ECIES MESSAGING key (signing_key) the
// matched card then supplies for routing via a2aRoutingKeyFor — the two are unified in
// publication but resolved here from the one card. Returns the parsed _logos object, or an
// empty object when nothing matches.
static QJsonObject matchedCardLogos(sqlite3* db, const std::string& agentAddress) {
    QJsonObject out;
    if (!db) return out;
    std::string key = extractEncryptionKey(agentAddress);
    if (key.empty()) return out;

    QJsonObject match;
    std::string matchPayout;
    bool have = false, ambiguous = false;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT card_json FROM discovered_agents;", -1, &st, nullptr);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char* c = sqlite3_column_text(st, 0);
        if (!c) continue;
        std::string cardStr = reinterpret_cast<const char*>(c);
        QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(cardStr));
        if (!doc.isObject()) continue;
        QJsonObject card = doc.object();
        QJsonObject logos = card["_logos"].toObject();
        if (extractEncryptionKey(logos["npk"].toString().toStdString()) != key) continue;
        // AUTHENTICATED PAYOUT (M3/M5): only an AUTHENTIC, identity-bound card may drive a
        // payout. An unsigned/invalid/unpinned-changed card is ignored — so an attacker
        // cannot publish a card under a victim's npk to redirect the payout, and a forged
        // card that swaps signing_key under an existing pinned npk verifies 'invalid' here.
        if (verifyCardStatus(card, db) != QStringLiteral("valid")) continue;
        std::string payout = logos["payout"].toString().toStdString();
        if (!have) { match = logos; matchPayout = payout; have = true; }
        else if (payout != matchPayout) { ambiguous = true; break; }   // conflicting valid claims
    }
    sqlite3_finalize(st);
    // Reject ambiguous multi-match: never silently take the first row when two VALID cards
    // claim the SAME npk with DIFFERENT payouts. Refuse (empty) so the caller pays nobody.
    if (!have || ambiguous) return out;
    return match;
}

// Declared price for `skill` from the canonically-matched Agent Card (_logos.pricing).
// Returns 0 when no card or no price is on file — we never fabricate a price, so an
// unknown price means "nothing to settle".
int64_t PilotImpl::discoveredPriceFor(const std::string& agentAddress, const std::string& skill) {
    QJsonObject pricing = matchedCardLogos(db_, agentAddress)["pricing"].toObject();
    QString skq = QString::fromStdString(skill);
    return pricing.contains(skq) ? static_cast<int64_t>(pricing[skq].toDouble()) : 0;
}

// Payout account (_logos.payout) from the canonically-matched, AUTHENTICATED Agent Card —
// the shielded LEZ account a requester pays the price to. matchedCardLogos only returns a
// card that verifyCardStatus()=='valid' (signed by its bound identity key AND consistent
// with the TOFU pin) and is unambiguous, so this is empty for an unsigned/invalid/
// unpinned-changed/ambiguous card; the caller then refuses to pay rather than pay an
// unauthenticated payout or mis-target the messaging address (M5).
std::string PilotImpl::discoveredPayoutFor(const std::string& agentAddress) {
    QJsonObject logos = matchedCardLogos(db_, agentAddress);
    std::string payout = logos["payout"].toString().toStdString();
    std::string npk    = logos["npk"].toString().toStdString();
    // H1: pay ONLY when the card's payout account is bound to the card's payment identity.
    // A genuine card always sets payout == _logos.npk (agentCard sets both to agentNpk_); a
    // forged card with a divergent payout could redirect funds to an attacker account even
    // after passing the TOFU/signature gate, so refuse it (empty -> the caller pays nobody).
    if (npk.empty() || payout != npk) return std::string();
    return payout;
}

// ECIES routing/encryption key for an outbound A2A message to `agentAddress` (see header).
// A2A messaging is unified on the doer's ECIES key — the one it HOLDS the private half of,
// subscribes its inbox under, and publishes as _logos.signing_key. We resolve that
// signing_key from the canonically-matched discovered Agent Card. With no card (or no
// signing_key) on file we fall back to agentAddress verbatim: a caller may pass the ECIES
// key directly, and a plain key round-trips unchanged. We deliberately do NOT reduce it
// through extractEncryptionKey (which extracts the wallet VIEWING key from an npk blob — a
// key the doer cannot decrypt with, the original request-leg blocker).
std::string PilotImpl::a2aRoutingKeyFor(const std::string& agentAddress) {
    // L1: prefer the doer's dedicated ENCRYPTION key (_logos.enc_key) — the key it subscribes its
    // inbox under and holds the private half of. A pre-split peer card has no enc_key, so fall back
    // to its _logos.signing_key (the old unified key, which that peer still decrypts with).
    QJsonObject logos = matchedCardLogos(db_, agentAddress);
    std::string key = logos["enc_key"].toString().toStdString();
    if (key.empty()) key = logos["signing_key"].toString().toStdString();   // pre-split peer fallback
    if (!key.empty()) return key;
    // No discovered/valid Agent Card resolved a messaging key. A caller may legitimately have
    // passed a BARE ECIES key directly (a plain hex string), which round-trips verbatim. But if
    // the address is an npk/wallet BLOB (a JSON object carrying the wallet viewing key), routing
    // to it would ENCRYPT the request to a key the doer cannot decrypt with — a silent
    // dead-drop into the void. Fail LOUDLY instead: return empty so callers refuse to send.
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(agentAddress));
    if (doc.isObject()) return std::string();   // npk/wallet blob + no card -> unroutable
    return agentAddress;                          // bare ECIES key -> route verbatim
}

// Test seam (L1): expose the private routing-key resolver. See pilot_impl.h.
std::string pilotTestA2ARoutingKey(PilotImpl& impl, const std::string& agentAddress) {
    return impl.a2aRoutingKeyFor(agentAddress);
}

// L1 dual-key decrypt: try the dedicated ENCRYPTION key (agentEncPriv_) first — the key our card
// now advertises as _logos.enc_key and that peers encrypt to — then fall back to the legacy
// SIGNING key (agentEciesPriv_) for a pre-split peer that still encrypts to _logos.signing_key.
// Returns true (out = plaintext) on the first key that decrypts; false (out untouched) otherwise.
bool PilotImpl::a2aTryDecrypt(const std::string& payload, std::string& out) const {
    ECIESCiphertext ct;
    try { ct = eciesDeserialize(payload); }
    catch (...) { return false; }
    if (!agentEncPriv_.empty()) {
        try {
            std::vector<uint8_t> plain = eciesDecrypt(agentEncPriv_, ct);
            out.assign(plain.begin(), plain.end());
            return true;
        } catch (...) { /* not encrypted to the enc key — try the legacy signing key */ }
    }
    if (!agentEciesPriv_.empty()) {
        try {
            std::vector<uint8_t> plain = eciesDecrypt(agentEciesPriv_, ct);
            out.assign(plain.begin(), plain.end());
            return true;
        } catch (...) { /* fall through */ }
    }
    return false;
}

// Requester-side reply consumer (Functionality #8) — ECIES/transport wrapper. A peer
// server replies on "/pilot/1/reply-<taskId>/proto" with a JSON-RPC status update. We
// decrypt it with a2aTryDecrypt (the enc key first, the legacy signing key as fallback —
// the doer encrypted to the sender_ecies we declared on the request, per the A2A contract — M4)
// and hand the decrypted reply to verifyAndSettleReply, which AUTHENTICATES the doer's signature
// before settling. Undecryptable/malformed input is dropped (ambiguity -> inaction).
void PilotImpl::handleA2AReply(const std::string& topic, const std::string& payload) {
    if (!db_ || (agentEncPriv_.empty() && agentEciesPriv_.empty())) return;

    const std::string prefix = "/pilot/1/reply-";
    const std::string suffix = "/proto";
    if (topic.rfind(prefix, 0) != 0 || topic.size() <= prefix.size() + suffix.size()) return;
    if (topic.compare(topic.size() - suffix.size(), suffix.size(), suffix) != 0) return;
    std::string taskId = topic.substr(prefix.size(), topic.size() - prefix.size() - suffix.size());

    std::string json;
    if (!a2aTryDecrypt(payload, json)) return;

    // ECIES decryption proves only that the reply was encrypted to our PUBLIC key — which any
    // observer can do. The SIGNATURE check (verifyAndSettleReply) is what proves the doer
    // authored it, so settlement can't be forged.
    verifyAndSettleReply(taskId, json);
}

// Signature gate (FIX 1 — the BLOCKER) + settle. See pilot_impl.h for the threat model.
// handleA2AReply decrypts a peer's reply then calls this; tests drive it directly.
void PilotImpl::verifyAndSettleReply(const std::string& taskId, const std::string& replyJson) {
    if (!db_ || taskId.empty()) return;

    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(replyJson));
    if (!doc.isObject()) return;
    QJsonObject obj = doc.object();

    // Status arrives either as a tasks/send reply (result.status.state) or as a
    // tasks/statusUpdate notification (params.status.state).
    QString state = obj["result"].toObject()["status"].toObject()["state"].toString();
    if (state.isEmpty())
        state = obj["params"].toObject()["status"].toObject()["state"].toString();
    if (state.isEmpty()) return;

    // Bind the signed reply to THE task being settled. The task id lives inside the signed
    // canonical bytes (result.id for a tasks/send reply, params.id for a statusUpdate), while
    // taskId here comes from the public Waku reply topic. A genuine doer-signed 'completed' for
    // task A could otherwise be replayed verbatim onto task B's reply topic (same pinned doer)
    // and settle B for work never done. Require the signed id to equal the id being settled.
    QString embeddedId = obj["result"].toObject()["id"].toString();
    if (embeddedId.isEmpty())
        embeddedId = obj["params"].toObject()["id"].toString();
    if (embeddedId.toStdString() != taskId) return;   // replay / id mismatch -> drop (NO settle)

    // Resolve the doer's AUTHORITATIVE signing_key from THIS task's agent_address via its
    // identity-validated, TOFU-pinned Agent Card (matchedCardLogos only returns a card that
    // verifyCardStatus(card,db_)=='valid' and is unambiguous). No such card -> no key to
    // authenticate against -> drop. We deliberately do NOT fall back to a2aRoutingKeyFor's
    // bare-address behaviour here: an unauthenticated reply must never settle a payment.
    std::string agentAddress;
    sqlite3_stmt* sel = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT agent_address FROM outbound_tasks WHERE id=?;", -1, &sel, nullptr);
    sqlite3_bind_text(sel, 1, taskId.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(sel) == SQLITE_ROW && sqlite3_column_text(sel, 0))
        agentAddress = reinterpret_cast<const char*>(sqlite3_column_text(sel, 0));
    sqlite3_finalize(sel);
    if (agentAddress.empty()) return;   // unknown task -> nothing to authenticate or settle

    std::string authKey =
        matchedCardLogos(db_, agentAddress)["signing_key"].toString().toStdString();
    if (authKey.empty()) return;        // no authenticated card on file -> cannot verify -> drop

    // Extract the doer's signature and reconstruct the canonical bytes it signed: the envelope
    // WITH _logos.signing_key but WITHOUT _logos.signature (mirrors agentCard()/replyToPeer).
    QJsonObject logos = obj.value("_logos").toObject();
    QString sig = logos.value("signature").toString();
    if (sig.isEmpty()) return;          // unsigned reply is forgeable -> drop (NO settle)
    logos.remove("signature");
    obj["_logos"] = logos;
    std::string canonical = QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString();
    std::vector<uint8_t> canonicalBytes(canonical.begin(), canonical.end());
    // Verify against the AUTHORITATIVE pinned key, NEVER the reply-supplied _logos.signing_key:
    // a forger can put any key (or the genuine key) there, but only the real doer can produce
    // a signature that verifies under the pinned key. Mismatch -> drop (NO settle, NO pay).
    if (!verifySignature(canonicalBytes, sig.toStdString(), authKey)) return;

    settleOutboundReply(taskId, state.toStdString());
}

// Pure pay-on-completion FSM for an outbound task WE submitted (no transport/ECIES — the
// decrypt happens in handleA2AReply; this is unit-testable directly).
//
// ASKER PAYS THE DOER: the doer RUNS the real skill and reports done; WE (the asker) pay.
// TERMINAL-ONLY TRIGGER (money safety): we settle ONLY on the doer's terminal SUCCESS,
// 'completed'. Progress replies {accepted, working} (and the owner-gated 'input-required')
// are NON-settling: we leave the row 'submitted' so a LATER 'completed' can still claim —
// paying on 'accepted' would pay for work the doer has not finished (no-fakery). We NEVER
// settle on {failed, canceled, rejected}: those are terminal negatives.
//
// The price is paid to the doer's DECLARED, AUTHENTICATED Agent Card payout account (M5) —
// only a card that verifyCardStatus()=='valid' (signed by its bound identity key AND
// consistent with the TOFU pin) can name a payee; with no such payout on file we refuse to
// pay (honest 'pay-failed') and never the messaging address. The atomic submitted->settling
// claim makes a task settle AT MOST ONCE even across a (possibly repeated) 'completed'.
void PilotImpl::settleOutboundReply(const std::string& taskId, const std::string& state) {
    if (!db_ || taskId.empty() || state.empty()) return;
    const std::string& st = state;

    if (st != "completed") {
        // Non-settling reply. Record a TERMINAL negative outcome so the task stops and a
        // stale reply can't later pay; but leave a still-settleable 'submitted' row alone
        // for progress/interim states (accepted/working/input-required) so a LATER
        // 'completed' can still claim and settle. Never overwrite an already-decided state.
        if (st == "failed" || st == "canceled" || st == "rejected") {
            // A contradictory terminal RETRACTS a payment still pending owner approval: if
            // this task is parked in 'awaiting-approval', its linked spend is HELD/NOTIFIED
            // and must be rejected (FIX 4b). rejectSpend moves HELD/NOTIFIED -> REJECTED and,
            // via advanceLinkedOutboundTask, drives THIS row to 'pay-failed' — so a peer that
            // first said 'completed' then 'failed' never leaves a held payment dangling.
            std::string curState, spendId;
            sqlite3_stmt* q = nullptr;
            sqlite3_prepare_v2(db_,
                "SELECT state, COALESCE(spend_request_id,'') FROM outbound_tasks WHERE id=?;",
                -1, &q, nullptr);
            sqlite3_bind_text(q, 1, taskId.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(q) == SQLITE_ROW) {
                if (sqlite3_column_text(q, 0)) curState = reinterpret_cast<const char*>(sqlite3_column_text(q, 0));
                if (sqlite3_column_text(q, 1)) spendId = reinterpret_cast<const char*>(sqlite3_column_text(q, 1));
            }
            sqlite3_finalize(q);

            if (curState == "awaiting-approval" && !spendId.empty()) {
                rejectSpend(spendId);   // HELD/NOTIFIED -> REJECTED + row -> 'pay-failed'
                return;
            }

            std::string ts = nowTimestamp();
            sqlite3_stmt* upd = nullptr;
            sqlite3_prepare_v2(db_,
                "UPDATE outbound_tasks SET state=?, updated_at=? WHERE id=? AND state IN "
                "('submitted','settling');", -1, &upd, nullptr);
            sqlite3_bind_text(upd, 1, st.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(upd, 2, ts.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(upd, 3, taskId.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(upd);
            sqlite3_finalize(upd);
        }
        return;
    }

    sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);  // L8: claim+create+link as one unit, committed before the wallet RPC
    // Atomically claim the task for settlement: submitted -> settling, exactly once. If
    // we don't win the claim (already settling/paid/awaiting-approval/pay-failed/canceled/
    // unknown), do nothing — this is what prevents a double-spend across a repeated
    // 'completed' reply.
    std::string ts = nowTimestamp();
    sqlite3_stmt* claim = nullptr;
    sqlite3_prepare_v2(db_,
        "UPDATE outbound_tasks SET state='settling', updated_at=? WHERE id=? AND state='submitted';",
        -1, &claim, nullptr);
    sqlite3_bind_text(claim, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(claim, 2, taskId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(claim);
    sqlite3_finalize(claim);
    if (sqlite3_changes(db_) == 0) { sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr); return; }

    std::string agentAddress, skill;
    int64_t price = 0;
    sqlite3_stmt* sel = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT agent_address, skill, price FROM outbound_tasks WHERE id=?;", -1, &sel, nullptr);
    sqlite3_bind_text(sel, 1, taskId.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(sel) == SQLITE_ROW) {
        if (sqlite3_column_text(sel, 0)) agentAddress = reinterpret_cast<const char*>(sqlite3_column_text(sel, 0));
        if (sqlite3_column_text(sel, 1)) skill = reinterpret_cast<const char*>(sqlite3_column_text(sel, 1));
        price = sqlite3_column_int64(sel, 2);
    }
    sqlite3_finalize(sel);

    auto setOutbound = [&](const char* outState, const std::string& payout, const std::string& spendId) {
        std::string t = nowTimestamp();
        sqlite3_stmt* u = nullptr;
        sqlite3_prepare_v2(db_,
            "UPDATE outbound_tasks SET state=?, payout=?, spend_request_id=?, updated_at=? WHERE id=?;",
            -1, &u, nullptr);
        sqlite3_bind_text(u, 1, outState, -1, SQLITE_STATIC);
        sqlite3_bind_text(u, 2, payout.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(u, 3, spendId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(u, 4, t.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(u, 5, taskId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(u);
        sqlite3_finalize(u);
    };

    // No declared price -> nothing to settle (honest: we don't invent a price).
    if (price <= 0) { setOutbound("accepted-nopay", "", ""); sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr); return; }

    // PAYOUT (M5): pay the doer's DECLARED Agent Card payout account, never the messaging
    // address (paying that mis-targets / TX_FAILs). No payout on file -> refuse to pay.
    std::string payout = discoveredPayoutFor(agentAddress);
    if (payout.empty()) { setOutbound("pay-failed", "", ""); sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr); return; }

    // Create the spend through the SAME spending-FSM primitives the inbound path uses, and
    // LINK it to this outbound task up front (payout + spend_request_id) BEFORE gating. The
    // link is what lets an owner decision (approveSpend/rejectSpend) or a restart
    // (outboundTasksRecover) later advance THIS exact row by spend_request_id — without it
    // an above-threshold task would orphan in 'awaiting-approval' (M6) and a crash
    // mid-settle would never reconcile (M7). (Inlined rather than calling walletSend so the
    // outbound row is linked to the spend id and so the path is exercised even before a
    // wallet account is wired.)
    std::string spendId = createSpendRequest(payout, price,
        "A2A pay-on-acceptance: " + skill + " (task " + taskId + ")");
    setOutbound("settling", payout, spendId);
    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);  // L8: claim+create+link durable as one unit; gate runs in its own autocommit (never a write lock across a wallet RPC)

    // Same autonomous gate as walletSend/inbound: only when the price fits BOTH the per-tx
    // cap AND the remaining per-period budget do we pay without the owner. Any ambiguity
    // (over either limit) routes to the owner; we NEVER default to autonomous execution.
    bool withinTx = price <= spendLimitPerTx_;
    bool withinPeriod = periodSpent() + price <= spendLimitPerPeriod_;
    if (withinTx && withinPeriod) {
        bool ok = executeSpend(spendId);   // CREATED -> EXECUTING -> COMPLETED/TX_FAILED
        // Honest: 'paid' ONLY on a real transfer; a failed transfer is 'pay-failed'.
        setOutbound(ok ? "paid" : "pay-failed", payout, spendId);
        return;
    }

    // Above threshold (per-tx or per-period) -> owner-approval gate. The spend is HELD/
    // NOTIFIED and the outbound task waits in 'awaiting-approval' until approveSpend or
    // rejectSpend advances it to a terminal 'paid'/'pay-failed' (M6).
    holdForApproval(spendId,
        "A2A payment needs approval:\nSkill: " + a2aFlattenForPrompt(skill) +
        "\nAmount: " + std::to_string(price) +
        " LEZ\nTo (payout): " + a2aFlattenForPrompt(payout) + "\nExpires: 60 min\n/approve " + spendId +
        "\n/reject " + spendId,
        "Awaiting owner approval");
    setOutbound("awaiting-approval", payout, spendId);
}

// Outbound recovery on restart (M7). Two jobs, both idempotent:
//   1. Re-arm settlement for tasks still 'submitted': re-subscribe their reply topic so a
//      peer's accept/complete reply can still settle after a restart (the in-memory Waku
//      subscription died with the previous process). Best-effort; no delivery -> skipped.
//   2. Reconcile tasks caught mid-settle ('settling') against their LINKED spend_request,
//      so a crash between createSpendRequest and the terminal outbound update is not
//      orphaned: spend COMPLETED -> 'paid'; spend terminal-failed (TX_FAILED/REJECTED/
//      EXPIRED) -> 'pay-failed'; anything still in flight (CREATED/HELD/NOTIFIED/EXECUTING/
//      APPROVED) is LEFT for retry / the owner gate.
void PilotImpl::outboundTasksRecover() {
    if (!db_) return;

    // (0) L8 self-heal: an unlinked 'settling' row (no spend_request_id) never moved money, so
    // resetting it to 'submitted' safely re-arms settlement. Runs before (1) so healed rows get
    // re-subscribed this same pass. Rows WITH a linked spend are untouched (reconciled by (2)).
    {
        std::string ts = nowTimestamp();
        sqlite3_stmt* heal = nullptr;
        sqlite3_prepare_v2(db_,
            "UPDATE outbound_tasks SET state='submitted', updated_at=? "
            "WHERE state='settling' AND COALESCE(spend_request_id,'')='';", -1, &heal, nullptr);
        sqlite3_bind_text(heal, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(heal);
        sqlite3_finalize(heal);
    }

    // (1) Re-subscribe reply topics for still-open outbound tasks.
    auto* delivery = logosAPI_ ? logosAPI_->getClient("delivery_module") : nullptr;
    if (delivery && delivery->isConnected()) {
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db_,
            "SELECT reply_topic FROM outbound_tasks WHERE state='submitted';", -1, &st, nullptr);
        while (sqlite3_step(st) == SQLITE_ROW) {
            if (!sqlite3_column_text(st, 0)) continue;
            std::string topic = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
            if (topic.empty()) continue;
            delivery->invokeRemoteMethod("delivery_module", "subscribe",
                QString::fromStdString(topic), RPC_TIMEOUT);
        }
        sqlite3_finalize(st);
    }

    // (2) Reconcile 'settling' rows against their linked spend_request. Collect first, then
    // update, so we never hold a SELECT cursor open across the UPDATE.
    struct Row { std::string taskId, spendId; };
    std::vector<Row> rows;
    sqlite3_stmt* sel = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT id, COALESCE(spend_request_id,'') FROM outbound_tasks WHERE state='settling';",
        -1, &sel, nullptr);
    while (sqlite3_step(sel) == SQLITE_ROW) {
        Row r;
        if (sqlite3_column_text(sel, 0)) r.taskId = reinterpret_cast<const char*>(sqlite3_column_text(sel, 0));
        if (sqlite3_column_text(sel, 1)) r.spendId = reinterpret_cast<const char*>(sqlite3_column_text(sel, 1));
        if (!r.taskId.empty()) rows.push_back(r);
    }
    sqlite3_finalize(sel);

    for (const auto& r : rows) {
        if (r.spendId.empty()) continue;   // no linked spend -> nothing to reconcile; leave for retry

        std::string sstate;
        sqlite3_stmt* sq = nullptr;
        sqlite3_prepare_v2(db_, "SELECT state FROM spend_requests WHERE id=?;", -1, &sq, nullptr);
        sqlite3_bind_text(sq, 1, r.spendId.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(sq) == SQLITE_ROW && sqlite3_column_text(sq, 0))
            sstate = reinterpret_cast<const char*>(sqlite3_column_text(sq, 0));
        sqlite3_finalize(sq);

        const char* outState = nullptr;
        if (sstate == "COMPLETED") outState = "paid";
        else if (sstate == "TX_FAILED" || sstate == "REJECTED" || sstate == "EXPIRED")
            outState = "pay-failed";
        else continue;   // still in flight -> leave 'settling' for retry / owner decision

        std::string ts = nowTimestamp();
        sqlite3_stmt* u = nullptr;
        sqlite3_prepare_v2(db_,
            "UPDATE outbound_tasks SET state=?, updated_at=? WHERE id=? AND state='settling';",
            -1, &u, nullptr);
        sqlite3_bind_text(u, 1, outState, -1, SQLITE_STATIC);
        sqlite3_bind_text(u, 2, ts.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(u, 3, r.taskId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(u);
        sqlite3_finalize(u);
    }
}

std::string PilotImpl::programQuery(const std::string& programId, const std::string& paramsJson) {
    if (!logosAPI_) return "{\"error\": \"not initialized\"}";

    auto* wallet = logosAPI_->getClient("logos_execution_zone");
    if (!wallet || !wallet->isConnected()) return "{\"error\": \"wallet module unavailable\"}";

    QVariant result = wallet->invokeRemoteMethod(
        "logos_execution_zone", "queryProgram",
        QString::fromStdString(programId),
        QString::fromStdString(paramsJson), RPC_TIMEOUT);

    if (result.isNull()) {
        QJsonObject err;
        err["error"] = QString("program.query unsupported: the logos_execution_zone module exposes no program-query method at the pinned LEZ revision (verified against upstream source). Not a Pilot-side gap.");
        err["program"] = QString::fromStdString(programId);
        return QJsonDocument(err).toJson(QJsonDocument::Compact).toStdString();
    }

    QJsonObject res;
    res["program"] = QString::fromStdString(programId);
    QJsonDocument resultDoc = QJsonDocument::fromJson(result.toString().toUtf8());
    res["result"] = resultDoc.isObject() ? QJsonValue(resultDoc.object()) :
        (resultDoc.isArray() ? QJsonValue(resultDoc.array()) : QJsonValue(result.toString()));
    return QJsonDocument(res).toJson(QJsonDocument::Compact).toStdString();
}

// M3 — LRU-trim ONLY the heavy discovered_agents card_json cache to kA2ADiscoveredAgentsMax by
// last_seen. A row is deleted only if it is BOTH (a) not backing a non-terminal outbound_task and
// (b) outside the freshest kA2ADiscoveredAgentsMax by last_seen.
//   [FIX-B] pinned_identities / pinned_request_identities are AUTHORITATIVE TOFU stores and are
//           NEVER touched here — pin permanence IS the anti-impersonation / anti-payout-swap
//           property, and a pin row is tiny (npk + key + ts).
//   [FIX-E] a card referenced by a non-terminal outbound_task ('submitted','settling',
//           'awaiting-approval') is spared, else settleOutboundReply would lose the payout /
//           signing_key at settle time and an owed payment would silently never pay.
//   [FIX-C/FIX-F] built with std::to_string + the subquery `NOT IN (... LIMIT k)` form so it
//           executes on stock SQLite (which ships without SQLITE_ENABLE_UPDATE_DELETE_LIMIT).
// The three in-flight labels were verified against the outbound FSM in settleOutboundReply: the
// row is created 'submitted', atomically claimed to 'settling', and parked 'awaiting-approval'
// when above threshold; all other states ('paid','pay-failed','accepted-nopay','failed',
// 'canceled','rejected') are terminal.
void a2aEvictDiscoveryCache(sqlite3* db) {
    if (!db) return;
    std::string sql =
        "DELETE FROM discovered_agents "
        "WHERE npk NOT IN (SELECT agent_address FROM outbound_tasks "
        "                  WHERE state IN ('submitted','settling','awaiting-approval')) "
        "AND npk NOT IN (SELECT npk FROM discovered_agents "
        "                ORDER BY CAST(last_seen AS INTEGER) DESC LIMIT " +
        std::to_string(kA2ADiscoveredAgentsMax) + ");";
    sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
}

std::string PilotImpl::programCall(const std::string& programId, const std::string& instruction, const std::string& paramsJson) {
    if (!logosAPI_ || agentAccountId_.empty()) return "{\"error\": \"not initialized\"}";

    int64_t estimatedCost = 10;

    if (estimatedCost > spendLimitPerTx_) {
        std::string reqId = createSpendRequest(programId, estimatedCost,
            "program.call: " + instruction);
        QJsonObject res;
        res["status"] = QString("held");
        res["request_id"] = QString::fromStdString(reqId);
        res["message"] = QString("Program call requires approval");
        return QJsonDocument(res).toJson(QJsonDocument::Compact).toStdString();
    }

    auto* wallet = logosAPI_->getClient("logos_execution_zone");
    if (!wallet || !wallet->isConnected()) return "{\"error\": \"wallet module unavailable\"}";

    QVariant result = wallet->invokeRemoteMethod(
        "logos_execution_zone", "callProgram",
        QString::fromStdString(agentAccountId_),
        QString::fromStdString(programId),
        QString::fromStdString(instruction),
        QString::fromStdString(paramsJson), RPC_TIMEOUT);

    if (result.isNull()) {
        QJsonObject err;
        err["error"] = QString("program.call unsupported: the logos_execution_zone module exposes no program-call method at the pinned LEZ revision (verified against upstream source). Not a Pilot-side gap.");
        err["program"] = QString::fromStdString(programId);
        return QJsonDocument(err).toJson(QJsonDocument::Compact).toStdString();
    }

    QJsonObject res;
    res["program"] = QString::fromStdString(programId);
    res["instruction"] = QString::fromStdString(instruction);
    QJsonDocument resultDoc = QJsonDocument::fromJson(result.toString().toUtf8());
    res["result"] = resultDoc.isObject() ? QJsonValue(resultDoc.object()) :
        (resultDoc.isArray() ? QJsonValue(resultDoc.array()) : QJsonValue(result.toString()));
    return QJsonDocument(res).toJson(QJsonDocument::Compact).toStdString();
}

std::string PilotImpl::programDeploy(const std::string& binaryPath) {
    if (!logosAPI_ || agentAccountId_.empty()) return "{\"error\": \"not initialized\"}";

    // Read the compiled program binary. If we can't read it, say so honestly —
    // do NOT pretend a deploy happened.
    QFile binFile(QString::fromStdString(binaryPath));
    if (binaryPath.empty() || !binFile.open(QIODevice::ReadOnly)) {
        QJsonObject err;
        err["error"] = QString("cannot read program binary");
        err["binary"] = QString::fromStdString(binaryPath);
        return QJsonDocument(err).toJson(QJsonDocument::Compact).toStdString();
    }
    QByteArray binary = binFile.readAll();
    binFile.close();

    QByteArray hashHex =
        QCryptographicHash::hash(binary, QCryptographicHash::Sha256).toHex();
    std::string binaryHash = hashHex.toStdString();

    auto* wallet = logosAPI_->getClient("logos_execution_zone");
    if (!wallet || !wallet->isConnected()) return "{\"error\": \"wallet module unavailable\"}";

    // Attempt the REAL upstream deploy. mirrors programCall/programQuery: invoke
    // the method and surface an honest error if the runtime returns null.
    QVariant result = wallet->invokeRemoteMethod(
        "logos_execution_zone", "deployProgram",
        QString::fromStdString(agentAccountId_),
        QString::fromStdString(binary.toHex().toStdString()),
        QString::fromStdString(binaryHash), RPC_TIMEOUT);

    if (result.isNull()) {
        QJsonObject err;
        err["error"] = QString("program.deploy unsupported: LEZ program deployment is done via a direct sequencer transaction (NSSATransaction), which the logos_execution_zone module does not expose — no wallet-module deploy method exists at the pinned revision (verified against upstream source).");
        err["binary"] = QString::fromStdString(binaryPath);
        err["binary_hash"] = QString::fromStdString(binaryHash);
        return QJsonDocument(err).toJson(QJsonDocument::Compact).toStdString();
    }

    QJsonObject res;
    res["binary"] = QString::fromStdString(binaryPath);
    res["binary_hash"] = QString::fromStdString(binaryHash);
    QJsonDocument resultDoc = QJsonDocument::fromJson(result.toString().toUtf8());
    res["result"] = resultDoc.isObject() ? QJsonValue(resultDoc.object()) :
        (resultDoc.isArray() ? QJsonValue(resultDoc.array()) : QJsonValue(result.toString()));
    return QJsonDocument(res).toJson(QJsonDocument::Compact).toStdString();
}
