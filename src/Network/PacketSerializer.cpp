// src/Network/PacketSerializer.cpp

#include "Network/PacketSerializer.h"
#include <cstring>

// Primitive implementations

void PacketSerializer::WriteUInt(Packet& pkt, uint32_t v) {
    pkt.WriteUInt(v);
}
uint32_t PacketSerializer::ReadUInt(Packet& pkt) {
    return pkt.ReadUInt();
}

void PacketSerializer::WriteInt(Packet& pkt, int32_t v) {
    pkt.WriteInt(v);
}
int32_t PacketSerializer::ReadInt(Packet& pkt) {
    return pkt.ReadInt();
}

void PacketSerializer::WriteFloat(Packet& pkt, float v) {
    pkt.WriteFloat(v);
}
float PacketSerializer::ReadFloat(Packet& pkt) {
    return pkt.ReadFloat();
}

void PacketSerializer::WriteBool(Packet& pkt, bool b) {
    WriteUInt(pkt, b ? 1u : 0u);
}
bool PacketSerializer::ReadBool(Packet& pkt) {
    return ReadUInt(pkt) != 0;
}

void PacketSerializer::WriteString(Packet& pkt, const std::string& s) {
    pkt.WriteString(s);
}
std::string PacketSerializer::ReadString(Packet& pkt) {
    return pkt.ReadString();
}

void PacketSerializer::WriteVector3(Packet& pkt, const Vector3& v) {
    pkt.WriteVector3(v);
}
Vector3 PacketSerializer::ReadVector3(Packet& pkt) {
    return pkt.ReadVector3();
}

// Raw helper
template<typename T>
void PacketSerializer::WriteRaw(Packet& pkt, const T& v) {
    static_assert(std::is_trivially_copyable<T>::value, "Raw write requires trivially copyable type");
    const uint8_t* data = reinterpret_cast<const uint8_t*>(&v);
    pkt.WriteBytes({data, data + sizeof(T)});
}

template<typename T>
T PacketSerializer::ReadRaw(Packet& pkt) {
    static_assert(std::is_trivially_copyable<T>::value, "Raw read requires trivially copyable type");
    T v;
    auto bytes = pkt.ReadBytes(sizeof(T));
    std::memcpy(&v, bytes.data(), sizeof(T));
    return v;
}

// Arrays
template<typename T>
void PacketSerializer::WriteArray(Packet& pkt, const std::vector<T>& arr) {
    WriteUInt(pkt, static_cast<uint32_t>(arr.size()));
    for (const auto& elem : arr) {
        if constexpr (std::is_same<T, std::string>::value) {
            WriteString(pkt, elem);
        } else if constexpr (std::is_same<T, Vector3>::value) {
            WriteVector3(pkt, elem);
        } else if constexpr (std::is_integral<T>::value) {
            WriteRaw(pkt, elem);
        } else if constexpr (std::is_floating_point<T>::value) {
            WriteRaw(pkt, elem);
        } else {
            WriteObject(pkt, elem);
        }
    }
}

template<typename T>
std::vector<T> PacketSerializer::ReadArray(Packet& pkt) {
    uint32_t count = ReadUInt(pkt);
    std::vector<T> arr;
    arr.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if constexpr (std::is_same<T, std::string>::value) {
            arr.push_back(ReadString(pkt));
        } else if constexpr (std::is_same<T, Vector3>::value) {
            arr.push_back(ReadVector3(pkt));
        } else if constexpr (std::is_integral<T>::value) {
            arr.push_back(ReadRaw<T>(pkt));
        } else if constexpr (std::is_floating_point<T>::value) {
            arr.push_back(ReadRaw<T>(pkt));
        } else {
            arr.push_back(ReadObject<T>(pkt));
        }
    }
    return arr;
}

// Custom object requires T::Serialize(Packet&) and static T::Deserialize(Packet&)
template<typename T>
void PacketSerializer::WriteObject(Packet& pkt, const T& obj) {
    obj.Serialize(pkt);
}

template<typename T>
T PacketSerializer::ReadObject(Packet& pkt) {
    return T::Deserialize(pkt);
}