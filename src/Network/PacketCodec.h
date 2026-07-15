// src/Network/PacketCodec.h
//
// UE3 (RS2 EngineVersion 7258) packet + bunch FRAMING codec. This is the layer
// that sits between a raw UDP datagram and the control-channel message codec
// (ControlChannel.h): it decodes the <PacketId><ack...><bunch...> wire structure
// into bunches, and encodes the reverse. It does NOT interpret NMT message
// bodies - that is ControlChannel's job.
//
// The wire format is pinned in docs/RS2V_ControlChannel_WireSpec_7258.md
// (reversed from VNGame.exe + validated against a real handshake capture). All
// bit IO is LSB-first and uses BitReader/BitWriter (UE3 FBitReader/FBitWriter
// compatible). Bounded ints use SerializeInt (UE3 ReadInt/WriteInt).

#pragma once

#include <cstdint>
#include <vector>

#include "Network/NetMessages.h"

namespace PacketCodec {

// ---- Wire constants (spec §2/§3) ------------------------------------------
// kMaxPacketId (16384) and kMaxChannels (1024) live in NetMessages.h.
constexpr uint32_t kMaxChSequence = 1024;   // ReadInt bound for ChSequence (0x400)
constexpr uint32_t kChTypeMax     = 8;      // ReadInt bound for ChType (CHTYPE_MAX)
constexpr uint32_t kControlChannelType = 1; // ChType for the control channel [UE3]

// BunchDataBits = ReadInt(MaxPacket*8). MaxPacket is 8 during the control
// handshake => bound 64 (so a control bunch carries <=63 data bits and large
// messages fragment across many reliable bunches). See spec §3.
// MaxPacket is phase-dependent on the wire: 8 during the StatelessConnect
// handshake (tiny bunches), but it GROWS once the connection is established and
// the NMT phase begins (the real client's Hello bunch is 504 data bits, Login is
// 6256). BunchDataBits = SerializeInt(MaxPacket*8), so the SerializeInt WIDTH -
// and thus where the payload starts - changes with MaxPacket. The caller passes
// the right value per connection phase (see PacketCodec::Decode/Encode).
constexpr uint32_t kHandshakeMaxPacketBytes = 8;     // StatelessConnect phase
// Retail RS2 C2S traffic uses MaxPacket=1280 (BunchDataBits bound 10240). This is
// pinned by the package-inventory burst: saturated 1258-1279 byte datagrams decode
// exactly into consecutive ch0 NMT_Have bunches only at 10240. The old 2048-byte
// bound misaligned those same packets, fabricated actor channels (notably ch512),
// and left reliable ch0 sequence gaps that prevented the eventual NMT_Join.
// Small handshake/Login packets can happen to decode at several 14-bit bounds, so
// the saturated post-login packets are the authoritative discriminator.
constexpr uint32_t kClientSendMaxPacketBytes = 1280;  // client -> server decode
// Compatibility name used throughout the established-phase tests/callers.
constexpr uint32_t kNmtMaxPacketBytes = kClientSendMaxPacketBytes;

// MaxPacket is per-direction and ASYMMETRIC on this build: the CLIENT encodes its
// C2S bunches with MaxPacket 1280, while the dedicated SERVER encodes
// its S2C bunches with a smaller MaxPacket ~1500 (Ethernet MTU; bound ~12000) -
// reversed from the official server's PackageMap export (frames f167-f185), whose
// 20 bunches only decode with consumesAll when the bound is in (10088, 13928].
// So we DECODE inbound at kClientSendMaxPacketBytes but ENCODE our outbound (we are the
// server) at this value, matching what the retail client expects from a server.
// Any value in (1261, 1741] bytes frames the PackageMap chunks identically; 1500
// is the principled MTU choice. See docs/RS2V_PostJoin_Replication_7258.md.
constexpr uint32_t kServerSendMaxPacketBytes = 1500;  // established / NMT phase (server->CLIENT encode)
constexpr uint32_t kBunchDataBitsMax = kHandshakeMaxPacketBytes * 8; // 64 (handshake default)

// One decoded bunch. `payload` holds the bunch data bits packed LSB-first (the
// same layout BitReader/BitWriter use); `payloadBits` is the exact bit count.
struct Bunch {
    bool bControl = false;
    bool bOpen = false;
    bool bClose = false;
    bool bReliable = false;
    uint32_t chIndex = 0;
    uint32_t chSequence = 0;   // meaningful only when bReliable
    uint32_t chType = 0;       // meaningful only when (bReliable || bOpen)
    std::vector<uint8_t> payload;
    uint32_t payloadBits = 0;
};

// One decoded packet.
struct Packet {
    uint32_t packetId = 0;
    std::vector<uint32_t> acks;     // AckPacketIds, in wire order
    std::vector<Bunch> bunches;     // bunches, in wire order
    bool ok = false;                // false if the datagram was malformed/overflowed
};

// Decode a raw UDP datagram (one UE3 packet) into its structure. `maxPacketBytes`
// is the connection's MaxPacket for the current phase (kHandshakeMaxPacketBytes
// during StatelessConnect, kClientSendMaxPacketBytes once established) - it sets the
// SerializeInt bound for BunchDataBits. Never reads out of bounds; sets
// Packet::ok = false on any malformed/truncated input.
Packet Decode(const uint8_t* data, size_t numBytes,
              uint32_t maxPacketBytes = kHandshakeMaxPacketBytes);

// Encode a packet (PacketId, then acks, then bunches, then the terminator '1'
// bit + zero pad to a byte boundary) into raw wire bytes. `maxPacketBytes` must
// match the phase the peer will decode with. The inverse of Decode.
std::vector<uint8_t> Encode(const Packet& pkt,
                            uint32_t maxPacketBytes = kHandshakeMaxPacketBytes);

// ---- Debug observability -------------------------------------------------
// When enabled, Decode/Encode emit a per-bunch structured trace (packetId,
// chIndex, bControl/bOpen/bClose/bReliable, chSequence, chType, payloadBits and
// a hex dump of the payload) plus NON-FATAL invariant warnings (payloadBits
// exceeding maxPacketBytes*8, unexpected leftover bits, or a malformed datagram
// that yields ok=false) through the project Logger. This is observability only:
// it NEVER alters the wire bytes, the parse result, or any control flow. Off by
// default; toggle at runtime (e.g. from a debug console / config flag).
void SetDebugTracing(bool enabled);
bool IsDebugTracing();

} // namespace PacketCodec
