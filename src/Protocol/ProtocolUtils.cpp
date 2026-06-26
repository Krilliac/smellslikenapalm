#include "Protocol/ProtocolUtils.h"
#include "Protocol/PacketTypes.h"
#include "Utils/Logger.h"          // For warning/error logging

#include <sstream>
#include <iomanip>
#include <algorithm>
#include <stdexcept>
#include <cstring>
#include <cctype>

// OpenSSL for Base64 (optional — guarded so the file builds without OpenSSL,
// e.g. under MinGW where OpenSSL may not be found by CMake).
#ifdef RS2V_HAS_OPENSSL
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#endif

namespace ProtocolUtils {

//-------------------------------------------------------------------------------------------------
// Tag splitting / joining

std::pair<std::string,std::string> SplitTag(const std::string& tag) {
    Logger::Trace("[ProtocolUtils::SplitTag] entry — tag='%s'", tag.c_str());
    auto pos = tag.find(':');
    if (pos == std::string::npos) {
        Logger::Debug("[ProtocolUtils::SplitTag] no ':' separator found — returning category='%s', subtype=''", tag.c_str());
        Logger::Trace("[ProtocolUtils::SplitTag] exit — returning {category='%s', subtype=''}", tag.c_str());
        return {tag, ""};
    }
    std::string category = tag.substr(0,pos);
    std::string subtype = tag.substr(pos+1);
    Logger::Debug("[ProtocolUtils::SplitTag] split tag at position %zu — category='%s', subtype='%s'",
                  pos, category.c_str(), subtype.c_str());
    Logger::Trace("[ProtocolUtils::SplitTag] exit — returning {category='%s', subtype='%s'}",
                  category.c_str(), subtype.c_str());
    return { category, subtype };
}

std::string JoinTag(const std::string& category, const std::string& subtype) {
    Logger::Trace("[ProtocolUtils::JoinTag] entry — category='%s', subtype='%s'",
                  category.c_str(), subtype.c_str());
    std::string result = subtype.empty() ? category : category + ":" + subtype;
    if (subtype.empty()) {
        Logger::Debug("[ProtocolUtils::JoinTag] subtype is empty — returning category only: '%s'", result.c_str());
    } else {
        Logger::Debug("[ProtocolUtils::JoinTag] joined category and subtype — result='%s'", result.c_str());
    }
    Logger::Trace("[ProtocolUtils::JoinTag] exit — returning '%s'", result.c_str());
    return result;
}

//-------------------------------------------------------------------------------------------------
// Hex-string encoding / decoding

std::string ToHexString(const std::vector<uint8_t>& data) {
    Logger::Trace("[ProtocolUtils::ToHexString] entry — data.size()=%zu bytes", data.size());
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (auto b : data) oss << std::setw(2) << int(b);
    std::string result = oss.str();
    Logger::Debug("[ProtocolUtils::ToHexString] converted %zu bytes to hex string of length %zu",
                  data.size(), result.size());
    Logger::Trace("[ProtocolUtils::ToHexString] exit — returning hex string of length %zu", result.size());
    return result;
}

std::optional<std::vector<uint8_t>> FromHexString(const std::string& hex) {
    Logger::Trace("[ProtocolUtils::FromHexString] entry — hex string length=%zu", hex.size());
    if (hex.size() % 2) {
        Logger::Warn("[ProtocolUtils::FromHexString] odd-length hex string (%zu chars) — cannot decode", hex.size());
        Logger::Trace("[ProtocolUtils::FromHexString] exit — returning nullopt (odd length)");
        return std::nullopt;
    }
    std::vector<uint8_t> out;
    out.reserve(hex.size()/2);
    Logger::Debug("[ProtocolUtils::FromHexString] parsing %zu hex chars into %zu bytes", hex.size(), hex.size()/2);
    try {
        for (size_t i = 0; i < hex.size(); i += 2) {
            // Defensive: std::stoi(base 16) also accepts leading whitespace, a
            // sign, or a "0x" prefix, which would silently decode malformed input
            // to a wrong byte instead of failing. Require both chars of the pair
            // to be plain hex digits before converting. (size() is even, so i+1 is
            // always in range here.) Valid hex strings are unaffected.
            const char hi = hex[i];
            const char lo = hex[i + 1];
            if (!std::isxdigit(static_cast<unsigned char>(hi)) ||
                !std::isxdigit(static_cast<unsigned char>(lo))) {
                Logger::Warn("[ProtocolUtils::FromHexString] non-hex character in pair at offset %zu — rejecting input", i);
                Logger::Trace("[ProtocolUtils::FromHexString] exit — returning nullopt (non-hex char)");
                return std::nullopt;
            }
            uint8_t byte = static_cast<uint8_t>(
                std::stoi(hex.substr(i,2), nullptr, 16)
            );
            out.push_back(byte);
        }
    } catch (...) {
        Logger::Error("[ProtocolUtils::FromHexString] failed to parse hex string — invalid hex characters encountered");
        Logger::Trace("[ProtocolUtils::FromHexString] exit — returning nullopt (parse error)");
        return std::nullopt;
    }
    Logger::Debug("[ProtocolUtils::FromHexString] successfully decoded %zu bytes from hex string", out.size());
    Logger::Trace("[ProtocolUtils::FromHexString] exit — returning %zu bytes", out.size());
    return out;
}

//-------------------------------------------------------------------------------------------------
// XOR checksum

uint8_t ComputeChecksum(const std::vector<uint8_t>& payload) {
    Logger::Trace("[ProtocolUtils::ComputeChecksum] entry — payload.size()=%zu bytes", payload.size());
    uint8_t chk = 0;
    for (auto b : payload) chk ^= b;
    Logger::Debug("[ProtocolUtils::ComputeChecksum] computed XOR checksum=0x%02X over %zu bytes",
                  chk, payload.size());
    Logger::Trace("[ProtocolUtils::ComputeChecksum] exit — returning checksum=0x%02X", chk);
    return chk;
}

bool VerifyAndStripChecksum(std::vector<uint8_t>& payload) {
    Logger::Trace("[ProtocolUtils::VerifyAndStripChecksum] entry — payload.size()=%zu bytes", payload.size());
    if (payload.empty()) {
        Logger::Warn("[ProtocolUtils::VerifyAndStripChecksum] empty payload — cannot verify checksum");
        Logger::Trace("[ProtocolUtils::VerifyAndStripChecksum] exit — returning false (empty payload)");
        return false;
    }
    uint8_t expected = payload.back();
    Logger::Debug("[ProtocolUtils::VerifyAndStripChecksum] expected checksum=0x%02X (last byte of payload)", expected);
    payload.pop_back();
    uint8_t actual = ComputeChecksum(payload);
    bool match = (actual == expected);
    if (match) {
        Logger::Debug("[ProtocolUtils::VerifyAndStripChecksum] checksum verified: actual=0x%02X matches expected=0x%02X, stripped payload size=%zu",
                      actual, expected, payload.size());
        Logger::Info("[ProtocolUtils::VerifyAndStripChecksum] checksum verification passed for %zu-byte payload", payload.size());
    } else {
        Logger::Error("[ProtocolUtils::VerifyAndStripChecksum] checksum mismatch: actual=0x%02X != expected=0x%02X for %zu-byte payload",
                      actual, expected, payload.size());
    }
    Logger::Trace("[ProtocolUtils::VerifyAndStripChecksum] exit — returning %s", match ? "true" : "false");
    return match;
}

//-------------------------------------------------------------------------------------------------
// Base64 encode / decode (no newlines)

#ifdef RS2V_HAS_OPENSSL

static std::string b64Encode(const uint8_t* data, size_t len) {
    Logger::Trace("[ProtocolUtils::b64Encode] entry — data=%p, len=%zu", (const void*)data, len);
    BIO* bmem = BIO_new(BIO_s_mem());
    BIO* b64  = BIO_new(BIO_f_base64());
    b64 = BIO_push(b64, bmem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, data, (int)len);
    BIO_flush(b64);
    BUF_MEM* bptr;
    BIO_get_mem_ptr(b64, &bptr);
    std::string ret(bptr->data, bptr->length);
    BIO_free_all(b64);
    Logger::Debug("[ProtocolUtils::b64Encode] encoded %zu raw bytes to %zu base64 chars", len, ret.size());
    Logger::Trace("[ProtocolUtils::b64Encode] exit — returning base64 string of length %zu", ret.size());
    return ret;
}

static std::vector<uint8_t> b64Decode(const std::string& b64str) {
    Logger::Trace("[ProtocolUtils::b64Decode] entry — b64str length=%zu", b64str.size());
    BIO* bmem = BIO_new_mem_buf(b64str.data(), (int)b64str.size());
    BIO* b64f = BIO_new(BIO_f_base64());
    bmem = BIO_push(b64f, bmem);
    BIO_set_flags(bmem, BIO_FLAGS_BASE64_NO_NL);
    std::vector<uint8_t> out(b64str.size());
    int len = BIO_read(bmem, out.data(), (int)out.size());
    BIO_free_all(bmem);
    if (len <= 0) {
        Logger::Error("[ProtocolUtils::b64Decode] BIO_read returned %d — decoding failed for %zu-char input", len, b64str.size());
        Logger::Trace("[ProtocolUtils::b64Decode] exit — returning empty vector (decode failure)");
        return {};
    }
    out.resize(len);
    Logger::Debug("[ProtocolUtils::b64Decode] decoded %zu base64 chars to %d raw bytes", b64str.size(), len);
    Logger::Trace("[ProtocolUtils::b64Decode] exit — returning %d bytes", len);
    return out;
}

#else // !RS2V_HAS_OPENSSL

// Portable, dependency-free Base64 (NOT a cryptographic operation, so a built-in
// implementation is fully correct and matches OpenSSL's NO_NL output).
static const char kB64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string b64Encode(const uint8_t* data, size_t len) {
    Logger::Trace("[ProtocolUtils::b64Encode] entry (built-in) — data=%p, len=%zu", (const void*)data, len);
    std::string ret;
    ret.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        ret.push_back(kB64Alphabet[(n >> 18) & 0x3F]);
        ret.push_back(kB64Alphabet[(n >> 12) & 0x3F]);
        ret.push_back(kB64Alphabet[(n >> 6) & 0x3F]);
        ret.push_back(kB64Alphabet[n & 0x3F]);
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t n = uint32_t(data[i]) << 16;
        ret.push_back(kB64Alphabet[(n >> 18) & 0x3F]);
        ret.push_back(kB64Alphabet[(n >> 12) & 0x3F]);
        ret.push_back('=');
        ret.push_back('=');
    } else if (rem == 2) {
        uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        ret.push_back(kB64Alphabet[(n >> 18) & 0x3F]);
        ret.push_back(kB64Alphabet[(n >> 12) & 0x3F]);
        ret.push_back(kB64Alphabet[(n >> 6) & 0x3F]);
        ret.push_back('=');
    }
    Logger::Debug("[ProtocolUtils::b64Encode] (built-in) encoded %zu raw bytes to %zu base64 chars", len, ret.size());
    Logger::Trace("[ProtocolUtils::b64Encode] exit — returning base64 string of length %zu", ret.size());
    return ret;
}

