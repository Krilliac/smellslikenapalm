// src/Network/NetworkPacket.cpp

#include "Network/NetworkPacket.h"
#include "Utils/Logger.h"
#include "Math/Vector3.h"
#include <cstring>
#include <chrono>

Packet::Packet(const std::string& tag)
    : m_tag(tag)
{
    Logger::Trace("[Packet::Packet(tag)] Entry: tag='%s'", tag.c_str());
    Logger::Debug("[Packet::Packet(tag)] Created packet with tag='%s', empty payload", tag.c_str());
    Logger::Trace("[Packet::Packet(tag)] Exit");
}

Packet::Packet(const std::string& tag, const std::vector<uint8_t>& payload)
    : m_tag(tag), m_payload(payload)
{
    Logger::Trace("[Packet::Packet(tag,payload)] Entry: tag='%s', payload size=%zu", tag.c_str(), payload.size());
    Logger::Debug("[Packet::Packet(tag,payload)] Created packet with tag='%s', payload=%zu bytes", tag.c_str(), payload.size());
    Logger::Trace("[Packet::Packet(tag,payload)] Exit");
}

std::vector<uint8_t> Packet::Serialize() const {
    Logger::Trace("[Packet::Serialize] Entry: tag='%s', payload size=%zu", m_tag.c_str(), m_payload.size());
    // Format: [4B tag length][tag bytes][4B payload length][payload bytes]
    std::vector<uint8_t> buf;
    uint32_t tagLen = (uint32_t)m_tag.size();
    uint32_t payloadLen = (uint32_t)m_payload.size();

    buf.resize(8 + tagLen + payloadLen);
    Logger::Debug("[Packet::Serialize] Buffer size=%zu (header=8, tagLen=%u, payloadLen=%u)",
                  buf.size(), tagLen, payloadLen);
    size_t offset = 0;
    std::memcpy(&buf[offset], &tagLen, 4); offset +=4;
    std::memcpy(&buf[offset], m_tag.data(), tagLen); offset += tagLen;
    std::memcpy(&buf[offset], &payloadLen, 4); offset +=4;
    if (payloadLen) {
        std::memcpy(&buf[offset], m_payload.data(), payloadLen);
        Logger::Debug("[Packet::Serialize] Copied %u payload bytes at offset %zu", payloadLen, offset);
    } else {
        Logger::Debug("[Packet::Serialize] No payload to copy");
    }
    Logger::Trace("[Packet::Serialize] Exit: returning %zu bytes", buf.size());
    return buf;
}

