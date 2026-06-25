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

// ---- NetGUID / object-reference codec (UPackageMap::SerializeObject) ----------
// VNGame.exe net override @0x140696070. Every object reference / NetGUID on the
// wire is a 1-bit selector followed by a ranged SerializeInt:
//   flag bit == 1  -> STATIC / already-known object: SerializeInt(index, 1023)
//   flag bit == 0  -> DYNAMIC / export (freshly-spawned actor or its class):
//                     SerializeInt(index, 0x80000000)
// (Confirmed against the ch2 PlayerController open `60 c1 01 00 ...`: byte0 bit0 = 0
//  => the dynamic/export path, correct for a just-spawned actor.)
constexpr uint32_t kStaticGuidMax = 1023;        // 0x3ff
constexpr uint32_t kExportGuidMax = 0x80000000u; // dynamic/export

struct NetGUIDRef {
    bool     isStatic = false;  // true => flag bit 1 (known object, max 1023)
    uint32_t index = 0;         // the NetIndex
};

// Serialize one object reference / NetGUID (selector bit + ranged int).
void WriteNetGUID(BitWriter& w, const NetGUIDRef& ref);

// Decode one object reference / NetGUID. On bit-stream overflow the BitReader's
// overflow flag is set (check r.IsOverflowed()); the returned value is then
// meaningless.
NetGUIDRef ReadNetGUID(BitReader& r);

// ---- SerializeNewActor opening-bunch header ---------------------------------
// The first bytes of an opening actor bunch (bOpen=1, ChType=2):
//   [actor NetGUID][class NetGUID]   (then the bNetInitial property block)
// The actor NetGUID is normally an export (a new actor); the class NetGUID refs a
// class object in the PackageMap we exported (so all same-class actors share it).
struct NewActorHeader {
    NetGUIDRef actorGuid;  // usually export (isStatic=false)
    NetGUIDRef classGuid;  // ref into the PackageMap (the actor's class)
};

void WriteNewActorHeader(BitWriter& w, const NewActorHeader& hdr);
NewActorHeader ReadNewActorHeader(BitReader& r);

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
// `chIndex` with ChSequence `chSeq`: payload = SerializeNewActor(hdr) followed by
// whatever `writeProps` writes (the bNetInitial property block). The returned
// Bunch's payloadBits is exact (not byte-padded), ready for
// PacketAssembler::BuildRawBunchPacket + Encode at the server MaxPacket.
PacketCodec::Bunch MakeOpeningActorBunch(
    uint32_t chIndex, uint32_t chSeq,
    const NewActorHeader& hdr,
    const std::function<void(BitWriter&)>& writeProps);

} // namespace ActorRepl
