// src/Network/PacketSerializer.cpp

#include "Network/PacketSerializer.h"
#include "Utils/Logger.h"
#include <cstring>

// Primitive implementations

void PacketSerializer::WriteUInt(Packet& pkt, uint32_t v) {
    Logger::Trace("[PacketSerializer::WriteUInt] Entry: v=%u", v);
    pkt.WriteUInt(v);
    Logger::Debug("[PacketSerializer::WriteUInt] Wrote uint32_t value %u to packet", v);
    Logger::Trace("[PacketSerializer::WriteUInt] Exit");
}
uint32_t PacketSerializer::ReadUInt(Packet& pkt) {
    Logger::Trace("[PacketSerializer::ReadUInt] Entry");
    uint32_t result = pkt.ReadUInt();
    Logger::Debug("[PacketSerializer::ReadUInt] Read uint32_t value %u from packet", result);
    Logger::Trace("[PacketSerializer::ReadUInt] Exit: returning %u", result);
    return result;
}

void PacketSerializer::WriteInt(Packet& pkt, int32_t v) {
    Logger::Trace("[PacketSerializer::WriteInt] Entry: v=%d", v);
    pkt.WriteInt(v);
    Logger::Debug("[PacketSerializer::WriteInt] Wrote int32_t value %d to packet", v);
    Logger::Trace("[PacketSerializer::WriteInt] Exit");
}
int32_t PacketSerializer::ReadInt(Packet& pkt) {
    Logger::Trace("[PacketSerializer::ReadInt] Entry");
    int32_t result = pkt.ReadInt();
    Logger::Debug("[PacketSerializer::ReadInt] Read int32_t value %d from packet", result);
    Logger::Trace("[PacketSerializer::ReadInt] Exit: returning %d", result);
    return result;
}

void PacketSerializer::WriteFloat(Packet& pkt, float v) {
    Logger::Trace("[PacketSerializer::WriteFloat] Entry: v=%f", v);
    pkt.WriteFloat(v);
    Logger::Debug("[PacketSerializer::WriteFloat] Wrote float value %f to packet", v);
    Logger::Trace("[PacketSerializer::WriteFloat] Exit");
}
float PacketSerializer::ReadFloat(Packet& pkt) {
    Logger::Trace("[PacketSerializer::ReadFloat] Entry");
    float result = pkt.ReadFloat();
    Logger::Debug("[PacketSerializer::ReadFloat] Read float value %f from packet", result);
    Logger::Trace("[PacketSerializer::ReadFloat] Exit: returning %f", result);
    return result;
}

void PacketSerializer::WriteBool(Packet& pkt, bool b) {
    Logger::Trace("[PacketSerializer::WriteBool] Entry: b=%s", b ? "true" : "false");
    WriteUInt(pkt, b ? 1u : 0u);
    Logger::Debug("[PacketSerializer::WriteBool] Wrote bool value %s (as uint %u) to packet",
                  b ? "true" : "false", b ? 1u : 0u);
    Logger::Trace("[PacketSerializer::WriteBool] Exit");
}
bool PacketSerializer::ReadBool(Packet& pkt) {
    Logger::Trace("[PacketSerializer::ReadBool] Entry");
    bool result = ReadUInt(pkt) != 0;
    Logger::Debug("[PacketSerializer::ReadBool] Read bool value %s from packet", result ? "true" : "false");
    Logger::Trace("[PacketSerializer::ReadBool] Exit: returning %s", result ? "true" : "false");
    return result;
}

void PacketSerializer::WriteString(Packet& pkt, const std::string& s) {
    Logger::Trace("[PacketSerializer::WriteString] Entry: s='%s' (length=%zu)", s.c_str(), s.size());
    pkt.WriteString(s);
    Logger::Debug("[PacketSerializer::WriteString] Wrote string of length %zu to packet", s.size());
    Logger::Trace("[PacketSerializer::WriteString] Exit");
}
std::string PacketSerializer::ReadString(Packet& pkt) {
    Logger::Trace("[PacketSerializer::ReadString] Entry");
    std::string result = pkt.ReadString();
    Logger::Debug("[PacketSerializer::ReadString] Read string '%s' (length=%zu) from packet", result.c_str(), result.size());
    Logger::Trace("[PacketSerializer::ReadString] Exit: returning string of length %zu", result.size());
    return result;
}

void PacketSerializer::WriteVector3(Packet& pkt, const Vector3& v) {
    Logger::Trace("[PacketSerializer::WriteVector3] Entry: v=(%f, %f, %f)", v.x, v.y, v.z);
    pkt.WriteVector3(v);
    Logger::Debug("[PacketSerializer::WriteVector3] Wrote Vector3(%f, %f, %f) to packet", v.x, v.y, v.z);
    Logger::Trace("[PacketSerializer::WriteVector3] Exit");
}
Vector3 PacketSerializer::ReadVector3(Packet& pkt) {
    Logger::Trace("[PacketSerializer::ReadVector3] Entry");
    Vector3 result = pkt.ReadVector3();
    Logger::Debug("[PacketSerializer::ReadVector3] Read Vector3(%f, %f, %f) from packet", result.x, result.y, result.z);
    Logger::Trace("[PacketSerializer::ReadVector3] Exit: returning (%f, %f, %f)", result.x, result.y, result.z);
    return result;
}