Packet Packet::FromBuffer(const std::vector<uint8_t>& buffer, PacketMetadata& meta) {
    Logger::Trace("[Packet::FromBuffer] Entry: buffer size=%zu", buffer.size());
    Packet pkt;

    // Roadmap T0 hardening: fully validate every length/offset against the actual
    // buffer size BEFORE reading. The legacy format is
    //   [4B tagLen][tagLen bytes][4B payloadLen][payloadLen bytes]
    // and previously the leading bytes were trusted blindly, allowing a malformed
    // or real-client datagram to drive an out-of-bounds read. Now any
    // inconsistency yields an empty parse-failure packet instead of OOB access.
    //
    // We always populate metadata so callers can timestamp even a rejected
    // datagram. A rejected packet has an empty tag and empty payload.

    // Always fill the timestamp first so even failures carry one.
    meta.timestamp = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    auto rejectEmpty = [&](const char* why) -> Packet {
        Logger::Warn("[Packet::FromBuffer] Rejecting malformed buffer (size=%zu): %s",
                     buffer.size(), why);
        Packet empty;
        empty.m_readOffset = 0;
        meta.rawTag.clear();
        return empty;
    };

    const size_t bufSize = buffer.size();

    // Need at least the 4-byte tag-length header.
    if (bufSize < 4) {
        return rejectEmpty("buffer smaller than 4-byte tag-length header");
    }

    size_t offset = 0;
    uint32_t tagLen = 0;
    std::memcpy(&tagLen, &buffer[offset], 4);
    offset += 4;
    Logger::Debug("[Packet::FromBuffer] Read tagLen=%u", tagLen);

    // tagLen must fit within the remaining buffer AND leave room for the
    // subsequent 4-byte payload-length header. Guard against overflow in the
    // additions by checking against the remaining byte count instead of adding.
    const size_t afterTagHeader = offset; // == 4
    if (tagLen > bufSize - afterTagHeader) {
        return rejectEmpty("tag length exceeds buffer");
    }
    offset += tagLen;

    // Read tag bytes (validated to be in range).
    if (tagLen > 0) {
        pkt.m_tag.assign(reinterpret_cast<const char*>(&buffer[afterTagHeader]), tagLen);
    }
    Logger::Debug("[Packet::FromBuffer] Read tag='%s'", pkt.m_tag.c_str());

    // Need 4 bytes for the payload-length header.
    if (offset > bufSize || bufSize - offset < 4) {
        return rejectEmpty("missing payload-length header");
    }
    uint32_t payloadLen = 0;
    std::memcpy(&payloadLen, &buffer[offset], 4);
    offset += 4;
    Logger::Debug("[Packet::FromBuffer] Read payloadLen=%u", payloadLen);

    // payloadLen must fit exactly within the remaining buffer.
    if (payloadLen > bufSize - offset) {
        return rejectEmpty("payload length exceeds buffer");
    }

    if (payloadLen) {
        pkt.m_payload.assign(buffer.begin() + offset,
                             buffer.begin() + offset + payloadLen);
        Logger::Debug("[Packet::FromBuffer] Copied %u payload bytes", payloadLen);
    } else {
        Logger::Debug("[Packet::FromBuffer] No payload in packet");
    }

    meta.rawTag = pkt.m_tag;
    pkt.m_readOffset = 0;
    Logger::Debug("[Packet::FromBuffer] Metadata filled: timestamp=%llu, rawTag='%s'",
                  (unsigned long long)meta.timestamp, meta.rawTag.c_str());
    Logger::Trace("[Packet::FromBuffer] Exit: returning packet tag='%s', payload=%u bytes",
                  pkt.m_tag.c_str(), payloadLen);
    return pkt;
}

const std::string& Packet::GetTag() const {
    Logger::Trace("[Packet::GetTag] Entry/Exit: returning '%s'", m_tag.c_str());
    return m_tag;
}

uint32_t Packet::GetPayloadSize() const {
    Logger::Trace("[Packet::GetPayloadSize] Entry/Exit: returning %u", (uint32_t)m_payload.size());
    return (uint32_t)m_payload.size();
}

const std::vector<uint8_t>& Packet::RawData() const {
    Logger::Trace("[Packet::RawData] Entry/Exit: payload size=%zu", m_payload.size());
    return m_payload;
}

void Packet::WriteUInt(uint32_t v) {
    Logger::Trace("[Packet::WriteUInt] Entry: v=%u", v);
    uint8_t buf[4]; std::memcpy(buf,&v,4);
    m_payload.insert(m_payload.end(), buf, buf+4);
    Logger::Debug("[Packet::WriteUInt] Wrote %u, payload now %zu bytes", v, m_payload.size());
    Logger::Trace("[Packet::WriteUInt] Exit");
}

void Packet::WriteInt(int32_t v) {
    Logger::Trace("[Packet::WriteInt] Entry: v=%d", v);
    WriteUInt((uint32_t)v);
    Logger::Trace("[Packet::WriteInt] Exit");
}

void Packet::WriteFloat(float f) {
    Logger::Trace("[Packet::WriteFloat] Entry: f=%f", f);
    uint8_t buf[4]; std::memcpy(buf,&f,4);
    m_payload.insert(m_payload.end(), buf, buf+4);
    Logger::Debug("[Packet::WriteFloat] Wrote %f, payload now %zu bytes", f, m_payload.size());
    Logger::Trace("[Packet::WriteFloat] Exit");
}

void Packet::WriteString(const std::string& s) {
    Logger::Trace("[Packet::WriteString] Entry: s='%s' (length=%zu)", s.c_str(), s.size());
    auto enc = EncodeString(s);
    m_payload.insert(m_payload.end(), enc.begin(), enc.end());
    Logger::Debug("[Packet::WriteString] Wrote string of length %zu (%zu encoded bytes), payload now %zu bytes",
                  s.size(), enc.size(), m_payload.size());
    Logger::Trace("[Packet::WriteString] Exit");
}

