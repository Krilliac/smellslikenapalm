// src/Network/NetworkPacket.h

#pragma once

#include <cstdint>
#include <vector>
#include <string>

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

    uint32_t ReadUInt();
    int32_t  ReadInt();
    float    ReadFloat();
    std::string ReadString();
    Vector3  ReadVector3();
    std::vector<uint8_t> ReadBytes(size_t count);

    // Raw data access
    const std::vector<uint8_t>& RawData() const;

private:
    std::string         m_tag;
    std::vector<uint8_t> m_payload;
    size_t               m_readOffset = 0;

    static std::vector<uint8_t> EncodeString(const std::string& s);
    static std::string DecodeString(const std::vector<uint8_t>& buf, size_t& offset);
};