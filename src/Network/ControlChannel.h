// src/Network/ControlChannel.h
//
// Build & parse functions for the UE3 / RS2 control-channel handshake messages.
//
// SCOPE: this is an ISOLATED, SWAPPABLE MESSAGE CODEC, not live integration. Each
// Build* function serializes ONE control message payload (the message-type BYTE
// followed by the message fields) into a byte buffer using BitWriter. Each Parse*
// function consumes such a payload with BitReader and returns a struct plus a
// success flag.
//
// The payload these functions produce/consume is the BUNCH PAYLOAD content:
// `<BYTE NMT type><fields...>`. The surrounding UE3 packet/bunch FRAMING
// (PacketId, ack bitfield, bunch header bits) is version-sensitive and only
// thinly modeled here - see BunchFraming below, which is TODO-marked. The point
// is a correct, isolated message codec; framing can be layered on once the
// capture in spec §9 pins the header bits.
//
// All wire constants come from NetMessages.h (the single source of truth).
// Confidence tags ([CB]/[UE3]/[?]) follow RS2V_ControlChannel_WireSpec_7258.md.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Network/NetMessages.h"

namespace ControlChannel {

// ===========================================================================
//  Message structs (parsed representations)
// ===========================================================================

// NMT_Hello (C->S) - RS2 EGS/EOS "Leech" extended layout [CB body]:
//   BYTE  bIsLittleEndian   [?] presence/position (UE3 stock has it; spec §4)
//   INT   MinVersion        client's GEngineMinNetVersion          [CB]
//   INT   Version           client's GEngineVersion (== 7258)      [CB]
//   QWORD SteamId           64-bit Steam ID                        [CB]
//   FSTR  LeechSessionId    EOS/"Leech" session id                 [CB]
//   FSTR  Token             second FString (purpose [?])           [CB present]
struct HelloMessage {
    uint8_t  bIsLittleEndian = 1;
    int32_t  minVersion = kMinNetVersion;
    int32_t  version = kEngineVersion;
    uint64_t steamId = 0;
    std::string leechSessionId;
    std::string token;
};

// NMT_Challenge (S->C): { FSTR ServerNonce } [UE3]
struct ChallengeMessage {
    std::string challenge;
};

// NMT_Netspeed (C->S): { INT Netspeed } [UE3]
struct NetspeedMessage {
    int32_t netspeed = kNetspeedInternet;
};

// NMT_Login (C->S): { FSTR ClientResponse, FSTR URL } [UE3]
struct LoginMessage {
    std::string response; // computed from the Challenge nonce
    std::string url;      // FURL option string (see URLOptions.h)
};

// NMT_Welcome (S->C): { FSTR LevelName, FSTR GameName, FSTR Redirect } [UE3]
struct WelcomeMessage {
    std::string levelName;
    std::string gameName;
    std::string redirectUrl; // RS2 may omit; [UE3]
};

// NMT_Failure (S->C): { FSTR ErrorKey } [UE3]
struct FailureMessage {
    std::string errorKey;
};

// NMT_Upgrade (S->C): { INT RemoteMinVer } [UE3]
struct UpgradeMessage {
    int32_t remoteMinVer = kMinNetVersion;
};

// NMT_Join (C->S): empty body [UE3]
struct JoinMessage {};

// ===========================================================================
//  Build functions - return the message payload bytes (BYTE type + fields).
// ===========================================================================

std::vector<uint8_t> BuildHello(const HelloMessage& msg);
std::vector<uint8_t> BuildChallenge(const ChallengeMessage& msg);
std::vector<uint8_t> BuildNetspeed(const NetspeedMessage& msg);
std::vector<uint8_t> BuildLogin(const LoginMessage& msg);
std::vector<uint8_t> BuildWelcome(const WelcomeMessage& msg);
std::vector<uint8_t> BuildFailure(const FailureMessage& msg);
std::vector<uint8_t> BuildUpgrade(const UpgradeMessage& msg);
std::vector<uint8_t> BuildJoin(const JoinMessage& msg);

// Convenience: build a Login URL FURL string from option pairs. The leading
// portal/map is `portal`; each option is appended as "?Key=Value" (or "?Key" for
// a bare flag where value is empty). Mirrors the client-side builder in spec §6.
std::string BuildLoginURL(const std::string& portal,
                          const std::vector<std::pair<std::string, std::string>>& options);

// ===========================================================================
//  Parse functions - consume a payload (BYTE type + fields).
//
//  Each returns true on success. On any malformed/truncated input the BitReader
//  overflow flag trips and the function returns false (never reads OOB). When
//  `expectType` is true the leading type byte is verified to match the expected
//  NMT; pass false if the caller already consumed/dispatched the type byte.
// ===========================================================================

bool ParseHello(const uint8_t* data, size_t len, HelloMessage& out, bool expectType = true);
bool ParseChallenge(const uint8_t* data, size_t len, ChallengeMessage& out, bool expectType = true);
bool ParseNetspeed(const uint8_t* data, size_t len, NetspeedMessage& out, bool expectType = true);
bool ParseLogin(const uint8_t* data, size_t len, LoginMessage& out, bool expectType = true);
bool ParseWelcome(const uint8_t* data, size_t len, WelcomeMessage& out, bool expectType = true);
bool ParseFailure(const uint8_t* data, size_t len, FailureMessage& out, bool expectType = true);
bool ParseUpgrade(const uint8_t* data, size_t len, UpgradeMessage& out, bool expectType = true);
bool ParseJoin(const uint8_t* data, size_t len, JoinMessage& out, bool expectType = true);

// Peek the leading message-type byte of a payload without fully parsing.
// Returns false if the buffer is empty.
bool PeekType(const uint8_t* data, size_t len, NMT& outType);

// ---------------------------------------------------------------------------
//  TODO (spec §3, §9): UE3 packet/bunch framing.
//  The functions above operate on the BUNCH PAYLOAD only. Wrapping a payload in
//  an open-control-channel-0 bunch inside an FBitWriter packet (PacketId, ack
//  bitfield, bControl/bOpen/bClose/bReliable, ChIndex, ChSequence, ChType,
//  BunchDataBits, and the version-dependent bHasReferencedGUIDs bit) is NOT
//  implemented here because those bits are [UE3]/[?] and MUST be validated by a
//  real packet capture before they can be trusted. Stream R2's capture pins
//  them; framing then layers cleanly on top of this codec.
// ---------------------------------------------------------------------------

} // namespace ControlChannel