void Packet::WriteVector3(const Vector3& v) {
    Logger::Trace("[Packet::WriteVector3] Entry: v=(%f, %f, %f)", v.x, v.y, v.z);
    WriteFloat(v.x); WriteFloat(v.y); WriteFloat(v.z);
    Logger::Debug("[Packet::WriteVector3] Wrote Vector3(%f, %f, %f)", v.x, v.y, v.z);
    Logger::Trace("[Packet::WriteVector3] Exit");
}

void Packet::WriteBytes(const std::vector<uint8_t>& data) {
    Logger::Trace("[Packet::WriteBytes] Entry: data size=%zu", data.size());
    m_payload.insert(m_payload.end(), data.begin(), data.end());
    Logger::Debug("[Packet::WriteBytes] Wrote %zu bytes, payload now %zu bytes", data.size(), m_payload.size());
    Logger::Trace("[Packet::WriteBytes] Exit");
}

uint32_t Packet::ReadUInt() const {
    Logger::Trace("[Packet::ReadUInt] Entry: readOffset=%zu", m_readOffset);
    uint32_t v=0;
    // Bounds-check the wire-controlled read (mirror ReadUInt32). Without this, a short
    // legacy payload causes an out-of-bounds read of m_payload[m_readOffset]. Additive.
    if (m_readOffset + 4 <= m_payload.size()) {
        std::memcpy(&v, &m_payload[m_readOffset],4);
        m_readOffset+=4;
        Logger::Debug("[Packet::ReadUInt] Read %u, readOffset now %zu", v, m_readOffset);
    } else {
        Logger::Warn("[Packet::ReadUInt] Not enough bytes (need 4, have %zu), returning 0",
                     m_payload.size() - m_readOffset);
    }
    Logger::Trace("[Packet::ReadUInt] Exit: returning %u", v);
    return v;
}

int32_t Packet::ReadInt() const {
    Logger::Trace("[Packet::ReadInt] Entry: readOffset=%zu", m_readOffset);
    int32_t result = (int32_t)ReadUInt();
    Logger::Trace("[Packet::ReadInt] Exit: returning %d", result);
    return result;
}

float Packet::ReadFloat() const {
    Logger::Trace("[Packet::ReadFloat] Entry: readOffset=%zu", m_readOffset);
    float f=0;
    // Bounds-check the wire-controlled read (mirror ReadUInt32). Additive.
    if (m_readOffset + 4 <= m_payload.size()) {
        std::memcpy(&f, &m_payload[m_readOffset],4);
        m_readOffset+=4;
        Logger::Debug("[Packet::ReadFloat] Read %f, readOffset now %zu", f, m_readOffset);
    } else {
        Logger::Warn("[Packet::ReadFloat] Not enough bytes (need 4, have %zu), returning 0",
                     m_payload.size() - m_readOffset);
    }
    Logger::Trace("[Packet::ReadFloat] Exit: returning %f", f);
    return f;
}

std::string Packet::ReadString() const {
    Logger::Trace("[Packet::ReadString] Entry: readOffset=%zu", m_readOffset);
    std::string result = DecodeString(m_payload, m_readOffset);
    Logger::Debug("[Packet::ReadString] Read string '%s' (length=%zu), readOffset now %zu",
                  result.c_str(), result.size(), m_readOffset);
    Logger::Trace("[Packet::ReadString] Exit: returning string of length %zu", result.size());
    return result;
}

Vector3 Packet::ReadVector3() const {
    Logger::Trace("[Packet::ReadVector3] Entry: readOffset=%zu", m_readOffset);
    Vector3 v; v.x=ReadFloat(); v.y=ReadFloat(); v.z=ReadFloat();
    Logger::Debug("[Packet::ReadVector3] Read Vector3(%f, %f, %f)", v.x, v.y, v.z);
    Logger::Trace("[Packet::ReadVector3] Exit: returning (%f, %f, %f)", v.x, v.y, v.z);
    return v;
}

