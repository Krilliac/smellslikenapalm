// src/Security/PasswordHasher.h
//
// Salted password hashing + verification for admin / RCON credentials.
//
// Storage format (single self-describing string):
//
//     pbkdf2$<iterations>$<base64-salt>$<base64-derived-key>
//
// When OpenSSL is available at build time (RS2V_HAS_OPENSSL, mirrored from
// src/Utils/CryptoUtils.cpp) the derived key is produced with PBKDF2-HMAC-SHA256.
// When OpenSSL is NOT available we fall back to an iterated HMAC-SHA256 stretch
// built on a portable SHA-256 implementation; the on-disk format is identical so
// hashes remain verifiable across builds with the same algorithm tag.
//
// Verify() compares the derived keys in constant time to avoid leaking match
// progress through timing.

#pragma once

#include <cstdint>
#include <string>

class PasswordHasher {
public:
    // Hash a plaintext password. A fresh random salt is generated per call, so
    // Hash(pw) != Hash(pw) — that's expected; Verify() recovers the salt from
    // the encoded string.
    static std::string Hash(const std::string& password);

    // Hash with an explicit iteration count (mainly for testing / tuning).
    static std::string Hash(const std::string& password, uint32_t iterations);

    // Verify a plaintext password against a previously produced encoded hash.
    // Returns false on any malformed input rather than throwing.
    static bool Verify(const std::string& password, const std::string& encodedHash);

    // Default PBKDF2 iteration count used by Hash(password).
    static constexpr uint32_t kDefaultIterations = 100000;

    // Length in bytes of the random salt and of the derived key.
    static constexpr uint32_t kSaltLength = 16;
    static constexpr uint32_t kKeyLength  = 32;
};
