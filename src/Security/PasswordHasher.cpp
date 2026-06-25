// src/Security/PasswordHasher.cpp

#include "Security/PasswordHasher.h"
#include "Utils/CryptoUtils.h"
#include "Utils/Logger.h"

#include <cstdint>
#include <cstring>
#include <vector>
#include <array>
#include <string>
#include <sstream>

#ifdef RS2V_HAS_OPENSSL
#include <openssl/evp.h>
#include <openssl/sha.h>
#endif

namespace {

// ---------------------------------------------------------------------------
// Portable SHA-256 (used only when OpenSSL is unavailable). Public-domain style
// reference implementation; correctness is what matters here, not speed.
// ---------------------------------------------------------------------------
#ifndef RS2V_HAS_OPENSSL
class Sha256 {
public:
    static constexpr size_t DIGEST_SIZE = 32;

    Sha256() { reset(); }

    void reset() {
        m_len = 0;
        m_h[0] = 0x6a09e667; m_h[1] = 0xbb67ae85;
        m_h[2] = 0x3c6ef372; m_h[3] = 0xa54ff53a;
        m_h[4] = 0x510e527f; m_h[5] = 0x9b05688c;
        m_h[6] = 0x1f83d9ab; m_h[7] = 0x5be0cd19;
        m_bufLen = 0;
    }

    void update(const uint8_t* data, size_t len) {
        m_len += len;
        while (len > 0) {
            size_t take = 64 - m_bufLen;
            if (take > len) take = len;
            std::memcpy(m_buf + m_bufLen, data, take);
            m_bufLen += take;
            data += take;
            len -= take;
            if (m_bufLen == 64) {
                process(m_buf);
                m_bufLen = 0;
            }
        }
    }

    void final(uint8_t out[DIGEST_SIZE]) {
        uint64_t bitLen = m_len * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0x00;
        while (m_bufLen != 56) {
            update(&zero, 1);
        }
        uint8_t lenBytes[8];
        for (int i = 0; i < 8; ++i) {
            lenBytes[7 - i] = static_cast<uint8_t>(bitLen >> (i * 8));
        }
        update(lenBytes, 8);
        for (int i = 0; i < 8; ++i) {
            out[i * 4 + 0] = static_cast<uint8_t>(m_h[i] >> 24);
            out[i * 4 + 1] = static_cast<uint8_t>(m_h[i] >> 16);
            out[i * 4 + 2] = static_cast<uint8_t>(m_h[i] >> 8);
            out[i * 4 + 3] = static_cast<uint8_t>(m_h[i]);
        }
    }

private:
    static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

    void process(const uint8_t* block) {
        static const uint32_t k[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
        };
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
                   (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = m_h[0], b = m_h[1], c = m_h[2], d = m_h[3];
        uint32_t e = m_h[4], f = m_h[5], g = m_h[6], h = m_h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t t1 = h + S1 + ch + k[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        m_h[0] += a; m_h[1] += b; m_h[2] += c; m_h[3] += d;
        m_h[4] += e; m_h[5] += f; m_h[6] += g; m_h[7] += h;
    }

    uint32_t m_h[8];
    uint64_t m_len;
    uint8_t  m_buf[64];
    size_t   m_bufLen;
};

std::array<uint8_t, 32> hmacSha256(const uint8_t* key, size_t keyLen,
                                   const uint8_t* msg, size_t msgLen) {
    uint8_t k0[64];
    std::memset(k0, 0, sizeof(k0));
    if (keyLen > 64) {
        Sha256 sh;
        sh.update(key, keyLen);
        uint8_t digest[32];
        sh.final(digest);
        std::memcpy(k0, digest, 32);
    } else {
        std::memcpy(k0, key, keyLen);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = k0[i] ^ 0x36;
        opad[i] = k0[i] ^ 0x5c;
    }
    uint8_t inner[32];
    {
        Sha256 sh;
        sh.update(ipad, 64);
        sh.update(msg, msgLen);
        sh.final(inner);
    }
    std::array<uint8_t, 32> out;
    {
        Sha256 sh;
        sh.update(opad, 64);
        sh.update(inner, 32);
        sh.final(out.data());
    }
    return out;
}

// PBKDF2-HMAC-SHA256 over the portable HMAC above (single 32-byte block: dkLen
// == hLen, so block index 1 is sufficient).
std::vector<uint8_t> pbkdf2_portable(const std::string& password,
                                     const std::vector<uint8_t>& salt,
                                     uint32_t iterations,
                                     uint32_t dkLen) {
    std::vector<uint8_t> result;
    result.reserve(dkLen);
    uint32_t blockIndex = 1;
    while (result.size() < dkLen) {
        // U1 = HMAC(password, salt || INT_32_BE(blockIndex))
        std::vector<uint8_t> msg = salt;
        msg.push_back(static_cast<uint8_t>(blockIndex >> 24));
        msg.push_back(static_cast<uint8_t>(blockIndex >> 16));
        msg.push_back(static_cast<uint8_t>(blockIndex >> 8));
        msg.push_back(static_cast<uint8_t>(blockIndex));

        std::array<uint8_t, 32> u = hmacSha256(
            reinterpret_cast<const uint8_t*>(password.data()), password.size(),
            msg.data(), msg.size());
        std::array<uint8_t, 32> t = u;
        for (uint32_t i = 1; i < iterations; ++i) {
            u = hmacSha256(reinterpret_cast<const uint8_t*>(password.data()), password.size(),
                           u.data(), u.size());
            for (int j = 0; j < 32; ++j) t[j] ^= u[j];
        }
        for (int j = 0; j < 32 && result.size() < dkLen; ++j) {
            result.push_back(t[j]);
        }
        ++blockIndex;
    }
    result.resize(dkLen);
    return result;
}
#endif // !RS2V_HAS_OPENSSL

// Derive key via PBKDF2-HMAC-SHA256, using OpenSSL when available.
std::vector<uint8_t> derive(const std::string& password,
                            const std::vector<uint8_t>& salt,
                            uint32_t iterations,
                            uint32_t dkLen) {
#ifdef RS2V_HAS_OPENSSL
    std::vector<uint8_t> out(dkLen);
    int rc = PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()),
                               salt.data(), static_cast<int>(salt.size()),
                               static_cast<int>(iterations), EVP_sha256(),
                               static_cast<int>(dkLen), out.data());
    if (rc != 1) {
        Logger::Error("[PasswordHasher::derive] PKCS5_PBKDF2_HMAC failed (rc=%d)", rc);
        return {};
    }
    return out;
#else
    return pbkdf2_portable(password, salt, iterations, dkLen);
#endif
}

// Constant-time byte comparison.
bool constantTimeEquals(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.size() != b.size()) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    }
    return diff == 0;
}