static int b64DecodeChar(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1; // padding or invalid
}

static std::vector<uint8_t> b64Decode(const std::string& b64str) {
    Logger::Trace("[ProtocolUtils::b64Decode] entry (built-in) — b64str length=%zu", b64str.size());
    std::vector<uint8_t> out;
    out.reserve((b64str.size() / 4) * 3);
    uint32_t buf = 0;
    int bits = 0;
    for (char c : b64str) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        int v = b64DecodeChar(c);
        if (v < 0) {
            Logger::Error("[ProtocolUtils::b64Decode] (built-in) invalid base64 char — decode failed for %zu-char input", b64str.size());
            Logger::Trace("[ProtocolUtils::b64Decode] exit — returning empty vector (decode failure)");
            return {};
        }
        buf = (buf << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
        }
    }
    Logger::Debug("[ProtocolUtils::b64Decode] (built-in) decoded %zu base64 chars to %zu raw bytes", b64str.size(), out.size());
    Logger::Trace("[ProtocolUtils::b64Decode] exit — returning %zu bytes", out.size());
    return out;
}

#endif // RS2V_HAS_OPENSSL

std::string Base64Encode(const std::vector<uint8_t>& data) {
    Logger::Trace("[ProtocolUtils::Base64Encode] entry — data.size()=%zu bytes", data.size());
    std::string result = b64Encode(data.data(), data.size());
    Logger::Info("[ProtocolUtils::Base64Encode] encoded %zu bytes to base64 string of length %zu",
                 data.size(), result.size());
    Logger::Trace("[ProtocolUtils::Base64Encode] exit — returning base64 string of length %zu", result.size());
    return result;
}

