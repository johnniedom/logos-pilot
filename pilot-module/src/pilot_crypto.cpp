#include "pilot_crypto.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ec.h>
#include <openssl/sha.h>
#include <openssl/err.h>
#include <openssl/core_names.h>
#include <openssl/param_build.h>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <cstring>

static std::string bytesToHex(const std::vector<uint8_t>& data) {
    std::ostringstream ss;
    for (auto b : data)
        ss << std::hex << std::setfill('0') << std::setw(2) << (int)b;
    return ss.str();
}

static bool isHexString(const std::string& s) {
    if (s.empty() || s.size() % 2 != 0) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    return true;
}

static std::vector<uint8_t> hexToBytes(const std::string& hex) {
    if (!isHexString(hex)) return {};
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        uint8_t byte = static_cast<uint8_t>(
            std::stoi(hex.substr(i, 2), nullptr, 16));
        out.push_back(byte);
    }
    return out;
}

AESKey generateFileKey() {
    AESKey k;
    k.key.resize(32);
    k.iv.resize(12);
    k.tag.resize(16);
    RAND_bytes(k.key.data(), 32);
    RAND_bytes(k.iv.data(), 12);
    return k;
}

std::vector<uint8_t> aesEncrypt(const std::vector<uint8_t>& plaintext, AESKey& key) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");

    std::vector<uint8_t> ciphertext(plaintext.size() + 16);
    int len = 0, ciphertextLen = 0;

    EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr);
    EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.key.data(), key.iv.data());
    EVP_EncryptUpdate(ctx, ciphertext.data(), &len,
                      plaintext.data(), static_cast<int>(plaintext.size()));
    ciphertextLen = len;
    EVP_EncryptFinal_ex(ctx, ciphertext.data() + len, &len);
    ciphertextLen += len;

    key.tag.resize(16);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, key.tag.data());
    EVP_CIPHER_CTX_free(ctx);

    ciphertext.resize(ciphertextLen);
    return ciphertext;
}

std::vector<uint8_t> aesDecrypt(const std::vector<uint8_t>& ciphertext, const AESKey& key) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");

    std::vector<uint8_t> plaintext(ciphertext.size());
    int len = 0, plaintextLen = 0;

    EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr);
    EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.key.data(), key.iv.data());
    EVP_DecryptUpdate(ctx, plaintext.data(), &len,
                      ciphertext.data(), static_cast<int>(ciphertext.size()));
    plaintextLen = len;

    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16,
                        const_cast<uint8_t*>(key.tag.data()));

    int ret = EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &len);
    EVP_CIPHER_CTX_free(ctx);

    if (ret <= 0) throw std::runtime_error("AES-GCM decryption failed (tag mismatch)");
    plaintextLen += len;
    plaintext.resize(plaintextLen);
    return plaintext;
}

std::string aesKeyToHex(const AESKey& key) {
    return bytesToHex(key.key) + ":" + bytesToHex(key.iv) + ":" + bytesToHex(key.tag);
}

AESKey aesKeyFromHex(const std::string& hex) {
    AESKey k;
    size_t p1 = hex.find(':');
    size_t p2 = hex.find(':', p1 + 1);
    if (p1 == std::string::npos || p2 == std::string::npos)
        throw std::runtime_error("Invalid AES key hex format");
    k.key = hexToBytes(hex.substr(0, p1));
    k.iv = hexToBytes(hex.substr(p1 + 1, p2 - p1 - 1));
    k.tag = hexToBytes(hex.substr(p2 + 1));
    return k;
}

// ECIES: ephemeral ECDH (secp256k1) + SHA256 KDF + AES-256-GCM

