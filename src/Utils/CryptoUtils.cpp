// src/Utils/CryptoUtils.cpp
#include "Utils/CryptoUtils.h"
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#include <openssl/bio.h>
#include <cstring>

namespace CryptoUtils {

static std::string toHex(const uint8_t* data, size_t len) {
    static const char* hexDigits = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = data[i];
        out.push_back(hexDigits[b >> 4]);
        out.push_back(hexDigits[b & 0xF]);
    }
    return out;
}

std::string SHA256Hex(const std::vector<uint8_t>& data) {
    uint8_t hash[SHA256_DIGEST_LENGTH];
    SHA256(data.data(), data.size(), hash);
    return toHex(hash, SHA256_DIGEST_LENGTH);
}

std::vector<uint8_t> HMAC_SHA256(const std::vector<uint8_t>& key,
                                 const std::vector<uint8_t>& data) {
    unsigned int len = EVP_MAX_MD_SIZE;
    std::vector<uint8_t> out(len);
    HMAC(EVP_sha256(), key.data(), (int)key.size(),
         data.data(), data.size(), out.data(), &len);
    out.resize(len);
    return out;
}

std::string Base64Encode(const std::vector<uint8_t>& data) {
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* bmem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, bmem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, data.data(), data.size());
    BIO_flush(b64);
    BUF_MEM* bptr;
    BIO_get_mem_ptr(b64, &bptr);
    std::string out(bptr->data, bptr->length);
    BIO_free_all(b64);
    return out;
}

std::optional<std::vector<uint8_t>> Base64Decode(const std::string& b64) {
    BIO* bmem = BIO_new_mem_buf(b64.data(), b64.size());
    BIO* b64f = BIO_new(BIO_f_base64());
    bmem = BIO_push(b64f, bmem);
    BIO_set_flags(bmem, BIO_FLAGS_BASE64_NO_NL);
    std::vector<uint8_t> out(b64.size());
    int len = BIO_read(bmem, out.data(), out.size());
    BIO_free_all(bmem);
    if (len <= 0) return std::nullopt;
    out.resize(len);
    return out;
}

std::vector<uint8_t> GenerateRandomBytes(size_t length) {
    std::vector<uint8_t> out(length);
    RAND_bytes(out.data(), length);
    return out;
}

static std::optional<std::vector<uint8_t>> aesCrypt(const std::vector<uint8_t>& key,
                                                    const std::vector<uint8_t>& iv,
                                                    const std::vector<uint8_t>& in,
                                                    bool encrypt) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return std::nullopt;
    const EVP_CIPHER* cipher = EVP_aes_256_cbc();
    if (EVP_CipherInit_ex(ctx, cipher, nullptr, key.data(), iv.data(), encrypt ? 1 : 0) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return std::nullopt;
    }
    std::vector<uint8_t> out(in.size() + EVP_CIPHER_block_size(cipher));
    int outLen1 = 0;
    if (EVP_CipherUpdate(ctx, out.data(), &outLen1, in.data(), in.size()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return std::nullopt;
    }
    int outLen2 = 0;
    if (EVP_CipherFinal_ex(ctx, out.data() + outLen1, &outLen2) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return std::nullopt;
    }
    EVP_CIPHER_CTX_free(ctx);
    out.resize(outLen1 + outLen2);
    return out;
}

std::optional<std::vector<uint8_t>> AESEncrypt(const std::vector<uint8_t>& key,
                                               const std::vector<uint8_t>& iv,
                                               const std::vector<uint8_t>& plaintext) {
    return aesCrypt(key, iv, plaintext, true);
}

std::optional<std::vector<uint8_t>> AESDecrypt(const std::vector<uint8_t>& key,
                                               const std::vector<uint8_t>& iv,
                                               const std::vector<uint8_t>& ciphertext) {
    return aesCrypt(key, iv, ciphertext, false);
}

} // namespace CryptoUtils