std::optional<std::vector<uint8_t>> Base64Decode(const std::string& b64) {
    Logger::Trace("[ProtocolUtils::Base64Decode] entry — b64 string length=%zu", b64.size());
    auto v = b64Decode(b64);
    if (v.empty()) {
        Logger::Warn("[ProtocolUtils::Base64Decode] decode returned empty result for input of length %zu — returning nullopt",
                     b64.size());
        Logger::Trace("[ProtocolUtils::Base64Decode] exit — returning nullopt (empty decode result)");
        return std::optional<std::vector<uint8_t>>{};
    }
    Logger::Info("[ProtocolUtils::Base64Decode] decoded base64 string of length %zu to %zu bytes",
                 b64.size(), v.size());
    Logger::Trace("[ProtocolUtils::Base64Decode] exit — returning %zu bytes", v.size());
    return v;
}

//-------------------------------------------------------------------------------------------------
// RS2V PacketType <-> tag mappings

static const std::vector<std::pair<std::string,PacketType>> kTagTypeMap = {
    { "HEARTBEAT",          PacketType::PT_HEARTBEAT },
    { "CHAT_MESSAGE",       PacketType::PT_CHAT_MESSAGE },
    { "PLAYER_SPAWN",       PacketType::PT_PLAYER_SPAWN },
    { "PLAYER_MOVE",        PacketType::PT_PLAYER_MOVE },
    { "PLAYER_ACTION",      PacketType::PT_PLAYER_ACTION },
    { "HEALTH_UPDATE",      PacketType::PT_HEALTH_UPDATE },
    { "TEAM_UPDATE",        PacketType::PT_TEAM_UPDATE },
    { "SPAWN_ENTITY",       PacketType::PT_SPAWN_ENTITY },
    { "DESPAWN_ENTITY",     PacketType::PT_DESPAWN_ENTITY },
    { "ACTOR_REPLICATION",  PacketType::PT_ACTOR_REPLICATION },
    { "OBJECTIVE_UPDATE",   PacketType::PT_OBJECTIVE_UPDATE },
    { "SCORE_UPDATE",       PacketType::PT_SCORE_UPDATE },
    { "SESSION_STATE",      PacketType::PT_SESSION_STATE },
    { "CHAT_HISTORY",       PacketType::PT_CHAT_HISTORY },
    { "ADMIN_COMMAND",      PacketType::PT_ADMIN_COMMAND },
    { "SERVER_NOTIFICATION",PacketType::PT_SERVER_NOTIFICATION },
    { "MAP_CHANGE",         PacketType::PT_MAP_CHANGE },
    { "CONFIG_SYNC",        PacketType::PT_CONFIG_SYNC },
    { "COMPRESSION",        PacketType::PT_COMPRESSION },
    { "RPC_CALL",           PacketType::PT_RPC_CALL },
    { "RPC_RESPONSE",       PacketType::PT_RPC_RESPONSE }
};