std::vector<uint8_t> Packet::ReadBytes(size_t count) const {
    Logger::Trace("[Packet::ReadBytes] Entry: count=%zu, readOffset=%zu", count, m_readOffset);
    // Bounds-check: reject a count that runs past the payload (the subtraction is
    // overflow-safe because m_readOffset <= size is an invariant of the guarded reads).
    // Additive: an in-range count behaves exactly as before.
    if (m_readOffset > m_payload.size() || count > m_payload.size() - m_readOffset) {
        Logger::Warn("[Packet::ReadBytes] Not enough bytes (need %zu, have %zu), returning empty",
                     count, (m_readOffset < m_payload.size()) ? (m_payload.size() - m_readOffset) : 0);
        return {};
    }
    std::vector<uint8_t> out(m_payload.begin()+m_readOffset,
                              m_payload.begin()+m_readOffset+count);
    m_readOffset+=count;
    Logger::Debug("[Packet::ReadBytes] Read %zu bytes, readOffset now %zu", count, m_readOffset);
    Logger::Trace("[Packet::ReadBytes] Exit: returning %zu bytes", out.size());
    return out;
}

// Typed read methods (const -- use mutable read offset)
uint8_t Packet::ReadUInt8() const {
    Logger::Trace("[Packet::ReadUInt8] Entry: readOffset=%zu, payloadSize=%zu", m_readOffset, m_payload.size());
    if (m_readOffset >= m_payload.size()) {
        Logger::Warn("[Packet::ReadUInt8] Read past end of payload (offset=%zu >= size=%zu), returning 0",
                     m_readOffset, m_payload.size());
        return 0;
    }
    uint8_t result = m_payload[m_readOffset++];
    Logger::Debug("[Packet::ReadUInt8] Read 0x%02X, readOffset now %zu", result, m_readOffset);
    Logger::Trace("[Packet::ReadUInt8] Exit: returning 0x%02X", result);
    return result;
}

uint16_t Packet::ReadUInt16() const {
    Logger::Trace("[Packet::ReadUInt16] Entry: readOffset=%zu, payloadSize=%zu", m_readOffset, m_payload.size());
    uint16_t v = 0;
    if (m_readOffset + 2 <= m_payload.size()) {
        std::memcpy(&v, &m_payload[m_readOffset], 2);
        m_readOffset += 2;
        Logger::Debug("[Packet::ReadUInt16] Read %u, readOffset now %zu", v, m_readOffset);
    } else {
        Logger::Warn("[Packet::ReadUInt16] Not enough bytes (need 2, have %zu), returning 0",
                     m_payload.size() - m_readOffset);
    }
    Logger::Trace("[Packet::ReadUInt16] Exit: returning %u", v);
    return v;
}

uint32_t Packet::ReadUInt32() const {
    Logger::Trace("[Packet::ReadUInt32] Entry: readOffset=%zu, payloadSize=%zu", m_readOffset, m_payload.size());
    uint32_t v = 0;
    if (m_readOffset + 4 <= m_payload.size()) {
        std::memcpy(&v, &m_payload[m_readOffset], 4);
        m_readOffset += 4;
        Logger::Debug("[Packet::ReadUInt32] Read %u, readOffset now %zu", v, m_readOffset);
    } else {
        Logger::Warn("[Packet::ReadUInt32] Not enough bytes (need 4, have %zu), returning 0",
                     m_payload.size() - m_readOffset);
    }
    Logger::Trace("[Packet::ReadUInt32] Exit: returning %u", v);
    return v;
}

uint64_t Packet::ReadUInt64() const {
    Logger::Trace("[Packet::ReadUInt64] Entry: readOffset=%zu, payloadSize=%zu", m_readOffset, m_payload.size());
    uint64_t v = 0;
    if (m_readOffset + 8 <= m_payload.size()) {
        std::memcpy(&v, &m_payload[m_readOffset], 8);
        m_readOffset += 8;
        Logger::Debug("[Packet::ReadUInt64] Read %llu, readOffset now %zu", (unsigned long long)v, m_readOffset);
    } else {
        Logger::Warn("[Packet::ReadUInt64] Not enough bytes (need 8, have %zu), returning 0",
                     m_payload.size() - m_readOffset);
    }
    Logger::Trace("[Packet::ReadUInt64] Exit: returning %llu", (unsigned long long)v);
    return v;
}

size_t Packet::BytesRemaining() const {
    Logger::Trace("[Packet::BytesRemaining] Entry: readOffset=%zu, payloadSize=%zu", m_readOffset, m_payload.size());
    size_t remaining = (m_readOffset < m_payload.size()) ? (m_payload.size() - m_readOffset) : 0;
    Logger::Debug("[Packet::BytesRemaining] %zu bytes remaining", remaining);
    Logger::Trace("[Packet::BytesRemaining] Exit: returning %zu", remaining);
    return remaining;
}

