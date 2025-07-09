// src/Protocol/ProtocolUtils.cpp
#include "Protocol/ProtocolUtils.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <stdexcept>
#include <cstring>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

namespace ProtocolUtils {

std::pair<std::string,std::string> SplitTag(const std::string& tag) {
    auto pos = tag.find(':');
    if (pos == std::string::npos) return {tag, ""};
    return { tag.substr(0,pos), tag.substr(pos+1) };
}

std::string JoinTag(const std::string& category, const std::string& subtype) {
    return subtype.empty() ? category : category + ":" + subtype;
}

std::string ToHexString(const std::vector<uint8_t>& data) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (auto b : data) oss << std::setw(2) << int(b);
    return oss.str();
}

std::optional<std::vector<uint8_t>> FromHexString(const std::string& hex) {
    if (hex.size()%2) return std::nullopt;
    std::vector<uint8_t> out;
    out.reserve(hex.size()/2);
    try {
        for (size_t i=0; i<hex.size(); i+=2) {
            uint8_t byte = std::stoi(hex.substr(i,2), nullptr, 16);
            out.push_back(byte);
        }
    } catch(...) {
        return std::nullopt;
    }
    return out;
}

uint8_t ComputeChecksum(const std::vector<uint8_t>& payload) {
    uint8_t chk = 0;
    for (auto b : payload) chk ^= b;
    return chk;
}

bool VerifyAndStripChecksum(std::vector<uint8_t>& payload) {
    if (payload.empty()) return false;
    uint8_t expected = payload.back();
    payload.pop_back();
    uint8_t actual = ComputeChecksum(payload);
    return actual == expected;
}

static std::string b64Encode(const uint8_t* data, size_t len) {
    BIO *bmem = BIO_new(BIO_s_mem());
    BIO *b64 = BIO_new(BIO_f_base64());
    b64 = BIO_push(b64, bmem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, data, len);
    BIO_flush(b64);
    BUF_MEM* bptr;
    BIO_get_mem_ptr(b64, &bptr);
    std::string ret(bptr->data, bptr->length);
    BIO_free_all(b64);
    return ret;
}

static std::vector<uint8_t> b64Decode(const std::string& b64) {
    BIO *bmem = BIO_new_mem_buf(b64.data(), (int)b64.size());
    BIO *b64f = BIO_new(BIO_f_base64());
    bmem = BIO_push(b64f, bmem);
    BIO_set_flags(bmem, BIO_FLAGS_BASE64_NO_NL);
    std::vector<uint8_t> out(b64.size());
    int len = BIO_read(bmem, out.data(), (int)out.size());
    BIO_free_all(bmem);
    if (len <= 0) return {};
    out.resize(len);
    return out;
}

std::string Base64Encode(const std::vector<uint8_t>& data) {
    return b64Encode(data.data(), data.size());
}

std::optional<std::vector<uint8_t>> Base64Decode(const std::string& b64) {
    auto v = b64Decode(b64);
    return v.empty() ? std::optional<std::vector<uint8_t>>{} : v;
}

} // namespace ProtocolUtils