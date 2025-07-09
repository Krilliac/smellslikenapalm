// src/Network/PacketSerializer.h

#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <type_traits>
#include "Network/Packet.h"
#include "Math/Vector3.h"

class PacketSerializer {
public:
    // Primitive types
    static void WriteUInt(Packet& pkt, uint32_t v);
    static uint32_t ReadUInt(Packet& pkt);

    static void WriteInt(Packet& pkt, int32_t v);
    static int32_t ReadInt(Packet& pkt);

    static void WriteFloat(Packet& pkt, float v);
    static float ReadFloat(Packet& pkt);

    static void WriteBool(Packet& pkt, bool b);
    static bool ReadBool(Packet& pkt);

    static void WriteString(Packet& pkt, const std::string& s);
    static std::string ReadString(Packet& pkt);

    // Vector3
    static void WriteVector3(Packet& pkt, const Vector3& v);
    static Vector3 ReadVector3(Packet& pkt);

    // Arrays of primitives
    template<typename T>
    static void WriteArray(Packet& pkt, const std::vector<T>& arr);

    template<typename T>
    static std::vector<T> ReadArray(Packet& pkt);

    // Custom struct (must implement Serialize/Deserialize methods)
    template<typename T>
    static void WriteObject(Packet& pkt, const T& obj);

    template<typename T>
    static T ReadObject(Packet& pkt);

private:
    template<typename T>
    static void WriteRaw(Packet& pkt, const T& v);

    template<typename T>
    static T ReadRaw(Packet& pkt);
};