ECIESKeypair generateECIESKeypair() {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    EVP_PKEY_keygen_init(ctx);
    EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, NID_secp256k1);
    EVP_PKEY* key = nullptr;
    EVP_PKEY_keygen(ctx, &key);
    EVP_PKEY_CTX_free(ctx);

    size_t pubLen = 0;
    EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_PUB_KEY, nullptr, 0, &pubLen);
    std::vector<uint8_t> pubBytes(pubLen);
    EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_PUB_KEY, pubBytes.data(), pubBytes.size(), &pubLen);

    BIGNUM* privBn = nullptr;
    EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_PRIV_KEY, &privBn);
    std::vector<uint8_t> privBytes(BN_num_bytes(privBn));
    BN_bn2bin(privBn, privBytes.data());
    BN_free(privBn);

    EVP_PKEY_free(key);
    return {bytesToHex(pubBytes), bytesToHex(privBytes)};
}

ECIESCiphertext eciesEncrypt(const std::string& recipientNpk,
                              const std::vector<uint8_t>& plaintext) {
    std::vector<uint8_t> recipientPub = hexToBytes(recipientNpk);
    if (recipientPub.empty())
        throw std::invalid_argument("invalid recipient key: not a hex string");

    EVP_PKEY_CTX* paramCtx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    EVP_PKEY_keygen_init(paramCtx);
    EVP_PKEY_CTX_set_ec_paramgen_curve_nid(paramCtx, NID_secp256k1);
    EVP_PKEY* ephKey = nullptr;
    EVP_PKEY_keygen(paramCtx, &ephKey);
    EVP_PKEY_CTX_free(paramCtx);

    // Extract ephemeral public key
    size_t ephPubLen = 0;
    EVP_PKEY_get_octet_string_param(ephKey, OSSL_PKEY_PARAM_PUB_KEY,
                                     nullptr, 0, &ephPubLen);
    std::vector<uint8_t> ephPub(ephPubLen);
    EVP_PKEY_get_octet_string_param(ephKey, OSSL_PKEY_PARAM_PUB_KEY,
                                     ephPub.data(), ephPub.size(), &ephPubLen);

    // Load recipient public key
    OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
    OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, "secp256k1", 0);
    OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY,
                                      recipientPub.data(), recipientPub.size());
    OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(bld);
    EVP_PKEY_CTX* fromCtx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
    EVP_PKEY_fromdata_init(fromCtx);
    EVP_PKEY* peerKey = nullptr;
    EVP_PKEY_fromdata(fromCtx, &peerKey, EVP_PKEY_PUBLIC_KEY, params);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(bld);
    EVP_PKEY_CTX_free(fromCtx);

    // ECDH derive shared secret
    EVP_PKEY_CTX* deriveCtx = EVP_PKEY_CTX_new(ephKey, nullptr);
    EVP_PKEY_derive_init(deriveCtx);
    EVP_PKEY_derive_set_peer(deriveCtx, peerKey);
    size_t secretLen = 0;
    EVP_PKEY_derive(deriveCtx, nullptr, &secretLen);
    std::vector<uint8_t> secret(secretLen);
    EVP_PKEY_derive(deriveCtx, secret.data(), &secretLen);
    EVP_PKEY_CTX_free(deriveCtx);

    // SHA256 the shared secret to get AES key
    uint8_t derived[32];
    SHA256(secret.data(), secretLen, derived);

    AESKey aesKey;
    aesKey.key.assign(derived, derived + 32);
    aesKey.iv.resize(12);
    RAND_bytes(aesKey.iv.data(), 12);

    std::vector<uint8_t> ct;
    try {
        ct = aesEncrypt(plaintext, aesKey);
    } catch (...) {
        EVP_PKEY_free(ephKey);
        EVP_PKEY_free(peerKey);
        throw;
    }

    EVP_PKEY_free(ephKey);
    EVP_PKEY_free(peerKey);

    return {ephPub, ct, aesKey.iv, aesKey.tag};
}

