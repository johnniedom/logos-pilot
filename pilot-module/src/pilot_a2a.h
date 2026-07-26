#ifndef PILOT_A2A_H
#define PILOT_A2A_H

// Qt-bearing A2A declarations shared between pilot_a2a.cpp (definitions) and the unit tests.
// This is the SINGLE SOURCE of these prototypes so the tests no longer hand-declare them and
// drift out of sync with the definitions. It MUST NOT be included from pilot_impl.h, which is
// the pure-C++ universal interface and stays Qt-free.

#include <QString>
#include <QJsonObject>
#include <string>
#include <cstdint>

struct sqlite3;

// HONEST SUCCESS CONTRACT for an auto-serviced inbound A2A skill result. Returns true (=>
// 'completed', pay the doer) ONLY on an EXPLICIT, positive success signal. Returns false (=>
// 'failed', NO pay) when the result is either an object carrying "error", an explicit FALSE
// flag (success/joined/ok == false), or status in {failed,error}; OR an opaque/ambiguous shape
// that proves nothing about the run — a bare string/scalar, an empty object, an empty array, or
// an empty/unparseable result. Only a NON-EMPTY object with no negative signal, or a NON-EMPTY
// array, is success. Ambiguity -> failed, NEVER completed. Single-source so the inbound
// dispatcher and the unit tests share ONE definition. External linkage (defined in pilot_a2a.cpp).
bool a2aResultIsSuccess(const std::string& result);

// Owner-prompt sanitizer (L4/shared): flatten any peer-controlled string before it is
// interpolated into an owner notification, so a malicious npk/skill/recipient/reason cannot
// inject extra lines (e.g. a fake "/approve <sid>") into the human approval prompt. Replaces
// CR/LF with a space. Defined in pilot_a2a.cpp; used by every owner-prompt builder in both TUs.
std::string a2aFlattenForPrompt(const std::string& s);

// Inbound wallet-send owner-approval prompt builder (M4). Includes the payee `recipient`
// (the single most security-relevant field, previously omitted) and an optional `reason`,
// matching walletSend's own owner message. Flattens skill/recipient/reason internally;
// `senderDisplay` is supplied already-sanitized by a2aSenderDisplay. Defined in pilot_a2a_inbox.cpp.
std::string a2aWalletSendApprovalMessage(const std::string& skill, const std::string& senderDisplay,
                                         int64_t amount, const std::string& recipient,
                                         const std::string& reason, const std::string& spendId);

// Agent Card authenticity verdict (M3). Checks that the embedded ECDSA signature verifies AND
// was produced by the card's OWN published identity key (_logos.signing_key), so a card
// re-signed under a different key is rejected even when its signature is internally consistent.
// Returns one of: "valid", "invalid", "unsigned", "unbound". External linkage (defined in
// pilot_a2a.cpp); the single-arg form is authenticity relative only to the card's self-declared
// identity key.
QString verifyCardStatus(const QJsonObject& card);

// Identity-BOUND verdict (M3 + TOFU). Runs the self-consistency check above, then — only for a
// card it already deems 'valid' — binds that verdict to a FIRST-CONTACT pin of
// (_logos.npk -> _logos.signing_key) in pinned_identities. A later card reusing that npk under a
// DIFFERENT signing_key is rejected as 'invalid', defeating a from-scratch forgery that reuses a
// victim's npk with the attacker's own signing_key + payout. Payout-bearing callers use THIS one.
QString verifyCardStatus(const QJsonObject& card, sqlite3* db);

// Sign an OUTBOUND A2A request envelope (H2). Sets _logos.signing_key to our ECIES public
// key, removes any prior _logos.signature, computes the ECDSA-secp256k1 signature over the
// canonical (compact) bytes, and re-attaches _logos.signature — the SAME byte pattern the
// reply path (replyToPeer) and verifyInboundRequest use, so a doer can authenticate us.
// Returns the signed envelope as compact JSON. Defined in pilot_a2a.cpp.
std::string signA2AEnvelope(QJsonObject env, const std::string& eciesPub, const std::string& eciesPriv);

