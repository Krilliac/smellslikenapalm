// src/Utils/CryptoUtils.h
#pragma once

#include <string>
#include <vector>
#include <optional>

namespace CryptoUtils {

    // Compute SHA-256 hash of input data, return hex string
    std::string SHA256Hex(const std::vector<uint8_t>& data);

    // Compute HMAC-SHA256 of input data with key, return raw bytes
    std::vector<uint8_t> HMAC_SHA256(const std::vector<uint8_t>& key,
                                     const std::vector<uint8_t>& data);

    // Base64 encode/decode
    std::string Base64Encode(const std::vector<uint8_t>& data);
    std::optional<std::vector<uint8_t>> Base64Decode(const std::string& b64);

    // Generate cryptographically secure random bytes
    std::vector<uint8_t> GenerateRandomBytes(size_t length);

    // AES-256-CBC encrypt/decrypt (key:32 bytes, iv:16 bytes)
    std::optional<std::vector<uint8_t>> AESEncrypt(const std::vector<uint8_t>& key,
                                                   const std::vector<uint8_t>& iv,
                                                   const std::vector<uint8_t>& plaintext);
    std::optional<std::vector<uint8_t>> AESDecrypt(const std::vector<uint8_t>& key,
                                                   const std::vector<uint8_t>& iv,
                                                   const std::vector<uint8_t>& ciphertext);
}