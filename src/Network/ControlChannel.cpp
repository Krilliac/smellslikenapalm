// src/Network/ControlChannel.cpp
//
// Implementation of the RS2/UE3 control-channel handshake message codec.
// See ControlChannel.h for scope and confidence tagging.

#include "Network/ControlChannel.h"

#include "Network/BitReader.h"
#include "Network/BitWriter.h"
#include "Network/URLOptions.h"
#include "Utils/Logger.h"

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
    w.WriteUInt32(msg.nonce);   // 32-bit cookie (one 40-bit bunch on the wire)
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

std::vector<uint8_t> BuildHandshakeChallenge(uint32_t nonce) {
    // The handshake NMT byte (0x1e) is the FIRST payload byte - there is NO 0x00
    // family prefix (reversed from the official server's f157: payload = [1e dd 96
    // 93 b1] = NMT 0x1e + 4 cookie bytes). The client validates/echoes the cookie,
    // so we send a full 32-bit nonce as the 4 bytes.
    BitWriter w;
    w.WriteByte(Handshake::kChallenge);    // 0x1e
    w.WriteByte(static_cast<uint8_t>(nonce & 0xFF));
    w.WriteByte(static_cast<uint8_t>((nonce >> 8) & 0xFF));
    w.WriteByte(static_cast<uint8_t>((nonce >> 16) & 0xFF));
    w.WriteByte(static_cast<uint8_t>((nonce >> 24) & 0xFF));
    return w.GetBytes();
}

std::vector<uint8_t> BuildHandshakeComplete() {
    // NMT 0x20, no family prefix (official f159 payload = [20]).
    BitWriter w;
    w.WriteByte(Handshake::kComplete);     // 0x20
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

// Defensive cap for any single attacker-supplied control-channel FString field.
// Real handshake strings (map/level names, EOS session ids, FURL option strings)
// are well under a kilobyte; this generous ceiling never trips on valid traffic
// but rejects an absurd length that survived BitReader's per-read bounds check
// (e.g. a deliberately bloated bunch buffer). Reject-and-log, never crash.
constexpr size_t kMaxControlStringLen = 16 * 1024;

// Reject a parse whose backing buffer is a null pointer paired with a non-zero
// claimed length. BitReader's bounds check only compares BIT POSITIONS against
// m_numBits (= len*8); it does NOT validate the data pointer, so a read on a
// (nullptr, len>0) reader would dereference null. This guard closes that gap.
// Purely additive: on valid input `data` is non-null so this never trips.
bool ValidBuffer(const uint8_t* data, size_t len, const char* ctx) {
    if (data == nullptr && len != 0) {
        Logger::Warn("[ControlChannel] %s: null data pointer with len=%zu - rejecting",
                     ctx, len);
        return false;
    }
    return true;
}

// Bound an FString field decoded from the wire. Returns false (reject the whole
// message) on an oversized field. On valid handshake input the field is tiny so
// this is a no-op; it only fires on hostile/garbage input.
bool StringSane(const std::string& s, const char* field, const char* ctx) {
    if (s.size() > kMaxControlStringLen) {
        Logger::Warn("[ControlChannel] %s: field '%s' length %zu exceeds cap %zu - rejecting",
                     ctx, field, s.size(), kMaxControlStringLen);
        return false;
    }
    return true;
}

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
    if (!ValidBuffer(data, len, "ParseHello")) {
        return false;
    }
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
    if (r.IsOverflowed()) {
        return false;
    }
    return StringSane(out.leechSessionId, "leechSessionId", "ParseHello")
        && StringSane(out.token, "token", "ParseHello");
}

bool ParseChallenge(const uint8_t* data, size_t len, ChallengeMessage& out, bool expectType) {
    if (!ValidBuffer(data, len, "ParseChallenge")) {
        return false;
    }
    BitReader r(data, len);
    if (!CheckType(r, expectType, NMT::Challenge)) {
        return false;
    }
    out.nonce = r.ReadUInt32();   // 32-bit cookie
    return !r.IsOverflowed();
}

bool ParseNetspeed(const uint8_t* data, size_t len, NetspeedMessage& out, bool expectType) {
    if (!ValidBuffer(data, len, "ParseNetspeed")) {
        return false;
    }
    BitReader r(data, len);
    if (!CheckType(r, expectType, NMT::Netspeed)) {
        return false;
    }
    out.netspeed = r.ReadInt32();
    return !r.IsOverflowed();
}

bool ParseLogin(const uint8_t* data, size_t len, LoginMessage& out, bool expectType) {
    if (!ValidBuffer(data, len, "ParseLogin")) {
        return false;
    }
    BitReader r(data, len);
    if (!CheckType(r, expectType, NMT::Login)) {
        return false;
    }
    out.response = r.ReadString();
    out.url = r.ReadString();
    out.steamId = r.ReadUInt64();      // spec §4: trailing QWORD
    if (r.IsOverflowed()) {
        return false;
    }
    return StringSane(out.response, "response", "ParseLogin")
        && StringSane(out.url, "url", "ParseLogin");
}

