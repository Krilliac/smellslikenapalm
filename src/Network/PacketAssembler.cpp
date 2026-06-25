// src/Network/PacketAssembler.cpp
//
// Implementation of the outbound control-channel packet assembler. See
// PacketAssembler.h for the contract and spec citations
// (docs/RS2V_ControlChannel_WireSpec_7258.md §2/§3/§5).

#include "Network/PacketAssembler.h"

#include "Network/BitWriter.h"
#include "Network/NetMessages.h"

namespace PacketCodec {

namespace {

// Pack `bitCount` bits starting at bit offset `startBit` of `bytes` (LSB-first,
// 8 bits/byte) into a fresh LSB-first buffer via BitWriter. Returns the packed
// bytes with the trailing partial byte zero-padded — the exact layout a Bunch
// payload uses.
std::vector<uint8_t> PackFragment(const std::vector<uint8_t>& bytes,
                                  size_t startBit, uint32_t bitCount) {
    BitWriter w;
    for (uint32_t i = 0; i < bitCount; ++i) {
        const size_t absBit = startBit + i;
        const size_t byteIndex = absBit >> 3;
        const int bitInByte = static_cast<int>(absBit & 7);
        const bool bit =
            byteIndex < bytes.size() &&
            ((bytes[byteIndex] >> bitInByte) & 1u) != 0;
        w.WriteBit(bit);
    }
    return w.GetBytes();
}

} // namespace

void PacketAssembler::QueueAck(uint32_t packetId) {
    m_pendingAcks.push_back(packetId);
}

uint32_t PacketAssembler::AllocatePacketId() {
    const uint32_t id = m_nextPacketId;
    m_nextPacketId = (m_nextPacketId + 1) % static_cast<uint32_t>(kMaxPacketId);
    return id;
}

void PacketAssembler::DrainAcksInto(Packet& pkt) {
    pkt.acks = std::move(m_pendingAcks);
    m_pendingAcks.clear();
}

std::vector<Packet> PacketAssembler::BuildControlMessagePackets(
    const std::vector<uint8_t>& messagePayload,
    uint32_t maxBunchDataBits) {
    std::vector<Packet> packets;

    const size_t totalBits = messagePayload.size() * 8u;
    const uint32_t maxFragmentBits = maxBunchDataBits > 0 ? maxBunchDataBits : 1u;

    // Compute fragment boundaries. An empty payload still yields exactly one
    // (zero-bit) bunch so the channel-open and any acks still go out.
    size_t bitOffset = 0;
    bool first = true;
    do {
        const uint32_t fragBits = static_cast<uint32_t>(
            (totalBits - bitOffset) < maxFragmentBits
                ? (totalBits - bitOffset)
                : maxFragmentBits);

        Bunch b;
        // The very first control bunch on this channel opens it.
        if (first && !m_channelOpened) {
            b.bControl = true;
            b.bOpen = true;
            m_channelOpened = true;
        } else {
            b.bControl = false;
            b.bOpen = false;
        }
        b.bClose = false;
        b.bReliable = true;
        b.chIndex = kControlChannelIndex; // 0
        b.chType = kControlChannelType;   // 1
        b.chSequence = m_nextControlChSequence++;
        b.payloadBits = fragBits;
        b.payload = PackFragment(messagePayload, bitOffset, fragBits);

        Packet pkt;
        pkt.packetId = AllocatePacketId();
        pkt.ok = true;
        pkt.bunches.push_back(std::move(b));
        packets.push_back(std::move(pkt));

        bitOffset += fragBits;
        first = false;
    } while (bitOffset < totalBits);

    // Drain queued acks onto the first produced packet.
    if (!packets.empty()) {
        DrainAcksInto(packets.front());
    }

    return packets;
}

Packet PacketAssembler::BuildRawBunchPacket(const Bunch& bunch) {
    Packet pkt;
    pkt.packetId = AllocatePacketId();
    pkt.ok = true;
    pkt.bunches.push_back(bunch);
    DrainAcksInto(pkt);
    return pkt;
}

Packet PacketAssembler::BuildAckOnlyPacket() {
    Packet pkt;
    pkt.packetId = AllocatePacketId();
    pkt.ok = true;
    DrainAcksInto(pkt);
    return pkt;
}

} // namespace PacketCodec