PacketType TagToType(const std::string& tag) {
    Logger::Trace("[ProtocolUtils::TagToType] entry — tag='%s'", tag.c_str());
    auto parts = SplitTag(tag);
    const std::string& key = parts.second.empty() ? parts.first : parts.second;
    Logger::Debug("[ProtocolUtils::TagToType] lookup key='%s' (category='%s', subtype='%s')",
                  key.c_str(), parts.first.c_str(), parts.second.c_str());
    for (const auto& kv : kTagTypeMap) {
        if (kv.first == key) {
            Logger::Debug("[ProtocolUtils::TagToType] matched tag '%s' to PacketType=%s (%d)",
                          tag.c_str(), kv.first.c_str(), static_cast<int>(kv.second));
            Logger::Trace("[ProtocolUtils::TagToType] exit — returning PacketType %d", static_cast<int>(kv.second));
            return kv.second;
        }
    }
    Logger::Warn("ProtocolUtils::TagToType: Unknown tag '%s'", tag.c_str());
    Logger::Debug("[ProtocolUtils::TagToType] searched %zu entries in kTagTypeMap — no match found for key='%s'",
                  kTagTypeMap.size(), key.c_str());
    Logger::Trace("[ProtocolUtils::TagToType] exit — returning PT_INVALID");
    return PacketType::PT_INVALID;
}

std::string TypeToTag(PacketType type) {
    Logger::Trace("[ProtocolUtils::TypeToTag] entry — type=%d", static_cast<int>(type));
    for (const auto& kv : kTagTypeMap) {
        if (kv.second == type) {
            Logger::Debug("[ProtocolUtils::TypeToTag] matched PacketType=%d to tag='%s'",
                          static_cast<int>(type), kv.first.c_str());
            Logger::Trace("[ProtocolUtils::TypeToTag] exit — returning '%s'", kv.first.c_str());
            return kv.first;
        }
    }
    // Handle custom/extension range
    auto t = static_cast<uint16_t>(type);
    auto start = static_cast<uint16_t>(PacketType::PT_CUSTOM_START);
    auto max   = static_cast<uint16_t>(PacketType::PT_MAX);
    Logger::Debug("[ProtocolUtils::TypeToTag] no standard match — checking custom range: type=%u, start=%u, max=%u",
                  t, start, max);
    if (t >= start && t < max) {
        std::string customTag = "CUSTOM_" + std::to_string(t - start);
        Logger::Debug("[ProtocolUtils::TypeToTag] type %u is in custom range — generated tag='%s'",
                      t, customTag.c_str());
        Logger::Trace("[ProtocolUtils::TypeToTag] exit — returning custom tag '%s'", customTag.c_str());
        return customTag;
    }
    Logger::Error("ProtocolUtils::TypeToTag: Unrecognized PacketType %u", t);
    Logger::Trace("[ProtocolUtils::TypeToTag] exit — returning empty string (unrecognized type)");
    return "";
}

} // namespace ProtocolUtils