size_t Packet::ReadBytesRemaining() const {
    Logger::Trace("[Packet::ReadBytesRemaining] Entry");
    size_t remaining = BytesRemaining();
    Logger::Trace("[Packet::ReadBytesRemaining] Exit: returning %zu", remaining);
    return remaining;
}

std::vector<uint8_t> Packet::ReadBytesRemainingVector() const {
    Logger::Trace("[Packet::ReadBytesRemainingVector] Entry: readOffset=%zu, payloadSize=%zu",
                  m_readOffset, m_payload.size());
    if (m_readOffset >= m_payload.size()) {
        Logger::Debug("[Packet::ReadBytesRemainingVector] No bytes remaining, returning empty vector");
        Logger::Trace("[Packet::ReadBytesRemainingVector] Exit: returning empty vector");
        return {};
    }
    auto result = std::vector<uint8_t>(m_payload.begin() + m_readOffset, m_payload.end());
    Logger::Debug("[Packet::ReadBytesRemainingVector] Returning %zu remaining bytes", result.size());
    Logger::Trace("[Packet::ReadBytesRemainingVector] Exit: returning %zu bytes", result.size());
    return result;
}

void Packet::ResetRead() {
    Logger::Trace("[Packet::ResetRead] Entry: m_readOffset=%zu", m_readOffset);
    m_readOffset = 0;
    Logger::Debug("[Packet::ResetRead] Read offset reset to 0");
    Logger::Trace("[Packet::ResetRead] Exit");
}

std::vector<uint8_t> Packet::EncodeString(const std::string& s) {
    Logger::Trace("[Packet::EncodeString] Entry: s='%s' (length=%zu)", s.c_str(), s.size());
    std::vector<uint8_t> buf;
    uint32_t len=(uint32_t)s.size();
    buf.resize(4+len);
    std::memcpy(buf.data(), &len,4);
    std::memcpy(buf.data()+4, s.data(), len);
    Logger::Debug("[Packet::EncodeString] Encoded string of length %u into %zu bytes", len, buf.size());
    Logger::Trace("[Packet::EncodeString] Exit: returning %zu bytes", buf.size());
    return buf;
}

std::string Packet::DecodeString(const std::vector<uint8_t>& buf, size_t& offset) {
    Logger::Trace("[Packet::DecodeString] Entry: offset=%zu, buf size=%zu", offset, buf.size());
    // SECURITY: len is a fully attacker-controlled uint32 from the wire. Without the
    // bounds checks below, an oversize len (e.g. 0xFFFFFFFF) constructs a multi-GB string
    // reading far past the buffer (OOB read -> crash/DoS or heap info disclosure), and a
    // payload shorter than 4 bytes reads the length prefix out of bounds. Both are
    // attacker-reachable via a legacy-path CHAT_MESSAGE. These checks are purely additive:
    // a correctly-framed string (len <= remaining) is byte-for-byte unaffected.
    if (offset > buf.size() || buf.size() - offset < 4) {
        Logger::Warn("[Packet::DecodeString] Truncated before 4-byte length prefix (offset=%zu, size=%zu), returning empty",
                     offset, buf.size());
        offset = buf.size();
        return std::string();
    }
    uint32_t len=0;
    std::memcpy(&len, &buf[offset],4);
    offset+=4;
    Logger::Debug("[Packet::DecodeString] String length=%u, reading from offset=%zu", len, offset);
    if (len > buf.size() - offset) {
        Logger::Warn("[Packet::DecodeString] String length %u exceeds remaining %zu bytes, returning empty",
                     len, buf.size() - offset);
        offset = buf.size();
        return std::string();
    }
    // buf.data()+offset is well-defined for offset==size() with len==0 (unlike &buf[offset]).
    std::string s(reinterpret_cast<const char*>(buf.data()) + offset, len);
    offset+=len;
    Logger::Debug("[Packet::DecodeString] Decoded string='%s', offset now=%zu", s.c_str(), offset);
    Logger::Trace("[Packet::DecodeString] Exit: returning string of length %u", len);
    return s;
}