bool ParseWelcome(const uint8_t* data, size_t len, WelcomeMessage& out, bool expectType) {
    if (!ValidBuffer(data, len, "ParseWelcome")) {
        return false;
    }
    BitReader r(data, len);
    if (!CheckType(r, expectType, NMT::Welcome)) {
        return false;
    }
    out.levelName = r.ReadString();
    out.gameName = r.ReadString();
    out.flags = r.ReadUInt64();        // spec §4: trailing QWORD (not a 3rd FString)
    if (r.IsOverflowed()) {
        return false;
    }
    return StringSane(out.levelName, "levelName", "ParseWelcome")
        && StringSane(out.gameName, "gameName", "ParseWelcome");
}

bool ParseFailure(const uint8_t* data, size_t len, FailureMessage& out, bool expectType) {
    if (!ValidBuffer(data, len, "ParseFailure")) {
        return false;
    }
    BitReader r(data, len);
    if (!CheckType(r, expectType, NMT::Failure)) {
        return false;
    }
    out.errorKey = r.ReadString();
    if (r.IsOverflowed()) {
        return false;
    }
    return StringSane(out.errorKey, "errorKey", "ParseFailure");
}

bool ParseUpgrade(const uint8_t* data, size_t len, UpgradeMessage& out, bool expectType) {
    if (!ValidBuffer(data, len, "ParseUpgrade")) {
        return false;
    }
    BitReader r(data, len);
    if (!CheckType(r, expectType, NMT::Upgrade)) {
        return false;
    }
    out.remoteMinVer = r.ReadInt32();
    out.remoteVer = r.ReadInt32();     // spec §4: second int32
    return !r.IsOverflowed();
}

bool ParseJoin(const uint8_t* data, size_t len, JoinMessage& /*out*/, bool expectType) {
    if (!ValidBuffer(data, len, "ParseJoin")) {
        return false;
    }
    BitReader r(data, len);
    if (!CheckType(r, expectType, NMT::Join)) {
        return false;
    }
    return !r.IsOverflowed(); // empty body
}

bool ConsumeMessage(BitReader& r, NMT& outType) {
    const uint8_t typeByte = r.ReadByte();
    if (r.IsOverflowed()) {
        return false;
    }
    const NMT type = static_cast<NMT>(typeByte);

    // Advance past the body for this NMT. Field reads MUST mirror the Parse*
    // functions above (and ControlChannel.h spec §4). Values are discarded - we
    // only need to advance the reader to the end of this message.
    switch (type) {
        case NMT::Hello:        // RS2 on-wire: NMT + a single BYTE.
            // NOTE: the disassembly suggested {bLE,INT,INT,QWORD,FStr,FStr}, but
            // the LIVE client's Hello bunch is only 16 bits (NMT + 1 byte). The
            // version/SteamId fields arrive in a later message, not here. Reading
            // the full layout here made the reassembler wait forever for bytes the
            // client never sends, deadlocking the handshake.
            r.ReadByte();
            break;
        case NMT::Welcome:      // FStr, FStr, QWORD
            r.ReadString(); r.ReadString(); r.ReadUInt64();
            break;
        case NMT::Upgrade:      // INT, INT
            r.ReadInt32(); r.ReadInt32();
            break;
        case NMT::Challenge:    // DWORD cookie
            r.ReadUInt32();
            break;
        case NMT::Netspeed:     // INT
            r.ReadInt32();
            break;
        case NMT::Login:        // FStr, FStr, QWORD
            r.ReadString(); r.ReadString(); r.ReadUInt64();
            break;
        case NMT::Failure:      // FStr
            r.ReadString();
            break;
        case NMT::Join:         // empty
            break;
        case NMT::SteamLogin:   // INT, INT, QWORD  (spec §4 handler 0x10)
            r.ReadInt32(); r.ReadInt32(); r.ReadUInt64();
            break;
        default:
            // Unknown / not-yet-modeled body length: cannot delimit safely.
            // Surface the offending NMT so a malformed/hostile stream that stalls
            // the reassembler is diagnosable rather than silently dropped.
            Logger::Warn("[ControlChannel] ConsumeMessage: unrecognized NMT 0x%02X - "
                         "cannot determine body length, rejecting",
                         static_cast<unsigned>(typeByte));
            return false;
    }

    if (r.IsOverflowed()) {
        return false; // incomplete: more bunches needed
    }
    outType = type;
    return true;
}

} // namespace ControlChannel
