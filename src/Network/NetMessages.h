// src/Network/NetMessages.h
//
// SINGLE SOURCE OF TRUTH for every UE3 control-channel WIRE CONSTANT used by the
// RS2V emulator: the NMT_ control-message ordinals and the version / netspeed
// integers. Everything here is isolated in ONE place on purpose so the codec can
// be corrected with a single edit without touching any message-building logic.
//
// Reverse-engineering sources:
//   - RS2V_ControlChannel_WireSpec_7258.md  (Stream R)
//   - Stream R2 jump-table dump of UNetConnection::ReceivedBunch in VNGame.exe
// Target: Rising Storm 2: Vietnam dedicated server, UE3 EngineVersion 7258,
// the EGS/EOS "Leech" build.
//
// Confidence legend:
//   [CB]      Confirmed-from-binary (R2 jump-table handler has a unique log
//             string, or the version-compare, pinning value -> message).
//   [CB-anc]  Confirmed-by-anchor: canonical slots 0..6 are UNCHANGED in RS2.
//             R2 confirmed Hello=0 / Netspeed=4 / Login=5 at their canonical
//             values, and bytes 1,2,3,6 fall to the server's default case (i.e.
//             they are the client-received Welcome/Upgrade/Challenge/Failure at
//             their stock positions). RS2 only *inserted* messages AFTER slot 6.
//   [?]       Uncertain.
//
// =====================================================================
//  ORDINALS PINNED FROM R2's JUMP-TABLE DUMP. The client->server messages the
//  emulator must RECEIVE are all [CB]. The server->client messages the emulator
//  must SEND (Welcome/Upgrade/Challenge/Failure) are [CB-anc] (high confidence,
//  but the *client's* receive switch was not dumped -- a packet capture or a
//  client-side dump would upgrade these from [CB-anc] to [CB]).
// =====================================================================

#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// NMT_ control-message types. Dispatched by a single BYTE switch in
// UNetConnection::ReceivedBunch, range 0x00..0x25 (38 cases) [CB].
// ---------------------------------------------------------------------------
enum class NMT : uint8_t {
    // --- Canonical block 0..6 (UNCHANGED in RS2) -----------------------------
    Hello     = 0,   // C->S  {BYTE bLittleEndian, INT MinVer, INT Ver, QWORD SteamId,
                     //        FStr LeechSessionId, FStr token}                 [CB]
    Welcome   = 1,   // S->C  {FStr Map, FStr GameClass, FStr Redirect}         [CB-anc]
    Upgrade   = 2,   // S->C  {INT RemoteMinVer}                                [CB-anc]
    Challenge = 3,   // S->C  {FStr ServerNonce}                                [CB-anc]
    Netspeed  = 4,   // C->S  {INT Netspeed}  (server clamps; "Client netspeed is %i") [CB]
    Login     = 5,   // C->S  {FStr ClientResponse, FStr URL}  ("Login request: %s")   [CB]
    Failure   = 6,   // S->C  {FStr ErrorKey}                                   [CB-anc]

    // --- RS2 inserted messages after slot 6 (this is why Join is 9, not 7) ---
    JoinGuidRebind = 8,  // C->S  Join-family GUID-rebind path, SpawnPlayActor; no
                         //       log string. Distinct from Join (0x09). [CB presence, name ?]
    Join      = 9,   // C->S  (empty/URL) -> PostLogin/spawn  ("Join request: %s")   [CB]
    JoinSplit = 10,  // C->S  split-screen  ("JOINSPLIT: Join request: URL=%s")       [CB]
    Skip      = 12,  // C->S  ("User skipped download of '%s'")                        [CB]
    Abort     = 13,  // C->S  ("Received ABORT with invalid GUID %s")                  [CB]
    PCSwap    = 15,  // S<->C ("Received invalid swap message with child index %i")    [CB]
    DebugText = 20,  // C<->S ("%s received NMT_DebugText ...")                         [CB]

