// src/Network/ActorReplication.cpp
// See ActorReplication.h.

#include "Network/ActorReplication.h"

namespace ActorRepl {

void WriteNetGUID(BitWriter& w, const NetGUIDRef& ref) {
    w.WriteBit(ref.isDynamic);  // flag: 1=dynamic (channel index), 0=static (object index)
    w.SerializeInt(ref.index, ref.isDynamic ? kDynamicChannelMax : kStaticObjectMax);
}

NetGUIDRef ReadNetGUID(BitReader& r) {
    NetGUIDRef ref;
    ref.isDynamic = r.ReadBit();
    ref.index = r.SerializeInt(ref.isDynamic ? kDynamicChannelMax : kStaticObjectMax);
    return ref;
}

namespace {
// appCeilLogTwo: smallest n with 2^n >= v. CeilLogTwo(1)=0, (2)=1, (3)=2, (4)=2.
uint32_t CeilLogTwo(uint32_t v) {
    if (v <= 1) return 0;
    uint32_t n = 0, x = v - 1;
    while (x > 0) { x >>= 1; ++n; }
    return n;
}
uint32_t AbsI(int32_t v) { return v < 0 ? static_cast<uint32_t>(-(int64_t)v) : static_cast<uint32_t>(v); }
int32_t RoundToInt(float f) { return static_cast<int32_t>(f < 0 ? f - 0.5f : f + 0.5f); }
} // namespace

void WriteCompressedVector(BitWriter& w, float x, float y, float z) {
    const int32_t ix = RoundToInt(x), iy = RoundToInt(y), iz = RoundToInt(z);
    uint32_t maxc = AbsI(ix);
    if (AbsI(iy) > maxc) maxc = AbsI(iy);
    if (AbsI(iz) > maxc) maxc = AbsI(iz);
    uint32_t bits = CeilLogTwo(1u + maxc);
    if (bits < 1) bits = 1;
    if (bits > 20) bits = 20;
    bits -= 1;
    w.SerializeInt(bits, 20);
    const uint32_t bias = 1u << (bits + 1);
    const uint32_t mx   = 1u << (bits + 2);
    w.SerializeInt(static_cast<uint32_t>(ix + static_cast<int32_t>(bias)), mx);
    w.SerializeInt(static_cast<uint32_t>(iy + static_cast<int32_t>(bias)), mx);
    w.SerializeInt(static_cast<uint32_t>(iz + static_cast<int32_t>(bias)), mx);
}

void ReadCompressedVector(BitReader& r, float& x, float& y, float& z) {
    const uint32_t bits = r.SerializeInt(20);
    const int32_t bias = static_cast<int32_t>(1u << (bits + 1));
    const uint32_t mx  = 1u << (bits + 2);
    x = static_cast<float>(static_cast<int32_t>(r.SerializeInt(mx)) - bias);
    y = static_cast<float>(static_cast<int32_t>(r.SerializeInt(mx)) - bias);
    z = static_cast<float>(static_cast<int32_t>(r.SerializeInt(mx)) - bias);
}

void WriteCompressedRotator(BitWriter& w, uint16_t pitch, uint16_t yaw, uint16_t roll) {
    const uint8_t bp = static_cast<uint8_t>(pitch >> 8);
    const uint8_t by = static_cast<uint8_t>(yaw >> 8);
    const uint8_t br = static_cast<uint8_t>(roll >> 8);
    w.WriteBit(bp != 0); if (bp) w.WriteByte(bp);
    w.WriteBit(by != 0); if (by) w.WriteByte(by);
    w.WriteBit(br != 0); if (br) w.WriteByte(br);
}

void ReadCompressedRotator(BitReader& r, uint16_t& pitch, uint16_t& yaw, uint16_t& roll) {
    pitch = r.ReadBit() ? static_cast<uint16_t>(r.ReadByte() << 8) : 0;
    yaw   = r.ReadBit() ? static_cast<uint16_t>(r.ReadByte() << 8) : 0;
    roll  = r.ReadBit() ? static_cast<uint16_t>(r.ReadByte() << 8) : 0;
}

void WriteActorOpenHeader(BitWriter& w, const ActorOpenHeader& hdr) {
    WriteNetGUID(w, hdr.classRef);                         // static class/archetype ref
    WriteCompressedVector(w, hdr.locX, hdr.locY, hdr.locZ); // Location (always present)
    if (hdr.hasRotation) {
        WriteCompressedRotator(w, hdr.pitch, hdr.yaw, hdr.roll);
    }
    if (hdr.isPlayerController) {
        w.SerializeInt(hdr.netPlayerIndex, kDynamicChannelMax); // NetPlayerIndex (0 = owning client)
    }
}

// ---- property serialization (handle + typed value) --------------------------
void WritePropBool(BitWriter& w, uint32_t handle, uint32_t maxHandle, bool v) {
    w.SerializeInt(handle, maxHandle);
    w.WriteBit(v);
}
void WritePropByte(BitWriter& w, uint32_t handle, uint32_t maxHandle, uint8_t v) {
    w.SerializeInt(handle, maxHandle);
    w.WriteByte(v);
}
void WritePropInt(BitWriter& w, uint32_t handle, uint32_t maxHandle, int32_t v) {
    w.SerializeInt(handle, maxHandle);
    w.WriteInt32(v);
}
void WritePropFloat(BitWriter& w, uint32_t handle, uint32_t maxHandle, float v) {
    w.SerializeInt(handle, maxHandle);
    w.WriteFloat(v);
}
void WritePropString(BitWriter& w, uint32_t handle, uint32_t maxHandle, const std::string& v) {
    w.SerializeInt(handle, maxHandle);
    w.WriteString(v);
}
void WritePropObject(BitWriter& w, uint32_t handle, uint32_t maxHandle, const NetGUIDRef& v) {
    w.SerializeInt(handle, maxHandle);
    WriteNetGUID(w, v);
}

PacketCodec::Bunch MakeOpeningActorBunch(
    uint32_t chIndex, uint32_t chSeq,
    const ActorOpenHeader& hdr,
    const std::function<void(BitWriter&)>& writeProps) {
    BitWriter w;
    WriteActorOpenHeader(w, hdr);
    if (writeProps) {
        writeProps(w);
    }
    PacketCodec::Bunch b;
    b.bControl = false;
    b.bOpen = true;       // opening an actor channel
    b.bClose = false;
    b.bReliable = true;
    b.chIndex = chIndex;
    b.chType = 2;         // CHTYPE_Actor
    b.chSequence = chSeq;
    b.payload = w.GetBytes();
    b.payloadBits = static_cast<uint32_t>(w.NumBits());
    return b;
}

} // namespace ActorRepl