// Authenticate an INBOUND A2A request (H2). Requires a valid ECDSA-secp256k1 signature in
// _logos.signature over the canonical request bytes (envelope minus _logos.signature) produced
// by _logos.signing_key, then TOFU-pins (_logos.sender_npk -> signing_key) in a DEDICATED
// `pinned_request_identities` table — never the card pin (pinned_identities) — so a request can
// never poison the card/payout pin. Returns true only for a signed, pin-consistent request;
// db==nullptr => signature-only (bare test harness) and *firstContact=true. *firstContact is set
// true only on the first authenticated contact for this npk. Defined in pilot_a2a_inbox.cpp.
bool verifyInboundRequest(const QJsonObject& req, sqlite3* db, bool* firstContact = nullptr);

// Render an injection-safe, trust-tagged sender label for an owner prompt (L4). Pure function
// of the H2 verdict — NO DB access. "UNVERIFIED <npk>" when unauthenticated, "authenticated,
// first contact <npk>" on first authenticated contact, "authenticated known peer <npk>"
// otherwise. Flattens npk via a2aFlattenForPrompt. Defined in pilot_a2a_inbox.cpp.
QString a2aSenderDisplay(bool authenticated, bool firstContact, const QString& senderNpk);

// Eviction / bounding helpers (M3). External linkage so the unit tests can drive them directly.
// a2aEvictOldInboundTasks: a TTL sweep + row-cap backstop over TERMINAL inbound_tasks rows only
// (in-flight rows are never touched). Defined in pilot_a2a_inbox.cpp.
// a2aEvictDiscoveryCache: LRU-trims ONLY the heavy discovered_agents card cache to a fixed cap by
// last_seen. [FIX-B] it NEVER touches the TOFU pin tables (pinned_identities /
// pinned_request_identities) — pin permanence is the anti-impersonation property — and [FIX-E] it
// never evicts a card backing a non-terminal outbound_task (else settlement loses its payout).
// Defined in pilot_a2a.cpp. Both build their bounded DELETEs with std::to_string + the subquery
// `NOT IN (... LIMIT k)` form so they execute on stock SQLite (no SQLITE_ENABLE_UPDATE_DELETE_LIMIT).
void a2aEvictOldInboundTasks(sqlite3* db, long nowEpoch);
void a2aEvictDiscoveryCache(sqlite3* db);

// Resolve the ECIES key an OUTBOUND A2A message must be encrypted to, from the peer's
// discovered/imported Agent Card: its dedicated _logos.enc_key, else its _logos.signing_key
// (pre-split peers). Returns EMPTY when no card on file vouches for the address — callers must
// then refuse to send.
//
// It deliberately does NOT fall back to using the address itself as a key. It used to: a bare
// hex address was returned verbatim on the theory that the caller had passed an ECIES key
// directly. But a wallet VIEWING key is bare hex too (and is exactly what
// test-two-agents-docker.sh passes), so the request got encrypted to a key the peer cannot
// decrypt and published to /pilot/1/inbox-<viewing key>/proto — a channel nobody subscribes to.
// The send reported success, nothing ever arrived, and no error was raised: a silent dead-drop
// (2026-07-26). A peer becomes routable by having a card, not by looking like a key.
//
// External linkage so the unit tests can drive it directly. Defined in pilot_a2a.cpp.
std::string a2aResolveRoutingKey(sqlite3* db, const std::string& agentAddress);

// L2 — verify-before-cache guard for the discovered_agents card cache (see pilot_a2a.cpp).
// Refuses to let a non-'valid' card evict an existing row that still verifies 'valid', closing a
// forged-same-npk denial-of-payment. Returns true iff a row was written. Defined in pilot_a2a.cpp.
bool a2aCacheDiscoveredCard(sqlite3* db, const QJsonObject& card,
                            const std::string& topic, const std::string& lastSeen);

#endif  // PILOT_A2A_H
