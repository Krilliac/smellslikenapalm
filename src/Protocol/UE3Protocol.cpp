// src/Protocol/UE3Protocol.cpp
#include "Protocol/UE3Protocol.h"
#include "Utils/Logger.h"
#include <cstring>

// Constructor initializes channel and sequences
UE3Protocol::UE3Protocol(uint8_t channelIndex)
    : m_channelIndex(channelIndex), m_outSeq(0), m_inSeq(0)
{
    Logger::Trace("[UE3Protocol::UE3Protocol] entry — channelIndex=%u", channelIndex);
    Logger::Debug("[UE3Protocol::UE3Protocol] initialized with channelIndex=%u, outSeq=0, inSeq=0", channelIndex);
    Logger::Trace("[UE3Protocol::UE3Protocol] exit — construction complete");
}

UE3Protocol::~UE3Protocol() {
    Logger::Trace("[UE3Protocol::~UE3Protocol] entry — channelIndex=%u, outSeq=%u, inSeq=%u",
                  m_channelIndex, m_outSeq, m_inSeq);
    Logger::Debug("[UE3Protocol::~UE3Protocol] destroying UE3Protocol for channel %u — final outSeq=%u, inSeq=%u",
                  m_channelIndex, m_outSeq, m_inSeq);
    Logger::Trace("[UE3Protocol::~UE3Protocol] exit — destruction complete");
}

std::vector<uint8_t> UE3Protocol::BuildBunch(
    const uint8_t* payload, size_t payloadLen,
    bool reliable, bool openChannel, bool closeChannel)
{
    Logger::Trace("[UE3Protocol::BuildBunch] entry — payloadLen=%zu, reliable=%s, openChannel=%s, closeChannel=%s",
                  payloadLen, reliable ? "true" : "false",
                  openChannel ? "true" : "false", closeChannel ? "true" : "false");
    // Header is 1 + 2 + 1 + 1 + 1 + 2 = 8 bytes
    std::vector<uint8_t> buf;
    buf.reserve(8 + payloadLen);
    Logger::Debug("[UE3Protocol::BuildBunch] reserved buffer of %zu bytes (header=8 + payload=%zu)",
                  8 + payloadLen, payloadLen);

    // ChannelIndex
    WriteUInt8(buf, m_channelIndex);
    Logger::Trace("[UE3Protocol::BuildBunch] wrote channelIndex=%u", m_channelIndex);
    // Sequence
    uint16_t seq = m_outSeq++;
    WriteUInt16(buf, seq);
    Logger::Trace("[UE3Protocol::BuildBunch] wrote sequence=%u (outSeq incremented to %u)", seq, m_outSeq);
    // Flags byte: bit0=reliable, bit1=open, bit2=close
    uint8_t flags = 0;
    if (reliable)    flags |= 1 << 0;
    if (openChannel) flags |= 1 << 1;
    if (closeChannel)flags |= 1 << 2;
    WriteUInt8(buf, flags);
    Logger::Debug("[UE3Protocol::BuildBunch] computed flags=0x%02X (reliable=%s, open=%s, close=%s)",
                  flags, reliable ? "true" : "false",
                  openChannel ? "true" : "false", closeChannel ? "true" : "false");
    // Padding (unused)
    WriteUInt8(buf, 0);
    Logger::Trace("[UE3Protocol::BuildBunch] wrote padding byte=0");
    // Payload length
    WriteUInt16(buf, static_cast<uint16_t>(payloadLen));
    Logger::Trace("[UE3Protocol::BuildBunch] wrote payloadLen=%u", static_cast<uint16_t>(payloadLen));
    // Payload
    buf.insert(buf.end(), payload, payload + payloadLen);
    Logger::Debug("[UE3Protocol::BuildBunch] appended %zu bytes of payload data", payloadLen);
    Logger::Info("[UE3Protocol::BuildBunch] built bunch on channel %u, seq=%u, flags=0x%02X, payloadLen=%zu, totalSize=%zu",
                 m_channelIndex, seq, flags, payloadLen, buf.size());
    Logger::Trace("[UE3Protocol::BuildBunch] exit — returning buffer of %zu bytes", buf.size());
    return buf;
}