std::vector<uint8_t> eciesDecrypt(const std::string& privateKeyHex,
                                   const ECIESCiphertext& ct) {
    std::vector<uint8_t> privBytes = hexToBytes(privateKeyHex);

    // Load private key
    BIGNUM* privBn = BN_bin2bn(privBytes.data(), static_cast<int>(privBytes.size()), nullptr);
    if (!privBn) throw std::runtime_error("BN_bin2bn failed");

    OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
    OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, "secp256k1", 0);
    OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_PRIV_KEY, privBn);
    OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(bld);

    EVP_PKEY_CTX* fromCtx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
    EVP_PKEY_fromdata_init(fromCtx);
    EVP_PKEY* myKey = nullptr;
    EVP_PKEY_fromdata(fromCtx, &myKey, EVP_PKEY_KEYPAIR, params);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(bld);
    EVP_PKEY_CTX_free(fromCtx);
    BN_free(privBn);

    // Load ephemeral public key
    OSSL_PARAM_BLD* bld2 = OSSL_PARAM_BLD_new();
    OSSL_PARAM_BLD_push_utf8_string(bld2, OSSL_PKEY_PARAM_GROUP_NAME, "secp256k1", 0);
    OSSL_PARAM_BLD_push_octet_string(bld2, OSSL_PKEY_PARAM_PUB_KEY,
                                      ct.ephemeralPub.data(), ct.ephemeralPub.size());
    OSSL_PARAM* params2 = OSSL_PARAM_BLD_to_param(bld2);
    EVP_PKEY_CTX* fromCtx2 = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
    EVP_PKEY_fromdata_init(fromCtx2);
    EVP_PKEY* ephKey = nullptr;
    EVP_PKEY_fromdata(fromCtx2, &ephKey, EVP_PKEY_PUBLIC_KEY, params2);
    OSSL_PARAM_free(params2);
    OSSL_PARAM_BLD_free(bld2);
    EVP_PKEY_CTX_free(fromCtx2);

    // ECDH
    EVP_PKEY_CTX* deriveCtx = EVP_PKEY_CTX_new(myKey, nullptr);
    EVP_PKEY_derive_init(deriveCtx);
    EVP_PKEY_derive_set_peer(deriveCtx, ephKey);
    size_t secretLen = 0;
    EVP_PKEY_derive(deriveCtx, nullptr, &secretLen);
    std::vector<uint8_t> secret(secretLen);
    EVP_PKEY_derive(deriveCtx, secret.data(), &secretLen);
    EVP_PKEY_CTX_free(deriveCtx);

    uint8_t derived[32];
    SHA256(secret.data(), secretLen, derived);

    AESKey aesKey;
    aesKey.key.assign(derived, derived + 32);
    aesKey.iv = ct.iv;
    aesKey.tag = ct.tag;

    std::vector<uint8_t> result;
    try {
        result = aesDecrypt(ct.ciphertext, aesKey);
    } catch (...) {
        EVP_PKEY_free(myKey);
        EVP_PKEY_free(ephKey);
        throw;
    }

    EVP_PKEY_free(myKey);
    EVP_PKEY_free(ephKey);

    return result;
}

// ECDSA over secp256k1: SHA-256 digest + DER signature, hex-encoded.
// Reuses the agent's existing ECIES key material (same curve).