// Raw helper
template<typename T>
void PacketSerializer::WriteRaw(Packet& pkt, const T& v) {
    static_assert(std::is_trivially_copyable<T>::value, "Raw write requires trivially copyable type");
    Logger::Trace("[PacketSerializer::WriteRaw] Entry: sizeof(T)=%zu", sizeof(T));
    const uint8_t* data = reinterpret_cast<const uint8_t*>(&v);
    pkt.WriteBytes({data, data + sizeof(T)});
    Logger::Debug("[PacketSerializer::WriteRaw] Wrote %zu raw bytes to packet", sizeof(T));
    Logger::Trace("[PacketSerializer::WriteRaw] Exit");
}

template<typename T>
T PacketSerializer::ReadRaw(Packet& pkt) {
    static_assert(std::is_trivially_copyable<T>::value, "Raw read requires trivially copyable type");
    Logger::Trace("[PacketSerializer::ReadRaw] Entry: sizeof(T)=%zu", sizeof(T));
    T v;
    auto bytes = pkt.ReadBytes(sizeof(T));
    std::memcpy(&v, bytes.data(), sizeof(T));
    Logger::Debug("[PacketSerializer::ReadRaw] Read %zu raw bytes from packet", sizeof(T));
    Logger::Trace("[PacketSerializer::ReadRaw] Exit");
    return v;
}

// Arrays
template<typename T>
void PacketSerializer::WriteArray(Packet& pkt, const std::vector<T>& arr) {
    Logger::Trace("[PacketSerializer::WriteArray] Entry: array size=%zu", arr.size());
    WriteUInt(pkt, static_cast<uint32_t>(arr.size()));
    Logger::Debug("[PacketSerializer::WriteArray] Writing %zu elements to packet", arr.size());
    for (size_t idx = 0; idx < arr.size(); ++idx) {
        const auto& elem = arr[idx];
        Logger::Trace("[PacketSerializer::WriteArray] Writing element[%zu]", idx);
        if constexpr (std::is_same<T, std::string>::value) {
            Logger::Debug("[PacketSerializer::WriteArray] Element[%zu] is string type", idx);
            WriteString(pkt, elem);
        } else if constexpr (std::is_same<T, Vector3>::value) {
            Logger::Debug("[PacketSerializer::WriteArray] Element[%zu] is Vector3 type", idx);
            WriteVector3(pkt, elem);
        } else if constexpr (std::is_integral<T>::value) {
            Logger::Debug("[PacketSerializer::WriteArray] Element[%zu] is integral type", idx);
            WriteRaw(pkt, elem);
        } else if constexpr (std::is_floating_point<T>::value) {
            Logger::Debug("[PacketSerializer::WriteArray] Element[%zu] is floating point type", idx);
            WriteRaw(pkt, elem);
        } else {
            Logger::Debug("[PacketSerializer::WriteArray] Element[%zu] is custom object type", idx);
            WriteObject(pkt, elem);
        }
    }
    Logger::Debug("[PacketSerializer::WriteArray] Finished writing %zu elements", arr.size());
    Logger::Trace("[PacketSerializer::WriteArray] Exit");
}

template<typename T>
std::vector<T> PacketSerializer::ReadArray(Packet& pkt) {
    Logger::Trace("[PacketSerializer::ReadArray] Entry");
    uint32_t count = ReadUInt(pkt);
    Logger::Debug("[PacketSerializer::ReadArray] Array count=%u, reserving space", count);
    std::vector<T> arr;
    arr.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        Logger::Trace("[PacketSerializer::ReadArray] Reading element[%u/%u]", i, count);
        if constexpr (std::is_same<T, std::string>::value) {
            Logger::Debug("[PacketSerializer::ReadArray] Element[%u] is string type", i);
            arr.push_back(ReadString(pkt));
        } else if constexpr (std::is_same<T, Vector3>::value) {
            Logger::Debug("[PacketSerializer::ReadArray] Element[%u] is Vector3 type", i);
            arr.push_back(ReadVector3(pkt));
        } else if constexpr (std::is_integral<T>::value) {
            Logger::Debug("[PacketSerializer::ReadArray] Element[%u] is integral type", i);
            arr.push_back(ReadRaw<T>(pkt));
        } else if constexpr (std::is_floating_point<T>::value) {
            Logger::Debug("[PacketSerializer::ReadArray] Element[%u] is floating point type", i);
            arr.push_back(ReadRaw<T>(pkt));
        } else {
            Logger::Debug("[PacketSerializer::ReadArray] Element[%u] is custom object type", i);
            arr.push_back(ReadObject<T>(pkt));
        }
    }
    Logger::Debug("[PacketSerializer::ReadArray] Finished reading %u elements", count);
    Logger::Trace("[PacketSerializer::ReadArray] Exit: returning array of %zu elements", arr.size());
    return arr;
}

// Custom object requires T::Serialize(Packet&) and static T::Deserialize(Packet&)
template<typename T>
void PacketSerializer::WriteObject(Packet& pkt, const T& obj) {
    Logger::Trace("[PacketSerializer::WriteObject] Entry: serializing custom object");
    obj.Serialize(pkt);
    Logger::Debug("[PacketSerializer::WriteObject] Custom object serialized to packet");
    Logger::Trace("[PacketSerializer::WriteObject] Exit");
}

template<typename T>
T PacketSerializer::ReadObject(Packet& pkt) {
    Logger::Trace("[PacketSerializer::ReadObject] Entry: deserializing custom object");
    T result = T::Deserialize(pkt);
    Logger::Debug("[PacketSerializer::ReadObject] Custom object deserialized from packet");
    Logger::Trace("[PacketSerializer::ReadObject] Exit");
    return result;
}