bool UE3Protocol::ParseBunch(
    const uint8_t* packetData, size_t packetLen,
    UE3BunchHeader& outHeader,
    const uint8_t*& outPayload, size_t& outPayloadLen)
{
    Logger::Trace("[UE3Protocol::ParseBunch] entry — packetData=%p, packetLen=%zu",
                  (const void*)packetData, packetLen);
    if (packetLen < 8) {
        Logger::Warn("[UE3Protocol::ParseBunch] packet too short: packetLen=%zu < minimum 8 bytes", packetLen);
        Logger::Trace("[UE3Protocol::ParseBunch] exit — returning false (packet too short)");
        return false;
    }
    size_t offset = 0;
    outHeader.ChannelIndex = ReadUInt8(packetData, offset);
    Logger::Trace("[UE3Protocol::ParseBunch] read ChannelIndex=%u at offset=0", outHeader.ChannelIndex);
    outHeader.ChSequence   = ReadUInt16(packetData, offset);
    Logger::Trace("[UE3Protocol::ParseBunch] read ChSequence=%u at offset=1", outHeader.ChSequence);
    uint8_t flags          = ReadUInt8(packetData, offset);
    Logger::Trace("[UE3Protocol::ParseBunch] read flags=0x%02X at offset=3", flags);
    // skip padding
    offset += 1;
    Logger::Trace("[UE3Protocol::ParseBunch] skipped padding byte at offset=4");
    outHeader.Reliable     = (flags & (1 << 0)) != 0;
    outHeader.Open         = (flags & (1 << 1)) != 0;
    outHeader.Close        = (flags & (1 << 2)) != 0;
    Logger::Debug("[UE3Protocol::ParseBunch] parsed flags: Reliable=%s, Open=%s, Close=%s",
                  outHeader.Reliable ? "true" : "false",
                  outHeader.Open ? "true" : "false",
                  outHeader.Close ? "true" : "false");
    outHeader.PayloadLen   = ReadUInt16(packetData, offset);
    Logger::Trace("[UE3Protocol::ParseBunch] read PayloadLen=%u at offset=5", outHeader.PayloadLen);
    if (offset + outHeader.PayloadLen > packetLen) {
        Logger::Error("[UE3Protocol::ParseBunch] payload extends beyond packet: offset=%zu + PayloadLen=%u = %zu > packetLen=%zu",
                      offset, outHeader.PayloadLen, offset + outHeader.PayloadLen, packetLen);
        Logger::Trace("[UE3Protocol::ParseBunch] exit — returning false (payload overflow)");
        return false;
    }
    outPayload    = packetData + offset;
    outPayloadLen = outHeader.PayloadLen;
    Logger::Debug("[UE3Protocol::ParseBunch] successfully parsed bunch: channel=%u, seq=%u, reliable=%s, open=%s, close=%s, payloadLen=%zu",
                  outHeader.ChannelIndex, outHeader.ChSequence,
                  outHeader.Reliable ? "true" : "false",
                  outHeader.Open ? "true" : "false",
                  outHeader.Close ? "true" : "false",
                  outPayloadLen);
    Logger::Info("[UE3Protocol::ParseBunch] parsed bunch on channel %u, seq=%u, payloadLen=%zu from %zu-byte packet",
                 outHeader.ChannelIndex, outHeader.ChSequence, outPayloadLen, packetLen);
    Logger::Trace("[UE3Protocol::ParseBunch] exit — returning true");
    return true;
}

void UE3Protocol::SendBunch(
    const uint8_t* payload, size_t payloadLen,
    bool reliable, bool open, bool close,
    SendFunc sender)
{
    Logger::Trace("[UE3Protocol::SendBunch] entry — payloadLen=%zu, reliable=%s, open=%s, close=%s, sender=%s",
                  payloadLen, reliable ? "true" : "false",
                  open ? "true" : "false", close ? "true" : "false",
                  sender ? "valid" : "null");
    auto packet = BuildBunch(payload, payloadLen, reliable, open, close);
    Logger::Debug("[UE3Protocol::SendBunch] built bunch of %zu bytes — invoking sender callback", packet.size());
    sender(packet.data(), packet.size());
    Logger::Info("[UE3Protocol::SendBunch] sent bunch on channel %u: %zu bytes payload, %zu bytes total, reliable=%s",
                 m_channelIndex, payloadLen, packet.size(), reliable ? "true" : "false");
    Logger::Trace("[UE3Protocol::SendBunch] exit — send complete");
}

// Serialization helpers
void UE3Protocol::WriteUInt8(std::vector<uint8_t>& buf, uint8_t v) {
    Logger::Trace("[UE3Protocol::WriteUInt8] writing value=%u (0x%02X), buf size before=%zu", v, v, buf.size());
    buf.push_back(v);
}

void UE3Protocol::WriteUInt16(std::vector<uint8_t>& buf, uint16_t v) {
    Logger::Trace("[UE3Protocol::WriteUInt16] writing value=%u (0x%04X) as little-endian, buf size before=%zu",
                  v, v, buf.size());
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void UE3Protocol::WriteFloat(std::vector<uint8_t>& buf, float f) {
    Logger::Trace("[UE3Protocol::WriteFloat] writing float=%.6f, buf size before=%zu", f, buf.size());
    uint8_t bytes[4];
    std::memcpy(bytes, &f, 4);
    buf.insert(buf.end(), bytes, bytes + 4);
}

uint8_t UE3Protocol::ReadUInt8(const uint8_t* data, size_t& offset) {
    uint8_t val = data[offset++];
    Logger::Trace("[UE3Protocol::ReadUInt8] read value=%u (0x%02X) at offset=%zu", val, val, offset - 1);
    return val;
}

uint16_t UE3Protocol::ReadUInt16(const uint8_t* data, size_t& offset) {
    uint16_t lo = data[offset++];
    uint16_t hi = data[offset++];
    uint16_t val = lo | (hi << 8);
    Logger::Trace("[UE3Protocol::ReadUInt16] read value=%u (0x%04X) as little-endian at offset=%zu",
                  val, val, offset - 2);
    return val;
}

float UE3Protocol::ReadFloat(const uint8_t* data, size_t& offset) {
    float f;
    std::memcpy(&f, data + offset, 4);
    Logger::Trace("[UE3Protocol::ReadFloat] read float=%.6f at offset=%zu", f, offset);
    offset += 4;
    return f;
}
