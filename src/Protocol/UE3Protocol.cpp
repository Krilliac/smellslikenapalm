// src/Protocol/UE3Protocol.cpp
#include "Protocol/UE3Protocol.h"
#include <cstring>

// Constructor initializes channel and sequences
UE3Protocol::UE3Protocol(uint8_t channelIndex)
    : m_channelIndex(channelIndex), m_outSeq(0), m_inSeq(0) {}

UE3Protocol::~UE3Protocol() = default;

std::vector<uint8_t> UE3Protocol::BuildBunch(
    const uint8_t* payload, size_t payloadLen,
    bool reliable, bool openChannel, bool closeChannel)
{
    // Header is 1 + 2 + 1 + 1 + 1 + 2 = 8 bytes
    std::vector<uint8_t> buf;
    buf.reserve(8 + payloadLen);

    // ChannelIndex
    WriteUInt8(buf, m_channelIndex);
    // Sequence
    WriteUInt16(buf, m_outSeq++);
    // Flags byte: bit0=reliable, bit1=open, bit2=close
    uint8_t flags = 0;
    if (reliable)    flags |= 1 << 0;
    if (openChannel) flags |= 1 << 1;
    if (closeChannel)flags |= 1 << 2;
    WriteUInt8(buf, flags);
    // Padding (unused)
    WriteUInt8(buf, 0);
    // Payload length
    WriteUInt16(buf, static_cast<uint16_t>(payloadLen));
    // Payload
    buf.insert(buf.end(), payload, payload + payloadLen);
    return buf;
}

bool UE3Protocol::ParseBunch(
    const uint8_t* packetData, size_t packetLen,
    UE3BunchHeader& outHeader,
    const uint8_t*& outPayload, size_t& outPayloadLen)
{
    if (packetLen < 8) return false;
    size_t offset = 0;
    outHeader.ChannelIndex = ReadUInt8(packetData, offset);
    outHeader.ChSequence   = ReadUInt16(packetData, offset);
    uint8_t flags          = ReadUInt8(packetData, offset);
    // skip padding
    offset += 1;
    outHeader.Reliable     = (flags & (1 << 0)) != 0;
    outHeader.Open         = (flags & (1 << 1)) != 0;
    outHeader.Close        = (flags & (1 << 2)) != 0;
    outHeader.PayloadLen   = ReadUInt16(packetData, offset);
    if (offset + outHeader.PayloadLen > packetLen) return false;
    outPayload    = packetData + offset;
    outPayloadLen = outHeader.PayloadLen;
    return true;
}

void UE3Protocol::SendBunch(
    const uint8_t* payload, size_t payloadLen,
    bool reliable, bool open, bool close,
    SendFunc sender)
{
    auto packet = BuildBunch(payload, payloadLen, reliable, open, close);
    sender(packet.data(), packet.size());
}

// Serialization helpers
void UE3Protocol::WriteUInt8(std::vector<uint8_t>& buf, uint8_t v) {
    buf.push_back(v);
}

void UE3Protocol::WriteUInt16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void UE3Protocol::WriteFloat(std::vector<uint8_t>& buf, float f) {
    uint8_t bytes[4];
    std::memcpy(bytes, &f, 4);
    buf.insert(buf.end(), bytes, bytes + 4);
}

uint8_t UE3Protocol::ReadUInt8(const uint8_t* data, size_t& offset) {
    return data[offset++];
}

uint16_t UE3Protocol::ReadUInt16(const uint8_t* data, size_t& offset) {
    uint16_t lo = data[offset++];
    uint16_t hi = data[offset++];
    return lo | (hi << 8);
}

float UE3Protocol::ReadFloat(const uint8_t* data, size_t& offset) {
    float f;
    std::memcpy(&f, data + offset, 4);
    offset += 4;
    return f;
}