// Split helper on '$'.
std::vector<std::string> splitDollar(const std::string& s) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : s) {
        if (c == '$') { parts.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    parts.push_back(cur);
    return parts;
}

} // namespace

std::string PasswordHasher::Hash(const std::string& password) {
    return Hash(password, kDefaultIterations);
}

std::string PasswordHasher::Hash(const std::string& password, uint32_t iterations) {
    Logger::Trace("[PasswordHasher::Hash] Entry: password.len=%zu, iterations=%u",
                  password.size(), iterations);

    if (iterations == 0) iterations = kDefaultIterations;

    std::vector<uint8_t> salt = CryptoUtils::GenerateRandomBytes(kSaltLength);
    if (salt.size() != kSaltLength) {
        // GenerateRandomBytes always sizes to request even on the fallback path,
        // but guard anyway.
        salt.resize(kSaltLength, 0);
    }

    std::vector<uint8_t> dk = derive(password, salt, iterations, kKeyLength);
    if (dk.empty()) {
        Logger::Error("[PasswordHasher::Hash] Key derivation produced no output");
        return std::string();
    }

    std::ostringstream oss;
    oss << "pbkdf2$" << iterations << "$"
        << CryptoUtils::Base64Encode(salt) << "$"
        << CryptoUtils::Base64Encode(dk);

    std::string encoded = oss.str();
    Logger::Debug("[PasswordHasher::Hash] Produced encoded hash of length %zu", encoded.size());
    Logger::Trace("[PasswordHasher::Hash] Exit");
    return encoded;
}

bool PasswordHasher::Verify(const std::string& password, const std::string& encodedHash) {
    Logger::Trace("[PasswordHasher::Verify] Entry: encodedHash.len=%zu", encodedHash.size());

    auto parts = splitDollar(encodedHash);
    if (parts.size() != 4 || parts[0] != "pbkdf2") {
        Logger::Warn("[PasswordHasher::Verify] Malformed encoded hash (expected pbkdf2$iter$salt$key)");
        return false;
    }

    uint32_t iterations = 0;
    try {
        unsigned long v = std::stoul(parts[1]);
        iterations = static_cast<uint32_t>(v);
    } catch (...) {
        Logger::Warn("[PasswordHasher::Verify] Invalid iteration count field");
        return false;
    }
    if (iterations == 0) {
        Logger::Warn("[PasswordHasher::Verify] Iteration count is zero");
        return false;
    }

    auto saltOpt = CryptoUtils::Base64Decode(parts[2]);
    auto keyOpt  = CryptoUtils::Base64Decode(parts[3]);
    if (!saltOpt.has_value() || !keyOpt.has_value()) {
        Logger::Warn("[PasswordHasher::Verify] Failed to base64-decode salt or key");
        return false;
    }

    std::vector<uint8_t> derived = derive(password, *saltOpt, iterations,
                                          static_cast<uint32_t>(keyOpt->size()));
    if (derived.empty()) {
        Logger::Error("[PasswordHasher::Verify] Key derivation failed during verify");
        return false;
    }

    bool ok = constantTimeEquals(derived, *keyOpt);
    Logger::Debug("[PasswordHasher::Verify] Verification %s", ok ? "succeeded" : "failed");
    Logger::Trace("[PasswordHasher::Verify] Exit: return %d", ok);
    return ok;
}
