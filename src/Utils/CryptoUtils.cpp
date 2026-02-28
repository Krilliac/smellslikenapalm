// src/Utils/CryptoUtils.cpp
#include "Utils/CryptoUtils.h"
#include "Utils/Logger.h"
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#include <openssl/bio.h>
#include <cstring>

namespace CryptoUtils {

static std::string toHex(const uint8_t* data, size_t len) {
    Logger::Trace("[CryptoUtils::toHex] Entry: data=%p, len=%zu", (const void*)data, len);
    static const char* hexDigits = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = data[i];
        out.push_back(hexDigits[b >> 4]);
        out.push_back(hexDigits[b & 0xF]);
    }
    Logger::Trace("[CryptoUtils::toHex] Exit: produced hex string of length %zu", out.size());
    return out;
}

std::string SHA256Hex(const std::vector<uint8_t>& data) {
    Logger::Trace("[CryptoUtils::SHA256Hex] Entry: data.size()=%zu", data.size());
    uint8_t hash[SHA256_DIGEST_LENGTH];
    SHA256(data.data(), data.size(), hash);
    Logger::Debug("[CryptoUtils::SHA256Hex] SHA256 hash computed successfully for %zu bytes of input", data.size());
    std::string result = toHex(hash, SHA256_DIGEST_LENGTH);
    Logger::Info("[CryptoUtils::SHA256Hex] SHA256 hash generated: %s", result.c_str());
    Logger::Trace("[CryptoUtils::SHA256Hex] Exit: returning hex string of length %zu", result.size());
    return result;
}

std::vector<uint8_t> HMAC_SHA256(const std::vector<uint8_t>& key,
                                 const std::vector<uint8_t>& data) {
    Logger::Trace("[CryptoUtils::HMAC_SHA256] Entry: key.size()=%zu, data.size()=%zu", key.size(), data.size());
    unsigned int len = EVP_MAX_MD_SIZE;
    std::vector<uint8_t> out(len);
    Logger::Debug("[CryptoUtils::HMAC_SHA256] Invoking HMAC with EVP_sha256, key length=%zu, data length=%zu", key.size(), data.size());
    HMAC(EVP_sha256(), key.data(), (int)key.size(),
         data.data(), data.size(), out.data(), &len);
    out.resize(len);
    Logger::Info("[CryptoUtils::HMAC_SHA256] HMAC-SHA256 computed successfully, output length=%u", len);
    Logger::Trace("[CryptoUtils::HMAC_SHA256] Exit: returning vector of size %zu", out.size());
    return out;
}

