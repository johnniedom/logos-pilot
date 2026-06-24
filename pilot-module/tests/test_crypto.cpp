#include <logos_test.h>
#include "../src/pilot_crypto.h"
#include <string>
#include <cstring>

LOGOS_TEST(aes_generate_key_correct_sizes) {
    AESKey k = generateFileKey();
    LOGOS_ASSERT_EQ(k.key.size(), size_t(32));
    LOGOS_ASSERT_EQ(k.iv.size(), size_t(12));
}

LOGOS_TEST(aes_encrypt_decrypt_roundtrip) {
    std::string original = "Hello, Pilot agent! This is secret data.";
    std::vector<uint8_t> plaintext(original.begin(), original.end());

    AESKey key = generateFileKey();
    std::vector<uint8_t> ciphertext = aesEncrypt(plaintext, key);

    LOGOS_ASSERT_TRUE(ciphertext.size() > 0);
    LOGOS_ASSERT_TRUE(ciphertext != plaintext);

    std::vector<uint8_t> decrypted = aesDecrypt(ciphertext, key);
    LOGOS_ASSERT_TRUE(decrypted == plaintext);

    std::string result(decrypted.begin(), decrypted.end());
    LOGOS_ASSERT_EQ(result, original);
}

LOGOS_TEST(aes_encrypt_produces_different_output) {
    std::string data = "same input data";
    std::vector<uint8_t> plaintext(data.begin(), data.end());

    AESKey key1 = generateFileKey();
    AESKey key2 = generateFileKey();
    std::vector<uint8_t> ct1 = aesEncrypt(plaintext, key1);
    std::vector<uint8_t> ct2 = aesEncrypt(plaintext, key2);

    LOGOS_ASSERT_TRUE(ct1 != ct2);
}

LOGOS_TEST(aes_decrypt_fails_with_wrong_key) {
    std::string data = "secret";
    std::vector<uint8_t> plaintext(data.begin(), data.end());

    AESKey key = generateFileKey();
    std::vector<uint8_t> ciphertext = aesEncrypt(plaintext, key);

    AESKey wrongKey = generateFileKey();
    wrongKey.tag = key.tag;
    bool threw = false;
    try {
        aesDecrypt(ciphertext, wrongKey);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    LOGOS_ASSERT_TRUE(threw);
}

LOGOS_TEST(aes_key_hex_roundtrip) {
    AESKey key = generateFileKey();
    std::vector<uint8_t> plaintext = {1, 2, 3, 4, 5};
    aesEncrypt(plaintext, key);

    std::string hex = aesKeyToHex(key);
    LOGOS_ASSERT_CONTAINS(hex, ":");

    AESKey restored = aesKeyFromHex(hex);
    LOGOS_ASSERT_TRUE(restored.key == key.key);
    LOGOS_ASSERT_TRUE(restored.iv == key.iv);
    LOGOS_ASSERT_TRUE(restored.tag == key.tag);
}

LOGOS_TEST(aes_empty_plaintext) {
    std::vector<uint8_t> plaintext;
    AESKey key = generateFileKey();
    std::vector<uint8_t> ciphertext = aesEncrypt(plaintext, key);
    std::vector<uint8_t> decrypted = aesDecrypt(ciphertext, key);
    LOGOS_ASSERT_EQ(decrypted.size(), size_t(0));
}

LOGOS_TEST(aes_large_data_roundtrip) {
    std::vector<uint8_t> plaintext(100000);
    for (size_t i = 0; i < plaintext.size(); i++)
        plaintext[i] = static_cast<uint8_t>(i % 256);

    AESKey key = generateFileKey();
    std::vector<uint8_t> ciphertext = aesEncrypt(plaintext, key);
    std::vector<uint8_t> decrypted = aesDecrypt(ciphertext, key);
    LOGOS_ASSERT_TRUE(decrypted == plaintext);
}

// --- ECDSA sign/verify (Agent Card signing, spec.md:63) ---

LOGOS_TEST(ecdsa_sign_verify_roundtrip) {
    ECIESKeypair kp = generateECIESKeypair();
    std::string canonical = "{\"name\":\"Pilot Agent\",\"version\":\"1.0.0\"}";
    std::vector<uint8_t> msg(canonical.begin(), canonical.end());

    std::string sig = signMessage(msg, kp.privateKeyHex);
    LOGOS_ASSERT_TRUE(sig.size() > 0);
    LOGOS_ASSERT_TRUE(verifySignature(msg, sig, kp.publicKeyHex));
}

LOGOS_TEST(ecdsa_verify_fails_on_tampered_card) {
    ECIESKeypair kp = generateECIESKeypair();
    std::string original = "{\"name\":\"Pilot Agent\",\"version\":\"1.0.0\"}";
    std::vector<uint8_t> msg(original.begin(), original.end());
    std::string sig = signMessage(msg, kp.privateKeyHex);

    // Flip a single value in the card bytes — signature must no longer verify.
    std::string tampered = "{\"name\":\"Pilot Agent\",\"version\":\"9.9.9\"}";
    std::vector<uint8_t> tmsg(tampered.begin(), tampered.end());
    LOGOS_ASSERT_FALSE(verifySignature(tmsg, sig, kp.publicKeyHex));
}

LOGOS_TEST(ecdsa_verify_fails_with_wrong_public_key) {
    ECIESKeypair signer = generateECIESKeypair();
    ECIESKeypair other = generateECIESKeypair();
    std::string data = "agent card bytes";
    std::vector<uint8_t> msg(data.begin(), data.end());

    std::string sig = signMessage(msg, signer.privateKeyHex);
    LOGOS_ASSERT_TRUE(verifySignature(msg, sig, signer.publicKeyHex));
    LOGOS_ASSERT_FALSE(verifySignature(msg, sig, other.publicKeyHex));
}

LOGOS_TEST(ecdsa_verify_rejects_garbage_signature) {
    ECIESKeypair kp = generateECIESKeypair();
    std::string data = "hello";
    std::vector<uint8_t> msg(data.begin(), data.end());
    LOGOS_ASSERT_FALSE(verifySignature(msg, "deadbeef", kp.publicKeyHex));
    LOGOS_ASSERT_FALSE(verifySignature(msg, "", kp.publicKeyHex));
}

LOGOS_TEST(ecdsa_sign_empty_message_roundtrip) {
    ECIESKeypair kp = generateECIESKeypair();
    std::vector<uint8_t> msg;
    std::string sig = signMessage(msg, kp.privateKeyHex);
    LOGOS_ASSERT_TRUE(verifySignature(msg, sig, kp.publicKeyHex));
}

LOGOS_TEST(ecies_serialize_deserialize_roundtrip) {
    ECIESCiphertext ct;
    ct.ephemeralPub = {0x04, 0xAA, 0xBB, 0xCC};
    ct.ciphertext = {0x01, 0x02, 0x03};
    ct.iv = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xA0, 0xB0, 0xC0};
    ct.tag = {0xDE, 0xAD, 0xBE, 0xEF};

    std::string serialized = eciesSerialize(ct);
    ECIESCiphertext restored = eciesDeserialize(serialized);

    LOGOS_ASSERT_TRUE(restored.ephemeralPub == ct.ephemeralPub);
    LOGOS_ASSERT_TRUE(restored.ciphertext == ct.ciphertext);
    LOGOS_ASSERT_TRUE(restored.iv == ct.iv);
    LOGOS_ASSERT_TRUE(restored.tag == ct.tag);
}
