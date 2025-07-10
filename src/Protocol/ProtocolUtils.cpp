#include "Protocol/ProtocolUtils.h"
#include "Protocol/PacketTypes.h"
#include "Utils/Logger.h"          // For warning/error logging

#include <sstream>
#include <iomanip>
#include <algorithm>
#include <stdexcept>
#include <cstring>

// OpenSSL for Base64
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

namespace ProtocolUtils {

//-------------------------------------------------------------------------------------------------
// Tag splitting / joining

std::pair<std::string,std::string> SplitTag(const std::string& tag) {
    auto pos = tag.find(':');
    if (pos == std::string::npos) return {tag, ""};
    return { tag.substr(0,pos), tag.substr(pos+1) };
}

std::string JoinTag(const std::string& category, const std::string& subtype) {
    return subtype.empty() ? category : category + ":" + subtype;
}

//-------------------------------------------------------------------------------------------------
// Hex-string encoding / decoding

std::string ToHexString(const std::vector<uint8_t>& data) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (auto b : data) oss << std::setw(2) << int(b);
    return oss.str();
}

std::optional<std::vector<uint8_t>> FromHexString(const std::string& hex) {
    if (hex.size() % 2) return std::nullopt;
    std::vector<uint8_t> out;
    out.reserve(hex.size()/2);
    try {
        for (size_t i = 0; i < hex.size(); i += 2) {
            uint8_t byte = static_cast<uint8_t>(
                std::stoi(hex.substr(i,2), nullptr, 16)
            );
            out.push_back(byte);
        }
    } catch (...) {
        return std::nullopt;
    }
    return out;
}

//-------------------------------------------------------------------------------------------------
// XOR checksum

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

//-------------------------------------------------------------------------------------------------
// Base64 encode / decode (no newlines)

static std::string b64Encode(const uint8_t* data, size_t len) {
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
    return ret;
}

static std::vector<uint8_t> b64Decode(const std::string& b64str) {
    BIO* bmem = BIO_new_mem_buf(b64str.data(), (int)b64str.size());
    BIO* b64f = BIO_new(BIO_f_base64());
    bmem = BIO_push(b64f, bmem);
    BIO_set_flags(bmem, BIO_FLAGS_BASE64_NO_NL);
    std::vector<uint8_t> out(b64str.size());
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

//-------------------------------------------------------------------------------------------------
// RS2V PacketType ↔ tag mappings

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
    auto parts = SplitTag(tag);
    const std::string& key = parts.second.empty() ? parts.first : parts.second;
    for (const auto& kv : kTagTypeMap) {
        if (kv.first == key) return kv.second;
    }
    Logger::Warn("ProtocolUtils::TagToType: Unknown tag '%s'", tag.c_str());
    return PacketType::PT_INVALID;
}

std::string TypeToTag(PacketType type) {
    for (const auto& kv : kTagTypeMap) {
        if (kv.second == type) return kv.first;
    }
    // Handle custom/extension range
    auto t = static_cast<uint16_t>(type);
    auto start = static_cast<uint16_t>(PacketType::PT_CUSTOM_START);
    auto max   = static_cast<uint16_t>(PacketType::PT_MAX);
    if (t >= start && t < max) {
        return "CUSTOM_" + std::to_string(t - start);
    }
    Logger::Error("ProtocolUtils::TypeToTag: Unrecognized PacketType %u", t);
    return "";
}

} // namespace ProtocolUtils