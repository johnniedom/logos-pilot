#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct AESKey {
    std::vector<uint8_t> key;   // 32 bytes
    std::vector<uint8_t> iv;    // 12 bytes
    std::vector<uint8_t> tag;   // 16 bytes
};

AESKey generateFileKey();
std::vector<uint8_t> aesEncrypt(const std::vector<uint8_t>& plaintext, AESKey& key);
std::vector<uint8_t> aesDecrypt(const std::vector<uint8_t>& ciphertext, const AESKey& key);
std::string aesKeyToHex(const AESKey& key);
AESKey aesKeyFromHex(const std::string& hex);

struct ECIESCiphertext {
    std::vector<uint8_t> ephemeralPub;
    std::vector<uint8_t> ciphertext;
    std::vector<uint8_t> iv;
    std::vector<uint8_t> tag;
};

struct ECIESKeypair {
    std::string publicKeyHex;
    std::string privateKeyHex;
};

ECIESKeypair generateECIESKeypair();

ECIESCiphertext eciesEncrypt(const std::string& recipientNpk,
                              const std::vector<uint8_t>& plaintext);
std::vector<uint8_t> eciesDecrypt(const std::string& privateKeyHex,
                                   const ECIESCiphertext& ct);
std::string eciesSerialize(const ECIESCiphertext& ct);
ECIESCiphertext eciesDeserialize(const std::string& data);

// ECDSA over secp256k1 (same curve/key material as ECIES). Signs SHA-256 of the
// message; returns a hex-encoded DER signature. Used to sign the A2A Agent Card.
std::string signMessage(const std::vector<uint8_t>& message,
                        const std::string& privateKeyHex);
bool verifySignature(const std::vector<uint8_t>& message,
                     const std::string& signatureHex,
                     const std::string& publicKeyHex);

// M2 — at-rest secret sealing. Wrap a hex secret (e.g. the agent's ecies.priv) with a
// passphrase-derived AES-256-GCM key so pilot.db never holds the raw key when
// PILOT_KEY_PASSPHRASE is set. Format: "enc:v1:<saltHex>:<ivHex>:<tagHex>:<ctHex>".
// wrapSecret derives a 32-byte key via PBKDF2-HMAC-SHA256 (>=200000 iters) over the
// passphrase + a random 16-byte salt; the AES IV/tag come from the existing AES-GCM
// primitive. unwrapSecret THROWS std::runtime_error on a wrong passphrase, a GCM tag
// mismatch, or a malformed blob. isWrappedSecret tests only the "enc:v1:" prefix.
std::string wrapSecret(const std::string& plaintext, const std::string& passphrase);
std::string unwrapSecret(const std::string& blob, const std::string& passphrase);
bool isWrappedSecret(const std::string& s);
