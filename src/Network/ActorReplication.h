// src/Network/ActorReplication.h
//
// UE3 (RS2 EngineVersion 7258) ACTOR-CHANNEL replication primitives - the layer
// above the bunch framing (PacketCodec) that turns a Joined-but-frozen client
// into a controllable player. Reversed from two real-server gameplay captures +
// VNGame.exe disassembly; see docs/RS2V_ActorReplication_7258.md.
//
// This file currently implements the FOUNDATION used by every actor channel:
//   * the NetGUID / object-reference codec (UPackageMap::SerializeObject), and
//   * the SerializeNewActor opening-bunch header (actor NetGUID + class NetGUID).
// Property-value serialization and the per-channel spawn/possess flow build on it
// (added incrementally; see the phased plan in the spec doc).
//
// All bit IO is LSB-first via BitReader/BitWriter; bounded ints use the same
// ranged SerializeInt as the bunch header.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "Network/BitReader.h"
#include "Network/BitWriter.h"
#include "Network/PacketCodec.h"

namespace ActorRepl {

// ---- Object-reference codec (UPackageMapLevel::SerializeObject) ---------------
// UE3 7258 (UnNetDrv.cpp:97) - there is NO NetGUID system (that is UE4). An object
// reference is a 1-bit selector followed by a ranged SerializeInt, where the value
// is one of exactly two kinds:
//   flag bit == 0  -> STATIC object (class / archetype / CDO / static actor):
//                     SerializeInt(index, 0x80000000), index = package.ObjectBase +
//                     Object->NetIndex (deterministic from our PackageMap export).
//   flag bit == 1  -> DYNAMIC actor: SerializeInt(index, 2048), index = the actor's
//                     OPEN ACTOR-CHANNEL INDEX. The actor IS its channel; there is no
//                     persistent id - opening a channel is the "assignment".
// (Verified against the ch2 PlayerController open `60 c1 01 00`: byte0 bit0 = 0 =>
//  STATIC = the archetype ref. A reference to an unopened channel / bad class ref =>
//  NMT_ActorChannelFailure and the client CLOSES the channel - our observed failure.)
constexpr uint32_t kStaticObjectMax   = 0x80000000u; // MAX_OBJECT_INDEX (static index)
constexpr uint32_t kDynamicChannelMax = 1024;        // RS2 MAX_CHANNELS (dynamic = ChIndex;
                                                     // disasm-confirmed ~1023 max, use 1024)

struct NetGUIDRef {
    bool     isDynamic = false;  // true => flag bit 1, value is an actor channel index
                                 // false => flag bit 0, value is a static object index
    uint32_t index = 0;
};

// Serialize one object reference / NetGUID (selector bit + ranged int).
void WriteNetGUID(BitWriter& w, const NetGUIDRef& ref);

// Decode one object reference / NetGUID. On bit-stream overflow the BitReader's
// overflow flag is set (check r.IsOverflowed()); the returned value is then
// meaningless.
NetGUIDRef ReadNetGUID(BitReader& r);

// ---- Compressed Vector / Rotator (FVector/FRotator::SerializeCompressed) ------
// UE3 UnMath.cpp. Used for the actor-open header Location/Rotation and for
// Vector/Rotator-typed replicated properties.
//
// Vector (UnMath.cpp:51): components rounded to INTEGERS, then
//   Bits = Clamp(CeilLogTwo(1+Max3(|ix|,|iy|,|iz|)), 1, 20) - 1; SerializeInt(Bits,20);
//   Bias = 1<<(Bits+1); Max = 1<<(Bits+2); SerializeInt(c+Bias, Max) x3.
void WriteCompressedVector(BitWriter& w, float x, float y, float z);
void ReadCompressedVector(BitReader& r, float& x, float& y, float& z);

// Rotator (UnMath.cpp:84): each of (pitch,yaw,roll) as the TOP byte (angle>>8):
//   1 presence bit (component>>8 != 0), then that byte iff non-zero.
// Angles are UE 16-bit rotator units [0,65535].
void WriteCompressedRotator(BitWriter& w, uint16_t pitch, uint16_t yaw, uint16_t roll);
void ReadCompressedRotator(BitReader& r, uint16_t& pitch, uint16_t& yaw, uint16_t& roll);

// ---- Actor-open (SerializeNewActor) header -----------------------------------
// The opening bunch of an actor channel (bOpen=1, ChType=2) begins with, in order
// (UnChan.cpp ReplicateActor / ReceivedBunch, doc UE3_ActorChannel.md):
//   [class static-ref] [compressed Location] [compressed Rotation if bNetInitialRotation]
//   [NetPlayerIndex (ranged int, max MAX_CHANNELS) if PlayerController]
// then the bNetInitial property block. There is NO per-actor object ref - the
// channel index IS the actor's identity. For the OWNING client's PlayerController,
// NetPlayerIndex MUST be 0 (UnConn.cpp HandleClientPlayer adopts it as the local
// autonomous PC). A class ref that does not resolve => NMT_ActorChannelFailure =>
// the client closes the channel.
struct ActorOpenHeader {
    NetGUIDRef classRef;                 // static class/archetype ref (isDynamic=false)
    float locX = 0.f, locY = 0.f, locZ = 0.f;   // compressed Location (always present)
    bool  hasRotation = false;           // bNetInitialRotation
    uint16_t pitch = 0, yaw = 0, roll = 0;       // compressed Rotation (if hasRotation)
    bool  isPlayerController = false;     // writes NetPlayerIndex when true
    uint32_t netPlayerIndex = 0;         // 0 for the owning client's PC
};

void WriteActorOpenHeader(BitWriter& w, const ActorOpenHeader& hdr);

// ---- Replicated-property serialization ---------------------------------------
// Each replicated property on the wire is `SerializeInt(handle, maxHandle)` then
// the type-specific value (spec §3.1). `handle` is the property's index in the
// class's replicated VAR DECLARATION order; `maxHandle` is the class's replicated
// field count (ClassNetCache) - a per-class value the caller supplies from the
// class tables in docs/RS2V_ActorReplication_7258.md. Properties are written in
// ascending handle order; the bunch's BunchDataBits length delimits the block (no
// terminator handle). The value encodings are standard UE3 net formats:
//   bool=1 bit, byte=8 bits, int=int32, float=float32, FString=length-prefixed,
//   object ref=NetGUID (WriteNetGUID).
void WritePropBool  (BitWriter& w, uint32_t handle, uint32_t maxHandle, bool v);
void WritePropByte  (BitWriter& w, uint32_t handle, uint32_t maxHandle, uint8_t v);
void WritePropInt   (BitWriter& w, uint32_t handle, uint32_t maxHandle, int32_t v);
void WritePropFloat (BitWriter& w, uint32_t handle, uint32_t maxHandle, float v);
void WritePropString(BitWriter& w, uint32_t handle, uint32_t maxHandle, const std::string& v);
void WritePropObject(BitWriter& w, uint32_t handle, uint32_t maxHandle, const NetGUIDRef& v);

// Build an OPENING actor-channel bunch (bOpen=1, bReliable=1, ChType=2) on
// `chIndex` with ChSequence `chSeq`: payload = the actor-open header (hdr) followed
// by whatever `writeProps` writes (the bNetInitial property block). The returned
// Bunch's payloadBits is exact (not byte-padded), ready for
// PacketAssembler::BuildRawBunchPacket + Encode at the server MaxPacket.
PacketCodec::Bunch MakeOpeningActorBunch(
    uint32_t chIndex, uint32_t chSeq,
    const ActorOpenHeader& hdr,
    const std::function<void(BitWriter&)>& writeProps);

} // namespace ActorRepl
