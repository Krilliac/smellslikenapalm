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
// kMaxPacketId (16384) and kMaxChannels (1023) live in NetMessages.h.
constexpr uint32_t kMaxChSequence = 1024;   // ReadInt bound for ChSequence (0x400)
constexpr uint32_t kChTypeMax     = 8;      // ReadInt bound for ChType (CHTYPE_MAX)
constexpr uint32_t kControlChannelType = 1; // ChType for the control channel [UE3]

// BunchDataBits = ReadInt(MaxPacket*8). MaxPacket is 8 during the control
// handshake => bound 64 (so a control bunch carries <=63 data bits and large
// messages fragment across many reliable bunches). See spec §3.
constexpr uint32_t kHandshakeMaxPacketBytes = 8;
constexpr uint32_t kBunchDataBitsMax = kHandshakeMaxPacketBytes * 8; // 64

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

// Decode a raw UDP datagram (one UE3 packet) into its structure. Never reads
// out of bounds; sets Packet::ok = false on any malformed/truncated input.
Packet Decode(const uint8_t* data, size_t numBytes);

// Encode a packet (PacketId, then acks, then bunches, then the terminator '1'
// bit + zero pad to a byte boundary) into raw wire bytes. The inverse of Decode:
// decoding a frame and re-encoding the result must reproduce the original bytes.
std::vector<uint8_t> Encode(const Packet& pkt);

} // namespace PacketCodec
