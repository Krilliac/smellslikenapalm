// src/Network/PacketAssembler.h
//
// Per-connection OUTBOUND packet assembler for the UE3 (RS2 EngineVersion 7258)
// control channel. This is the SEND-side counterpart to PacketCodec::Decode: it
// takes raw control-channel MESSAGE PAYLOADS (a <BYTE NMT><fields> byte buffer,
// exactly what ControlChannel::Build* produces) and turns them into one or more
// PacketCodec::Packet structs, ready to hand to PacketCodec::Encode for emission.
//
// It owns the per-connection outbound SEQUENCING state:
//   * nextPacketId        — PacketId stamped on each produced packet (wraps at
//                           kMaxPacketId; spec §2).
//   * nextControlChSequence — ChSequence stamped on each reliable control bunch
//                           (starts at 1; spec §3/§5).
//   * channelOpened       — whether the control channel has been opened yet (the
//                           very first control bunch carries bControl+bOpen).
//   * pendingAcks         — inbound PacketIds we have received and must ack.
//
// Fragmentation (spec §3): MaxPacket is 8 during the control handshake, so a
// control bunch carries at most kBunchDataBitsMax-1 == 63 data bits. Large
// messages are split across many small reliable bunches, reassembled in
// ChSequence order by the receiver. We read the message payload as an LSB-first
// bit stream and emit <=63-bit fragments, one bunch per packet.
//
// Wire format authority: docs/RS2V_ControlChannel_WireSpec_7258.md.

#pragma once

#include <cstdint>
#include <vector>

#include "Network/PacketCodec.h"

namespace PacketCodec {

class PacketAssembler {
public:
    PacketAssembler() = default;

    // Record an inbound PacketId that must be acknowledged on a future outbound
    // packet. Drained into the produced Packet(s) by the next Build* call (or by
    // BuildAckOnlyPacket).
    void QueueAck(uint32_t packetId);

    // Turn one control-channel message payload (raw <BYTE NMT><fields> bytes)
    // into the outbound packets that carry it. The payload bits are fragmented
    // into reliable control bunches of <=63 data bits each, with consecutive
    // ChSequence values; each bunch goes in its own packet (one-bunch-per-packet,
    // matching the small-MaxPacket handshake regime). The FIRST bunch of the very
    // first control message opens the channel (bControl=true, bOpen=true); all
    // later bunches have bControl=false, bOpen=false. Any queued acks are drained
    // onto the first produced packet. Packets are returned in send order.
    //
    // An empty payload still produces exactly one packet carrying a single
    // zero-bit bunch (so the channel-open / ack still goes out).
    //
    // `maxBunchDataBits` is the largest payload bit count one bunch may carry,
    // i.e. MaxPacket*8 - 1 for the phase. It defaults to the small handshake
    // value (63, from MaxPacket=8); once the connection is established the caller
    // passes the NMT value (kNmtMaxPacketBytes*8 - 1 = 16383) so a large message
    // - e.g. NMT_Welcome or the PackageMap export - goes out as ONE big bunch,
    // exactly as the retail server frames it, instead of hundreds of 63-bit
    // fragments. The BunchDataBits SerializeInt bound used when ENCODING must
    // match (see PacketCodec::Encode's maxPacketBytes) or the peer mis-reads it.
    std::vector<Packet> BuildControlMessagePackets(
        const std::vector<uint8_t>& messagePayload,
        uint32_t maxBunchDataBits = kBunchDataBitsMax - 1);

    // Flush any pending acks in a bunch-less packet (PacketId + acks only). Useful
    // when we must acknowledge received packets without sending data. Always
    // produces a packet (with an incremented PacketId) even if no acks are queued.
    Packet BuildAckOnlyPacket();

    // Wrap ONE fully-specified bunch in its own packet (next PacketId, queued acks
    // drained onto it). Unlike BuildControlMessagePackets this does NOT touch the
    // control-channel sequence or fragment - the caller owns every bunch field
    // (chIndex, chType, bOpen/bClose/bReliable, chSequence, payload). Used to open
    // bootstrap ACTOR channels (ChIndex>=2, ChType 2, bOpen) whose per-channel
    // sequence is independent of the control channel.
    Packet BuildRawBunchPacket(const Bunch& bunch);

    // ---- state accessors (for tests / integration) ----
    uint32_t NextPacketId() const { return m_nextPacketId; }
    uint32_t NextControlChSequence() const { return m_nextControlChSequence; }
    bool IsChannelOpened() const { return m_channelOpened; }
    size_t PendingAckCount() const { return m_pendingAcks.size(); }

private:
    // Allocate the next PacketId, wrapping at kMaxPacketId (spec §2).
    uint32_t AllocatePacketId();

    // Move all pending acks into `pkt.acks` and clear the pending list.
    void DrainAcksInto(Packet& pkt);

    uint32_t m_nextPacketId = 0;          // spec §2: starts 0, wraps at kMaxPacketId
    uint32_t m_nextControlChSequence = 1; // spec §3/§5: reliable bunch seq starts 1
    bool m_channelOpened = false;         // first control bunch opens the channel
    std::vector<uint32_t> m_pendingAcks;  // inbound PacketIds awaiting ack
};

} // namespace PacketCodec