std::string signMessage(const std::vector<uint8_t>& message,
                        const std::string& privateKeyHex) {
    std::vector<uint8_t> privBytes = hexToBytes(privateKeyHex);
    if (privBytes.empty())
        throw std::invalid_argument("invalid private key: not a hex string");

    BIGNUM* privBn = BN_bin2bn(privBytes.data(), static_cast<int>(privBytes.size()), nullptr);
    if (!privBn) throw std::runtime_error("BN_bin2bn failed");

    OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
    OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, "secp256k1", 0);
    OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_PRIV_KEY, privBn);
    OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(bld);

    EVP_PKEY_CTX* fromCtx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
    EVP_PKEY_fromdata_init(fromCtx);
    EVP_PKEY* pkey = nullptr;
    EVP_PKEY_fromdata(fromCtx, &pkey, EVP_PKEY_KEYPAIR, params);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(bld);
    EVP_PKEY_CTX_free(fromCtx);
    BN_free(privBn);
    if (!pkey) throw std::runtime_error("failed to load private key for signing");

    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    std::vector<uint8_t> sig;
    try {
        if (!mdctx) throw std::runtime_error("EVP_MD_CTX_new failed");
        if (EVP_DigestSignInit(mdctx, nullptr, EVP_sha256(), nullptr, pkey) != 1)
            throw std::runtime_error("EVP_DigestSignInit failed");
        size_t sigLen = 0;
        if (EVP_DigestSign(mdctx, nullptr, &sigLen,
                           message.data(), message.size()) != 1)
            throw std::runtime_error("EVP_DigestSign (size probe) failed");
        sig.resize(sigLen);
        if (EVP_DigestSign(mdctx, sig.data(), &sigLen,
                           message.data(), message.size()) != 1)
            throw std::runtime_error("EVP_DigestSign failed");
        sig.resize(sigLen);
    } catch (...) {
        EVP_MD_CTX_free(mdctx);
        EVP_PKEY_free(pkey);
        throw;
    }
    EVP_MD_CTX_free(mdctx);
    EVP_PKEY_free(pkey);
    return bytesToHex(sig);
}

bool verifySignature(const std::vector<uint8_t>& message,
                     const std::string& signatureHex,
                     const std::string& publicKeyHex) {
    std::vector<uint8_t> sig = hexToBytes(signatureHex);
    std::vector<uint8_t> pubBytes = hexToBytes(publicKeyHex);
    if (sig.empty() || pubBytes.empty()) return false;

    OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
    OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, "secp256k1", 0);
    OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY,
                                      pubBytes.data(), pubBytes.size());
    OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(bld);
    EVP_PKEY_CTX* fromCtx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
    EVP_PKEY_fromdata_init(fromCtx);
    EVP_PKEY* pkey = nullptr;
    EVP_PKEY_fromdata(fromCtx, &pkey, EVP_PKEY_PUBLIC_KEY, params);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(bld);
    EVP_PKEY_CTX_free(fromCtx);
    if (!pkey) return false;

    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    bool ok = false;
    if (mdctx &&
        EVP_DigestVerifyInit(mdctx, nullptr, EVP_sha256(), nullptr, pkey) == 1) {
        int rc = EVP_DigestVerify(mdctx, sig.data(), sig.size(),
                                  message.data(), message.size());
        ok = (rc == 1);
    }
    EVP_MD_CTX_free(mdctx);
    EVP_PKEY_free(pkey);
    return ok;
}

std::string eciesSerialize(const ECIESCiphertext& ct) {
    return bytesToHex(ct.ephemeralPub) + ":" +
           bytesToHex(ct.ciphertext) + ":" +
           bytesToHex(ct.iv) + ":" +
           bytesToHex(ct.tag);
}

ECIESCiphertext eciesDeserialize(const std::string& data) {
    ECIESCiphertext ct;
    size_t p1 = data.find(':');
    size_t p2 = data.find(':', p1 + 1);
    size_t p3 = data.find(':', p2 + 1);
    if (p1 == std::string::npos || p2 == std::string::npos || p3 == std::string::npos)
        throw std::runtime_error("Invalid ECIES serialized format");
    ct.ephemeralPub = hexToBytes(data.substr(0, p1));
    ct.ciphertext = hexToBytes(data.substr(p1 + 1, p2 - p1 - 1));
    ct.iv = hexToBytes(data.substr(p2 + 1, p3 - p2 - 1));
    ct.tag = hexToBytes(data.substr(p3 + 1));
    return ct;
}