    // --- RS2 Steam/EOS + peer extensions (confirmed handler positions) -------
    SteamLogin  = 16, // C->S  SteamId + dup-IP dedup ("Ignoring old connection ... SteamId (%I64u)") [CB]
    SteamAuth   = 18, // C->S  raw-socket Steam auth ticket; "Engine.Errors.SteamAuthFailed"          [CB]
    PeerListen  = 21, // C->S  ("UWorld: received NMT_PeerListen. Peer connection str=%s")            [CB]
    SessionId   = 34, // C->S  ("SessionIdReceivedFromClient")                                        [CB]
    PeerHostExt = 37, // C->S  peer host-migration {FStr, INT}; no log string                  [CB presence]
};

// Convenience: the raw byte value of an NMT (the on-wire message-type byte).
constexpr uint8_t NMTByte(NMT t) { return static_cast<uint8_t>(t); }

// Dispatcher accepts message-type bytes in [0, kNMTMaxCase] inclusive [CB]
// (cmp eax,0x25; ja default @ 0x14096c29a). 0x25 == 37.
constexpr uint8_t kNMTMaxCase = 0x25;

// ---------------------------------------------------------------------------
// Version / build constants. The version integers are [CB] (data block at
// 0x14146b990 in VNGame.exe); the Hello handler compares the client's Version
// against kMinNetVersion (cmp ecx,[0x14146b99c]).
// ---------------------------------------------------------------------------
constexpr int32_t kEngineVersion = 7258;   // GEngineVersion        (0x1C5A)  [CB]
constexpr int32_t kMinNetVersion = 7038;   // GEngineMinNetVersion  (0x1B7E)  [CB]
constexpr int32_t kChangelist    = 230264; // GBuiltFromChangeList  (0x38578) [CB]

// Adjacent data-block field 0x14146b990 == 1101 (0x44D): GEngineNetVersion or a
// Perforce branch id - purpose [?]. Exposed for completeness; not used on wire.
constexpr int32_t kEngineNetVersionOrBranch = 1101; // [?]

// ---------------------------------------------------------------------------
// Client default netspeeds [CB] (UnrealScript Player.uc defaultproperties).
// Sent as the INT body of NMT_Netspeed.
// ---------------------------------------------------------------------------
constexpr int32_t kNetspeedInternet = 80000; // ConfiguredInternetSpeed [CB]
constexpr int32_t kNetspeedLAN      = 20000; // ConfiguredLanSpeed      [CB]

// ---------------------------------------------------------------------------
// Bunch / channel framing constants [UE3] (spec §3). Used by SerializeInt-bounded
// fields in the packet/bunch header. The header layout itself is version-sensitive
// and TODO-marked in ControlChannel.cpp; these maxima are the standard UE3 values.
// ---------------------------------------------------------------------------
constexpr int32_t kMaxPacketId = 16384;  // MAX_PACKETID  (14-bit packet seq)   [UE3]
constexpr int32_t kMaxChannels = 1024;   // MAX_CHANNELS (power-of-two)         [UE3]
// MUST be 1024, not 1023: UE3 frames ChIndex via SerializeInt(ChIndex, MAX_CHANNELS), which
// is exactly 10 bits for all 0..1023 ONLY when the bound is a power of two. With 1023,
// ChIndex>=511 encodes in 9 bits instead of 10 and desyncs the bunch header on the wire.
// (Today the bootstrap uses ch0..140 where 1023/1024 are bit-identical, so this was latent.)
constexpr uint8_t kControlChannelIndex = 0; // control channel is index 0       [UE3]

// Channel types (SerializeInt(CHTYPE_MAX)) [UE3].
enum class ChType : uint8_t {
    Control = 0,
    Actor   = 1,
    File    = 2,
    Voice   = 3,
};

// Well-known NMT_Failure error keys [CB] (string constants in VNGame.exe).
namespace NetFailureKeys {
    // Sent when Hello arrives without a usable Steam client/ticket @ 0x14115a390.
    constexpr const char* SteamClientRequired = "Engine.Errors.SteamClientRequired";
    // Steam auth ticket validation failure (handler 0x12).
    constexpr const char* SteamAuthFailed = "Engine.Errors.SteamAuthFailed";
}
