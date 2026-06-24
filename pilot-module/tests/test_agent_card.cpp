#include <logos_test.h>
#include "../src/pilot_crypto.h"
#include <sqlite3.h>
#include <QString>
#include <QJsonObject>
#include <QJsonDocument>
#include <QByteArray>
#include <string>
#include <vector>

// verifyCardStatus is defined in pilot_a2a.cpp (external linkage); its prototypes live in the
// single-source Qt-bearing header so the tests can no longer drift from the definitions. It
// checks the AUTHENTICITY of a received Agent Card (M3): the embedded ECDSA signature must
// verify AND be produced by the card's OWN published identity key (_logos.signing_key), so a
// card re-signed under a different key is rejected even when its signature is internally valid.
// The DB-aware overload binds the verdict to a first-contact TOFU pin. These tests pin that
// contract.
#include "../src/pilot_a2a.h"

// Build a card that publishes `identityKey` as its _logos.signing_key, then sign the
// canonical (signature-excluded) bytes with `signingPriv` and attach a signature whose
// publicKey is `sigPub`. This mirrors PilotImpl::agentCard()'s signing EXACTLY: the
// signature is computed over the compact card with _logos.signing_key already present
// and the "signature" field absent, so verifyCardStatus reproduces the same bytes.
static QJsonObject makeCard(const std::string& identityKey,
                            const std::string& signingPriv,
                            const std::string& sigPub) {
    QJsonObject logos;
    logos["npk"] = QString("npk-genuine-identity");
    logos["payout"] = QString("{\"nullifier_public_key\":\"aa\",\"viewing_public_key\":\"bb\"}");
    logos["signing_key"] = QString::fromStdString(identityKey);

    QJsonObject card;
    card["name"] = QString("Pilot Agent");
    card["version"] = QString("1.0.0");
    card["_logos"] = logos;

    std::string canonical = QJsonDocument(card).toJson(QJsonDocument::Compact).toStdString();
    std::vector<uint8_t> bytes(canonical.begin(), canonical.end());
    std::string sigHex = signMessage(bytes, signingPriv);

    QJsonObject sig;
    sig["alg"] = QString("ES256K");
    sig["publicKey"] = QString::fromStdString(sigPub);
    sig["value"] = QString::fromStdString(sigHex);
    card["signature"] = sig;
    return card;
}

// A genuine card — signed by the agent's own key, with signature.publicKey ==
// _logos.signing_key — verifies as authentic.
LOGOS_TEST(card_genuine_is_valid) {
    ECIESKeypair kp = generateECIESKeypair();
    QJsonObject card = makeCard(kp.publicKeyHex, kp.privateKeyHex, kp.publicKeyHex);
    LOGOS_ASSERT_EQ(verifyCardStatus(card).toStdString(), std::string("valid"));
}

// Impersonation (M3): the genuine agent's identity key stays published, but an attacker
// re-signs the card with their OWN key and sets signature.publicKey to that key. The
// signature is internally valid yet was NOT produced by the card's published identity
// key, so it must NOT be accepted as "valid".
LOGOS_TEST(card_impersonation_rejected) {
    ECIESKeypair genuine = generateECIESKeypair();
    ECIESKeypair attacker = generateECIESKeypair();
    QJsonObject card = makeCard(genuine.publicKeyHex,    // identity key unchanged (genuine)
                                attacker.privateKeyHex,  // signed by the attacker
                                attacker.publicKeyHex);  // signature.publicKey = attacker
    LOGOS_ASSERT_TRUE(verifyCardStatus(card).toStdString() != std::string("valid"));
    LOGOS_ASSERT_EQ(verifyCardStatus(card).toStdString(), std::string("invalid"));
}

// Tampering: flip a field AFTER signing. signature.publicKey still matches the published
// identity key, but the bytes no longer match the signature -> "invalid".
LOGOS_TEST(card_tampered_body_invalid) {
    ECIESKeypair kp = generateECIESKeypair();
    QJsonObject card = makeCard(kp.publicKeyHex, kp.privateKeyHex, kp.publicKeyHex);
    card["name"] = QString("Evil Agent");
    LOGOS_ASSERT_EQ(verifyCardStatus(card).toStdString(), std::string("invalid"));
}

