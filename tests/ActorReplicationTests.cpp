// tests/ActorReplicationTests.cpp
//
// Tests for the actor-channel replication primitives (src/Network/ActorReplication).
// The NetGUID/object-reference codec is the foundation of every per-session actor
// channel; these round-trip it and anchor it to real-capture bytes.
// Spec: docs/RS2V_ActorReplication_7258.md.

#include <gtest/gtest.h>

#include "Network/ActorReplication.h"
#include "Network/BitReader.h"
#include "Network/BitWriter.h"

#include <vector>
#include <cstdint>

using ActorRepl::NetGUIDRef;
using ActorRepl::NewActorHeader;

// A static (known-object) NetGUID round-trips through write/read.
TEST(ActorReplication, StaticNetGUIDRoundTrips) {
    for (uint32_t idx : {0u, 1u, 17u, 511u, 1022u}) {
        BitWriter w;
        ActorRepl::WriteNetGUID(w, NetGUIDRef{/*isStatic=*/true, idx});
        std::vector<uint8_t> bytes = w.GetBytes();

        BitReader r(bytes);
        NetGUIDRef got = ActorRepl::ReadNetGUID(r);
        EXPECT_FALSE(r.IsOverflowed());
        EXPECT_TRUE(got.isStatic);
        EXPECT_EQ(got.index, idx) << "static idx " << idx;
    }
}

// A dynamic/export NetGUID (freshly-spawned actor) round-trips.
TEST(ActorReplication, ExportNetGUIDRoundTrips) {
    for (uint32_t idx : {0u, 5u, 1234u, 65535u, 0x1234567u}) {
        BitWriter w;
        ActorRepl::WriteNetGUID(w, NetGUIDRef{/*isStatic=*/false, idx});
        std::vector<uint8_t> bytes = w.GetBytes();

        BitReader r(bytes);
        NetGUIDRef got = ActorRepl::ReadNetGUID(r);
        EXPECT_FALSE(r.IsOverflowed());
        EXPECT_FALSE(got.isStatic);
        EXPECT_EQ(got.index, idx) << "export idx " << idx;
    }
}

// The SerializeNewActor header (actor NetGUID + class NetGUID) round-trips,
// modelling the common case: an exported actor referencing a known (static) class.
TEST(ActorReplication, NewActorHeaderRoundTrips) {
    NewActorHeader in;
    in.actorGuid = NetGUIDRef{/*isStatic=*/false, 0xABCDEu};  // exported new actor
    in.classGuid = NetGUIDRef{/*isStatic=*/true, 600u};       // class ref into PackageMap

    BitWriter w;
    ActorRepl::WriteNewActorHeader(w, in);

    const std::vector<uint8_t> bytes = w.GetBytes();  // keep alive (BitReader views it)
    BitReader r(bytes);
    NewActorHeader out = ActorRepl::ReadNewActorHeader(r);
    EXPECT_FALSE(r.IsOverflowed());
    EXPECT_EQ(out.actorGuid.isStatic, in.actorGuid.isStatic);
    EXPECT_EQ(out.actorGuid.index, in.actorGuid.index);
    EXPECT_EQ(out.classGuid.isStatic, in.classGuid.isStatic);
    EXPECT_EQ(out.classGuid.index, in.classGuid.index);
}

// Capture anchor: the real ch2 PlayerController open (Session A f1484) begins
// `60 c1 01 00 ...`. The first (flag) bit is the static/dynamic selector and must
// decode as DYNAMIC/EXPORT (bit 0) for a freshly-spawned actor - the [H] result
// that corrected the earlier "even NetGUID" misread.
TEST(ActorReplication, CapturePlayerControllerOpenIsExport) {
    // Real bytes from docs/RS2V_ActorReplication_7258.md §2.2 (ch2 PC open prefix).
    const std::vector<uint8_t> openPrefix = {
        0x60, 0xc1, 0x01, 0x00, 0x1b, 0xcc, 0xea, 0x3f, 0x49, 0x04, 0x60, 0xe0};

    BitReader r(openPrefix);
    NetGUIDRef actor = ActorRepl::ReadNetGUID(r);
    EXPECT_FALSE(actor.isStatic) << "a freshly-spawned actor's NetGUID must be the export path";
    // The class ref follows; it must also decode without running out of bits.
    NetGUIDRef cls = ActorRepl::ReadNetGUID(r);
    EXPECT_FALSE(r.IsOverflowed());
    (void)cls;
}

