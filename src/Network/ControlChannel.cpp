// src/Network/ControlChannel.cpp
//
// Implementation of the RS2/UE3 control-channel handshake message codec.
// See ControlChannel.h for scope and confidence tagging.

#include "Network/ControlChannel.h"

#include "Network/BitReader.h"
#include "Network/BitWriter.h"
#include "Network/URLOptions.h"

namespace ControlChannel {

// ---------------------------------------------------------------------------
// Build functions
// ---------------------------------------------------------------------------

std::vector<uint8_t> BuildHello(const HelloMessage& msg) {
    BitWriter w;
    w.WriteByte(NMTByte(NMT::Hello));
    // Leading bIsLittleEndian byte: position is [?] (spec §4). We place it first
    // to match stock UE3; if R2 capture shows otherwise this is the one place to
    // move it.
    w.WriteByte(msg.bIsLittleEndian);
    w.WriteInt32(msg.minVersion);          // [CB] serialize(...,4)
    w.WriteInt32(msg.version);             // [CB] serialize(...,4)
    w.WriteUInt64(msg.steamId);            // [CB] serialize(...,8)
    w.WriteString(msg.leechSessionId);     // [CB] FString
    w.WriteString(msg.token);              // [CB present] second FString
    return w.GetBytes();
}

std::vector<uint8_t> BuildChallenge(const ChallengeMessage& msg) {
    BitWriter w;
    w.WriteByte(NMTByte(NMT::Challenge));
    w.WriteInt32(msg.serverFlags);   // spec §4: leading int32
    w.WriteString(msg.challenge);
    return w.GetBytes();
}

std::vector<uint8_t> BuildNetspeed(const NetspeedMessage& msg) {
    BitWriter w;
    w.WriteByte(NMTByte(NMT::Netspeed));
    w.WriteInt32(msg.netspeed);
    return w.GetBytes();
}

std::vector<uint8_t> BuildLogin(const LoginMessage& msg) {
    BitWriter w;
    w.WriteByte(NMTByte(NMT::Login));
    w.WriteString(msg.response);
    w.WriteString(msg.url);
    w.WriteUInt64(msg.steamId);       // spec §4: trailing QWORD
    return w.GetBytes();
}

std::vector<uint8_t> BuildWelcome(const WelcomeMessage& msg) {
    BitWriter w;
    w.WriteByte(NMTByte(NMT::Welcome));
    w.WriteString(msg.levelName);
    w.WriteString(msg.gameName);
    w.WriteUInt64(msg.flags);         // spec §4: trailing QWORD (not a 3rd FString)
    return w.GetBytes();
}

std::vector<uint8_t> BuildFailure(const FailureMessage& msg) {
    BitWriter w;
    w.WriteByte(NMTByte(NMT::Failure));
    w.WriteString(msg.errorKey);
    return w.GetBytes();
}

std::vector<uint8_t> BuildUpgrade(const UpgradeMessage& msg) {
    BitWriter w;
    w.WriteByte(NMTByte(NMT::Upgrade));
    w.WriteInt32(msg.remoteMinVer);
    w.WriteInt32(msg.remoteVer);      // spec §4: second int32
    return w.GetBytes();
}

std::vector<uint8_t> BuildJoin(const JoinMessage& /*msg*/) {
    BitWriter w;
    w.WriteByte(NMTByte(NMT::Join)); // empty body
    return w.GetBytes();
}

std::string BuildLoginURL(const std::string& portal,
                          const std::vector<std::pair<std::string, std::string>>& options) {
    std::string url = portal;
    for (const auto& kv : options) {
        url += '?';
        url += kv.first;
        if (!kv.second.empty()) {
            url += '=';
            url += kv.second;
        }
    }
    return url;
}

// ---------------------------------------------------------------------------
// Parse helpers
// ---------------------------------------------------------------------------

namespace {

// Verify (optionally) the leading type byte. Returns false on overflow or
// mismatch.
bool CheckType(BitReader& r, bool expectType, NMT expected) {
    if (!expectType) {
        return true;
    }
    const uint8_t t = r.ReadByte();
    if (r.IsOverflowed()) {
        return false;
    }
    return t == NMTByte(expected);
}

} // namespace

bool PeekType(const uint8_t* data, size_t len, NMT& outType) {
    if (data == nullptr || len == 0) {
        return false;
    }
    outType = static_cast<NMT>(data[0]);
    return true;
}

bool ParseHello(const uint8_t* data, size_t len, HelloMessage& out, bool expectType) {
    BitReader r(data, len);
    if (!CheckType(r, expectType, NMT::Hello)) {
        return false;
    }
    out.bIsLittleEndian = r.ReadByte();
    out.minVersion = r.ReadInt32();
    out.version = r.ReadInt32();
    out.steamId = r.ReadUInt64();
    out.leechSessionId = r.ReadString();
    out.token = r.ReadString();
    return !r.IsOverflowed();
}

bool ParseChallenge(const uint8_t* data, size_t len, ChallengeMessage& out, bool expectType) {
    BitReader r(data, len);
    if (!CheckType(r, expectType, NMT::Challenge)) {
        return false;
    }
    out.serverFlags = r.ReadInt32();   // spec §4: leading int32
    out.challenge = r.ReadString();
    return !r.IsOverflowed();
}

bool ParseNetspeed(const uint8_t* data, size_t len, NetspeedMessage& out, bool expectType) {
    BitReader r(data, len);
    if (!CheckType(r, expectType, NMT::Netspeed)) {
        return false;
    }
    out.netspeed = r.ReadInt32();
    return !r.IsOverflowed();
}

bool ParseLogin(const uint8_t* data, size_t len, LoginMessage& out, bool expectType) {
    BitReader r(data, len);
    if (!CheckType(r, expectType, NMT::Login)) {
        return false;
    }
    out.response = r.ReadString();
    out.url = r.ReadString();
    out.steamId = r.ReadUInt64();      // spec §4: trailing QWORD
    return !r.IsOverflowed();
}

bool ParseWelcome(const uint8_t* data, size_t len, WelcomeMessage& out, bool expectType) {
    BitReader r(data, len);
    if (!CheckType(r, expectType, NMT::Welcome)) {
        return false;
    }
    out.levelName = r.ReadString();
    out.gameName = r.ReadString();
    out.flags = r.ReadUInt64();        // spec §4: trailing QWORD (not a 3rd FString)
    return !r.IsOverflowed();
}

bool ParseFailure(const uint8_t* data, size_t len, FailureMessage& out, bool expectType) {
    BitReader r(data, len);
    if (!CheckType(r, expectType, NMT::Failure)) {
        return false;
    }
    out.errorKey = r.ReadString();
    return !r.IsOverflowed();
}

bool ParseUpgrade(const uint8_t* data, size_t len, UpgradeMessage& out, bool expectType) {
    BitReader r(data, len);
    if (!CheckType(r, expectType, NMT::Upgrade)) {
        return false;
    }
    out.remoteMinVer = r.ReadInt32();
    out.remoteVer = r.ReadInt32();     // spec §4: second int32
    return !r.IsOverflowed();
}

bool ParseJoin(const uint8_t* data, size_t len, JoinMessage& /*out*/, bool expectType) {
    BitReader r(data, len);
    if (!CheckType(r, expectType, NMT::Join)) {
        return false;
    }
    return !r.IsOverflowed(); // empty body
}

} // namespace ControlChannel