// Tampering the published identity key itself (without re-signing) also fails: the
// attacker swaps _logos.signing_key to their key so signature.publicKey would have to
// change too, but then the canonical bytes change and the genuine signature no longer
// verifies. Either way the card is not "valid".
LOGOS_TEST(card_swapped_identity_key_not_valid) {
    ECIESKeypair genuine = generateECIESKeypair();
    ECIESKeypair attacker = generateECIESKeypair();
    QJsonObject card = makeCard(genuine.publicKeyHex, genuine.privateKeyHex, genuine.publicKeyHex);
    // Point both the published identity key and signature.publicKey at the attacker.
    QJsonObject logos = card["_logos"].toObject();
    logos["signing_key"] = QString::fromStdString(attacker.publicKeyHex);
    card["_logos"] = logos;
    QJsonObject sig = card["signature"].toObject();
    sig["publicKey"] = QString::fromStdString(attacker.publicKeyHex);
    card["signature"] = sig;
    LOGOS_ASSERT_TRUE(verifyCardStatus(card).toStdString() != std::string("valid"));
}

// A card with no signature is flagged "unsigned" (interop), never "valid".
LOGOS_TEST(card_unsigned_flagged) {
    QJsonObject logos; logos["signing_key"] = QString("ab12");
    QJsonObject card; card["name"] = QString("X"); card["_logos"] = logos;
    LOGOS_ASSERT_EQ(verifyCardStatus(card).toStdString(), std::string("unsigned"));
}

// TOFU identity binding (M3): the FIRST signing_key seen for an npk is PINNED. A
// from-scratch forged card that reuses that npk under a DIFFERENT (attacker) signing_key —
// internally self-consistent because the attacker signs with its OWN key and sets
// signature.publicKey to it (so the pure check alone reads 'valid') — is rejected as
// 'invalid' against the pin. The attacker cannot swap the payout under an existing identity.
LOGOS_TEST(card_forged_existing_npk_new_signing_key_rejected) {
    sqlite3* db = nullptr;
    sqlite3_open(":memory:", &db);

    ECIESKeypair genuine = generateECIESKeypair();
    ECIESKeypair attacker = generateECIESKeypair();

    // First contact pins (npk-genuine-identity -> genuine key) and reads 'valid'.
    QJsonObject genuineCard = makeCard(genuine.publicKeyHex, genuine.privateKeyHex, genuine.publicKeyHex);
    LOGOS_ASSERT_EQ(verifyCardStatus(genuineCard, db).toStdString(), std::string("valid"));

    // Forged card: SAME npk (makeCard always publishes "npk-genuine-identity"), attacker's
    // key, self-signed so the pure check would call it 'valid'...
    QJsonObject forged = makeCard(attacker.publicKeyHex, attacker.privateKeyHex, attacker.publicKeyHex);
    LOGOS_ASSERT_EQ(verifyCardStatus(forged).toStdString(), std::string("valid"));        // self-consistent
    // ...but the pin from the genuine first contact rejects the swapped signing_key.
    LOGOS_ASSERT_EQ(verifyCardStatus(forged, db).toStdString(), std::string("invalid"));

    // The genuine identity still validates against its own pin (idempotent re-pin).
    LOGOS_ASSERT_EQ(verifyCardStatus(genuineCard, db).toStdString(), std::string("valid"));

    sqlite3_close(db);
}

// An unsigned/unbound card is never 'valid' even with a DB: the base verdict passes through
// unchanged (nothing authentic to pin).
LOGOS_TEST(card_unsigned_with_db_stays_unsigned) {
    sqlite3* db = nullptr;
    sqlite3_open(":memory:", &db);
    QJsonObject logos; logos["npk"] = QString("npk-x"); logos["signing_key"] = QString("ab12");
    QJsonObject card; card["name"] = QString("X"); card["_logos"] = logos;
    LOGOS_ASSERT_EQ(verifyCardStatus(card, db).toStdString(), std::string("unsigned"));
    sqlite3_close(db);
}

// A signed card that publishes NO identity key cannot be bound to an identity, so it is
// "unbound" — explicitly NOT "valid".
LOGOS_TEST(card_without_identity_key_unbound) {
    ECIESKeypair kp = generateECIESKeypair();
    QJsonObject card;
    card["name"] = QString("Pilot Agent");
    card["version"] = QString("1.0.0");
    std::string canonical = QJsonDocument(card).toJson(QJsonDocument::Compact).toStdString();
    std::vector<uint8_t> bytes(canonical.begin(), canonical.end());
    QJsonObject sig;
    sig["alg"] = QString("ES256K");
    sig["publicKey"] = QString::fromStdString(kp.publicKeyHex);
    sig["value"] = QString::fromStdString(signMessage(bytes, kp.privateKeyHex));
    card["signature"] = sig;
    LOGOS_ASSERT_EQ(verifyCardStatus(card).toStdString(), std::string("unbound"));
}
