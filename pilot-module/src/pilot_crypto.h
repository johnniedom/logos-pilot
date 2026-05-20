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

ECIESCiphertext eciesEncrypt(const std::string& recipientNpk,
                              const std::vector<uint8_t>& plaintext);
std::vector<uint8_t> eciesDecrypt(const std::string& privateKeyHex,
                                   const ECIESCiphertext& ct);
std::string eciesSerialize(const ECIESCiphertext& ct);
ECIESCiphertext eciesDeserialize(const std::string& data);
