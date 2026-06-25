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

} // namespace ActorRepl
