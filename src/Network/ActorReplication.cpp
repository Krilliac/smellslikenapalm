// src/Network/ActorReplication.cpp
// See ActorReplication.h.

#include "Network/ActorReplication.h"

namespace ActorRepl {

void WriteNetGUID(BitWriter& w, const NetGUIDRef& ref) {
    w.WriteBit(ref.isStatic);  // flag: 1=static(known), 0=dynamic/export
    w.SerializeInt(ref.index, ref.isStatic ? kStaticGuidMax : kExportGuidMax);
}

NetGUIDRef ReadNetGUID(BitReader& r) {
    NetGUIDRef ref;
    ref.isStatic = r.ReadBit();
    ref.index = r.SerializeInt(ref.isStatic ? kStaticGuidMax : kExportGuidMax);
    return ref;
}

void WriteNewActorHeader(BitWriter& w, const NewActorHeader& hdr) {
    WriteNetGUID(w, hdr.actorGuid);
    WriteNetGUID(w, hdr.classGuid);
}

NewActorHeader ReadNewActorHeader(BitReader& r) {
    NewActorHeader hdr;
    hdr.actorGuid = ReadNetGUID(r);
    hdr.classGuid = ReadNetGUID(r);
    return hdr;
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
    const NewActorHeader& hdr,
    const std::function<void(BitWriter&)>& writeProps) {
    BitWriter w;
    WriteNewActorHeader(w, hdr);
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