// A mixed replicated-property block (one of each type) round-trips: each property
// is SerializeInt(handle, maxHandle) + typed value, read back in order.
TEST(ActorReplication, PropertyBlockRoundTrips) {
    const uint32_t maxHandle = 64;
    BitWriter w;
    ActorRepl::WritePropBool  (w, 5,  maxHandle, true);
    ActorRepl::WritePropByte  (w, 7,  maxHandle, 0xAB);
    ActorRepl::WritePropInt   (w, 10, maxHandle, -12345);
    ActorRepl::WritePropFloat (w, 12, maxHandle, 3.5f);
    ActorRepl::WritePropString(w, 20, maxHandle, "Krill");
    ActorRepl::WritePropObject(w, 30, maxHandle, NetGUIDRef{/*isStatic=*/true, 600u});

    const std::vector<uint8_t> bytes = w.GetBytes();
    const size_t nbits = w.NumBits();
    BitReader r(bytes.data(), bytes.size(), nbits);

    EXPECT_EQ(r.SerializeInt(maxHandle), 5u);    EXPECT_TRUE(r.ReadBit());
    EXPECT_EQ(r.SerializeInt(maxHandle), 7u);    EXPECT_EQ(r.ReadByte(), 0xABu);
    EXPECT_EQ(r.SerializeInt(maxHandle), 10u);   EXPECT_EQ(r.ReadInt32(), -12345);
    EXPECT_EQ(r.SerializeInt(maxHandle), 12u);   EXPECT_FLOAT_EQ(r.ReadFloat(), 3.5f);
    EXPECT_EQ(r.SerializeInt(maxHandle), 20u);   EXPECT_EQ(r.ReadString(), "Krill");
    EXPECT_EQ(r.SerializeInt(maxHandle), 30u);
    NetGUIDRef g = ActorRepl::ReadNetGUID(r);
    EXPECT_TRUE(g.isStatic);
    EXPECT_EQ(g.index, 600u);

    EXPECT_FALSE(r.IsOverflowed());
    EXPECT_EQ(r.BitPos(), nbits) << "block consumed exactly (length-delimited)";
}

// An opening actor bunch (SerializeNewActor header + property block) decodes back:
// models a ch2 PlayerController open with the Actor-base role flags that make it
// the owning client's (RemoteRole=AutonomousProxy, bNetOwner).
TEST(ActorReplication, OpeningActorBunchDecodes) {
    const uint32_t maxHandle = 80;
    NewActorHeader hdr;
    hdr.actorGuid = NetGUIDRef{/*isStatic=*/false, 0x4242u}; // exported new actor
    hdr.classGuid = NetGUIDRef{/*isStatic=*/true, 90u};      // PlayerController class ref

    PacketCodec::Bunch b = ActorRepl::MakeOpeningActorBunch(
        /*chIndex=*/2, /*chSeq=*/1, hdr,
        [&](BitWriter& w) {
            ActorRepl::WritePropByte(w, 6, maxHandle, 2);  // RemoteRole=AutonomousProxy
            ActorRepl::WritePropByte(w, 7, maxHandle, 3);  // Role=Authority
            ActorRepl::WritePropBool(w, 8, maxHandle, true); // bNetOwner
        });

    EXPECT_FALSE(b.bControl);
    EXPECT_TRUE(b.bOpen);
    EXPECT_TRUE(b.bReliable);
    EXPECT_EQ(b.chIndex, 2u);
    EXPECT_EQ(b.chType, 2u);
    EXPECT_EQ(b.chSequence, 1u);

    BitReader r(b.payload.data(), b.payload.size(), b.payloadBits);
    NewActorHeader out = ActorRepl::ReadNewActorHeader(r);
    EXPECT_FALSE(out.actorGuid.isStatic);
    EXPECT_EQ(out.actorGuid.index, 0x4242u);
    EXPECT_TRUE(out.classGuid.isStatic);
    EXPECT_EQ(out.classGuid.index, 90u);

    EXPECT_EQ(r.SerializeInt(maxHandle), 6u);  EXPECT_EQ(r.ReadByte(), 2u);
    EXPECT_EQ(r.SerializeInt(maxHandle), 7u);  EXPECT_EQ(r.ReadByte(), 3u);
    EXPECT_EQ(r.SerializeInt(maxHandle), 8u);  EXPECT_TRUE(r.ReadBit());
    EXPECT_FALSE(r.IsOverflowed());
    EXPECT_EQ(r.BitPos(), static_cast<size_t>(b.payloadBits));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