// ===== M2: passphrase-wrapped secret-at-rest (PBKDF2-HMAC-SHA256 + AES-256-GCM) =====
//
// Sealed form: "enc:v1:<saltHex>:<ivHex>:<tagHex>:<ctHex>". The 32-byte AES key is
// derived from the passphrase + a fresh random 16-byte salt; the IV (12B) and GCM tag
// (16B) come straight from the existing AESKey/aesEncrypt primitive — aesEncrypt uses
// the caller-supplied AESKey.iv and WRITES the authentication tag back into AESKey.tag
// (it does NOT generate the IV itself), exactly as generateFileKey()/eciesEncrypt rely
// on. aesDecrypt reads AESKey.{key,iv,tag} and throws std::runtime_error on a tag
// mismatch — so a wrong passphrase (different derived key) surfaces as that same throw.

static const int kPbkdf2Iters = 200000;

static std::vector<uint8_t> deriveWrapKey(const std::string& passphrase,
                                          const std::vector<uint8_t>& salt) {
    std::vector<uint8_t> key(32);
    if (PKCS5_PBKDF2_HMAC(passphrase.data(), static_cast<int>(passphrase.size()),
                          salt.data(), static_cast<int>(salt.size()),
                          kPbkdf2Iters, EVP_sha256(),
                          static_cast<int>(key.size()), key.data()) != 1)
        throw std::runtime_error("PBKDF2 key derivation failed");
    return key;
}

bool isWrappedSecret(const std::string& s) {
    return s.rfind("enc:v1:", 0) == 0;
}

std::string wrapSecret(const std::string& plaintext, const std::string& passphrase) {
    std::vector<uint8_t> salt(16);
    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1)
        throw std::runtime_error("RAND_bytes (salt) failed");

    AESKey key;
    key.key = deriveWrapKey(passphrase, salt);   // 32 bytes
    key.iv.resize(12);
    if (RAND_bytes(key.iv.data(), 12) != 1)
        throw std::runtime_error("RAND_bytes (iv) failed");

    std::vector<uint8_t> pt(plaintext.begin(), plaintext.end());
    std::vector<uint8_t> ct = aesEncrypt(pt, key);   // fills key.tag (16 bytes)

    return "enc:v1:" + bytesToHex(salt) + ":" + bytesToHex(key.iv) + ":" +
           bytesToHex(key.tag) + ":" + bytesToHex(ct);
}

std::string unwrapSecret(const std::string& blob, const std::string& passphrase) {
    if (!isWrappedSecret(blob))
        throw std::runtime_error("unwrapSecret: not an enc:v1 blob");

    std::string body = blob.substr(7);   // strip "enc:v1:"
    size_t p1 = body.find(':');
    size_t p2 = (p1 == std::string::npos) ? std::string::npos : body.find(':', p1 + 1);
    size_t p3 = (p2 == std::string::npos) ? std::string::npos : body.find(':', p2 + 1);
    if (p1 == std::string::npos || p2 == std::string::npos || p3 == std::string::npos)
        throw std::runtime_error("unwrapSecret: malformed blob");

    std::vector<uint8_t> salt = hexToBytes(body.substr(0, p1));
    std::vector<uint8_t> iv   = hexToBytes(body.substr(p1 + 1, p2 - p1 - 1));
    std::vector<uint8_t> tag  = hexToBytes(body.substr(p2 + 1, p3 - p2 - 1));
    std::vector<uint8_t> ct   = hexToBytes(body.substr(p3 + 1));
    if (salt.empty() || iv.size() != 12 || tag.size() != 16)
        throw std::runtime_error("unwrapSecret: malformed blob fields");

    AESKey key;
    key.key = deriveWrapKey(passphrase, salt);   // wrong passphrase -> wrong key -> tag fails
    key.iv = iv;
    key.tag = tag;

    std::vector<uint8_t> pt = aesDecrypt(ct, key);   // throws on GCM tag mismatch
    return std::string(pt.begin(), pt.end());
}
