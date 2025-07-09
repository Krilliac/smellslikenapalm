// src/Protocol/UE3Protocol.h
#pragma once

#include <cstdint>
#include <vector>
#include <functional>
#include "Math/Vector3.h"

// Low‐level Unreal Engine 3 “bunch” header
struct UE3BunchHeader {
    uint8_t  ChannelIndex;
    uint16_t ChSequence;
    bool     Reliable;
    bool     Open;
    bool     Close;
    uint16_t PayloadLen;
};

class UE3Protocol {
public:
    using SendFunc = std::function<void(const uint8_t* data, size_t len)>;

    UE3Protocol(uint8_t channelIndex = 0);
    ~UE3Protocol();

    // Build a UDP packet framing one bunch
    std::vector<uint8_t> BuildBunch(
        const uint8_t* payload, size_t payloadLen,
        bool reliable, bool openChannel, bool closeChannel);

    // Parse incoming UDP packet into header + payload
    bool ParseBunch(
        const uint8_t* packetData, size_t packetLen,
        UE3BunchHeader& outHeader,
        const uint8_t*& outPayload, size_t& outPayloadLen);

    // Send a bunch via provided send function
    void SendBunch(
        const uint8_t* payload, size_t payloadLen,
        bool reliable, bool open, bool close,
        SendFunc sender);

    // Helpers to serialize basic types into payload
    static void WriteUInt8(std::vector<uint8_t>& buf, uint8_t v);
    static void WriteUInt16(std::vector<uint8_t>& buf, uint16_t v);
    static void WriteFloat(std::vector<uint8_t>& buf, float v);
    static uint8_t  ReadUInt8(const uint8_t* data, size_t& offset);
    static uint16_t ReadUInt16(const uint8_t* data, size_t& offset);
    static float    ReadFloat(const uint8_t* data, size_t& offset);

private:
    uint8_t m_channelIndex;
    uint16_t m_outSeq;
    uint16_t m_inSeq;
};