std::string Base64Encode(const std::vector<uint8_t>& data) {
    Logger::Trace("[CryptoUtils::Base64Encode] Entry: data.size()=%zu", data.size());
    BIO* b64 = BIO_new(BIO_f_base64());
    if (!b64) {
        Logger::Error("[CryptoUtils::Base64Encode] Failed to create BIO base64 filter");
        return "";
    }
    BIO* bmem = BIO_new(BIO_s_mem());
    if (!bmem) {
        Logger::Error("[CryptoUtils::Base64Encode] Failed to create BIO memory sink");
        BIO_free(b64);
        return "";
    }
    b64 = BIO_push(b64, bmem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    Logger::Debug("[CryptoUtils::Base64Encode] BIO chain created, writing %zu bytes", data.size());
    BIO_write(b64, data.data(), data.size());
    BIO_flush(b64);
    BUF_MEM* bptr;
    BIO_get_mem_ptr(b64, &bptr);
    std::string out(bptr->data, bptr->length);
    BIO_free_all(b64);
    Logger::Info("[CryptoUtils::Base64Encode] Base64 encoding complete: input=%zu bytes, output=%zu chars", data.size(), out.size());
    Logger::Trace("[CryptoUtils::Base64Encode] Exit: returning encoded string of length %zu", out.size());
    return out;
}

std::optional<std::vector<uint8_t>> Base64Decode(const std::string& b64) {
    Logger::Trace("[CryptoUtils::Base64Decode] Entry: b64.size()=%zu", b64.size());
    BIO* bmem = BIO_new_mem_buf(b64.data(), b64.size());
    if (!bmem) {
        Logger::Error("[CryptoUtils::Base64Decode] Failed to create BIO memory buffer from input");
        return std::nullopt;
    }
    BIO* b64f = BIO_new(BIO_f_base64());
    if (!b64f) {
        Logger::Error("[CryptoUtils::Base64Decode] Failed to create BIO base64 filter");
        BIO_free(bmem);
        return std::nullopt;
    }
    bmem = BIO_push(b64f, bmem);
    BIO_set_flags(bmem, BIO_FLAGS_BASE64_NO_NL);
    std::vector<uint8_t> out(b64.size());
    Logger::Debug("[CryptoUtils::Base64Decode] BIO chain created, reading decoded data");
    int len = BIO_read(bmem, out.data(), out.size());
    BIO_free_all(bmem);
    if (len <= 0) {
        Logger::Error("[CryptoUtils::Base64Decode] BIO_read failed or returned 0 bytes, len=%d", len);
        Logger::Trace("[CryptoUtils::Base64Decode] Exit: returning std::nullopt");
        return std::nullopt;
    }
    out.resize(len);
    Logger::Info("[CryptoUtils::Base64Decode] Base64 decoding complete: input=%zu chars, output=%d bytes", b64.size(), len);
    Logger::Trace("[CryptoUtils::Base64Decode] Exit: returning decoded vector of size %d", len);
    return out;
}

std::vector<uint8_t> GenerateRandomBytes(size_t length) {
    Logger::Trace("[CryptoUtils::GenerateRandomBytes] Entry: length=%zu", length);
    std::vector<uint8_t> out(length);
    int rc = RAND_bytes(out.data(), length);
    if (rc != 1) {
        Logger::Error("[CryptoUtils::GenerateRandomBytes] RAND_bytes failed, rc=%d, requested length=%zu", rc, length);
    } else {
        Logger::Info("[CryptoUtils::GenerateRandomBytes] Successfully generated %zu random bytes", length);
    }
    Logger::Trace("[CryptoUtils::GenerateRandomBytes] Exit: returning vector of size %zu", out.size());
    return out;
}

static std::optional<std::vector<uint8_t>> aesCrypt(const std::vector<uint8_t>& key,
                                                    const std::vector<uint8_t>& iv,
                                                    const std::vector<uint8_t>& in,
                                                    bool encrypt) {
    Logger::Trace("[CryptoUtils::aesCrypt] Entry: key.size()=%zu, iv.size()=%zu, in.size()=%zu, encrypt=%s",
                  key.size(), iv.size(), in.size(), encrypt ? "true" : "false");
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        Logger::Error("[CryptoUtils::aesCrypt] EVP_CIPHER_CTX_new() returned nullptr, cannot allocate cipher context");
        Logger::Trace("[CryptoUtils::aesCrypt] Exit: returning std::nullopt (context allocation failed)");
        return std::nullopt;
    }
    Logger::Debug("[CryptoUtils::aesCrypt] Cipher context created successfully");
    const EVP_CIPHER* cipher = EVP_aes_256_cbc();
    Logger::Debug("[CryptoUtils::aesCrypt] Using AES-256-CBC cipher, mode=%s", encrypt ? "encrypt" : "decrypt");
    if (EVP_CipherInit_ex(ctx, cipher, nullptr, key.data(), iv.data(), encrypt ? 1 : 0) != 1) {
        Logger::Error("[CryptoUtils::aesCrypt] EVP_CipherInit_ex failed for %s operation", encrypt ? "encryption" : "decryption");
        EVP_CIPHER_CTX_free(ctx);
        Logger::Trace("[CryptoUtils::aesCrypt] Exit: returning std::nullopt (CipherInit failed)");
        return std::nullopt;
    }
    Logger::Debug("[CryptoUtils::aesCrypt] Cipher initialized successfully");
    std::vector<uint8_t> out(in.size() + EVP_CIPHER_block_size(cipher));
    int outLen1 = 0;
    if (EVP_CipherUpdate(ctx, out.data(), &outLen1, in.data(), in.size()) != 1) {
        Logger::Error("[CryptoUtils::aesCrypt] EVP_CipherUpdate failed, input size=%zu", in.size());
        EVP_CIPHER_CTX_free(ctx);
        Logger::Trace("[CryptoUtils::aesCrypt] Exit: returning std::nullopt (CipherUpdate failed)");
        return std::nullopt;
    }
    Logger::Debug("[CryptoUtils::aesCrypt] CipherUpdate produced %d bytes", outLen1);
    int outLen2 = 0;
    if (EVP_CipherFinal_ex(ctx, out.data() + outLen1, &outLen2) != 1) {
        Logger::Error("[CryptoUtils::aesCrypt] EVP_CipherFinal_ex failed after %d bytes produced", outLen1);
        EVP_CIPHER_CTX_free(ctx);
        Logger::Trace("[CryptoUtils::aesCrypt] Exit: returning std::nullopt (CipherFinal failed)");
        return std::nullopt;
    }
    Logger::Debug("[CryptoUtils::aesCrypt] CipherFinal produced %d additional bytes", outLen2);
    EVP_CIPHER_CTX_free(ctx);
    out.resize(outLen1 + outLen2);
    Logger::Info("[CryptoUtils::aesCrypt] AES %s complete: input=%zu bytes, output=%zu bytes",
                 encrypt ? "encryption" : "decryption", in.size(), out.size());
    Logger::Trace("[CryptoUtils::aesCrypt] Exit: returning vector of size %zu", out.size());
    return out;
}

