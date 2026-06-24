#ifndef PILOT_A2A_H
#define PILOT_A2A_H

// Qt-bearing A2A declarations shared between pilot_a2a.cpp (definitions) and the unit tests.
// This is the SINGLE SOURCE of these prototypes so the tests no longer hand-declare them and
// drift out of sync with the definitions. It MUST NOT be included from pilot_impl.h, which is
// the pure-C++ universal interface and stays Qt-free.

#include <QString>
#include <QJsonObject>
#include <string>

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

#endif  // PILOT_A2A_H
