// src/Network/NetworkPacket.h

#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include "Math/Vector3.h"

struct PacketMetadata {
    uint32_t clientId;
    uint64_t timestamp;      // milliseconds since epoch
    bool     isChat;
    std::string rawTag;
};

class Packet {
public:
    Packet() = default;
    Packet(const std::string& tag);
    Packet(const std::string& tag, const std::vector<uint8_t>& payload);

    // Serialization
    std::vector<uint8_t> Serialize() const;
    static Packet FromBuffer(const std::vector<uint8_t>& buffer, PacketMetadata& meta);

    // Header access
    const std::string& GetTag() const;
    uint32_t           GetPayloadSize() const;

    // Payload read/write
    void WriteUInt(uint32_t v);
    void WriteInt(int32_t v);
    void WriteFloat(float v);
    void WriteString(const std::string& s);
    void WriteVector3(const Vector3& v);
    void WriteBytes(const std::vector<uint8_t>& data);

    uint32_t ReadUInt() const;
    int32_t  ReadInt() const;
    float    ReadFloat() const;
    std::string ReadString() const;
    Vector3  ReadVector3() const;
    std::vector<uint8_t> ReadBytes(size_t count) const;

    // Typed read methods (used by PacketAnalysis and protocol decoder)
    uint8_t  ReadUInt8() const;
    uint16_t ReadUInt16() const;
    uint32_t ReadUInt32() const;
    uint64_t ReadUInt64() const;

    // Remaining data access
    size_t BytesRemaining() const;
    size_t ReadBytesRemaining() const;
    std::vector<uint8_t> ReadBytesRemainingVector() const;

    // Reset read cursor
    void ResetRead();

    // Raw data access
    const std::vector<uint8_t>& RawData() const;

private:
    std::string         m_tag;
    std::vector<uint8_t> m_payload;
    mutable size_t       m_readOffset = 0;

    static std::vector<uint8_t> EncodeString(const std::string& s);
    static std::string DecodeString(const std::vector<uint8_t>& buf, size_t& offset);
};