std::optional<std::vector<uint8_t>> AESEncrypt(const std::vector<uint8_t>& key,
                                               const std::vector<uint8_t>& iv,
                                               const std::vector<uint8_t>& plaintext) {
    Logger::Trace("[CryptoUtils::AESEncrypt] Entry: key.size()=%zu, iv.size()=%zu, plaintext.size()=%zu",
                  key.size(), iv.size(), plaintext.size());
    Logger::Debug("[CryptoUtils::AESEncrypt] Delegating to aesCrypt with encrypt=true");
    auto result = aesCrypt(key, iv, plaintext, true);
    if (result.has_value()) {
        Logger::Info("[CryptoUtils::AESEncrypt] Encryption successful, ciphertext size=%zu", result->size());
    } else {
        Logger::Error("[CryptoUtils::AESEncrypt] Encryption failed for plaintext of size %zu", plaintext.size());
    }
    Logger::Trace("[CryptoUtils::AESEncrypt] Exit: has_value=%s", result.has_value() ? "true" : "false");
    return result;
}

std::optional<std::vector<uint8_t>> AESDecrypt(const std::vector<uint8_t>& key,
                                               const std::vector<uint8_t>& iv,
                                               const std::vector<uint8_t>& ciphertext) {
    Logger::Trace("[CryptoUtils::AESDecrypt] Entry: key.size()=%zu, iv.size()=%zu, ciphertext.size()=%zu",
                  key.size(), iv.size(), ciphertext.size());
    Logger::Debug("[CryptoUtils::AESDecrypt] Delegating to aesCrypt with encrypt=false");
    auto result = aesCrypt(key, iv, ciphertext, false);
    if (result.has_value()) {
        Logger::Info("[CryptoUtils::AESDecrypt] Decryption successful, plaintext size=%zu", result->size());
    } else {
        Logger::Error("[CryptoUtils::AESDecrypt] Decryption failed for ciphertext of size %zu", ciphertext.size());
    }
    Logger::Trace("[CryptoUtils::AESDecrypt] Exit: has_value=%s", result.has_value() ? "true" : "false");
    return result;
}

} // namespace CryptoUtils
