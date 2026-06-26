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

// A static object reference (class/archetype, flag bit 0, large index space) round-trips.
TEST(ActorReplication, StaticObjectRefRoundTrips) {
    for (uint32_t idx : {0u, 1u, 600u, 65535u, 0x1234567u}) {
        BitWriter w;
        ActorRepl::WriteNetGUID(w, NetGUIDRef{/*isDynamic=*/false, idx});
        std::vector<uint8_t> bytes = w.GetBytes();

        BitReader r(bytes);
        NetGUIDRef got = ActorRepl::ReadNetGUID(r);
        EXPECT_FALSE(r.IsOverflowed());
        EXPECT_FALSE(got.isDynamic);
        EXPECT_EQ(got.index, idx) << "static idx " << idx;
    }
}

// A dynamic actor reference (flag bit 1, value = channel index, bound 1024) round-trips.
TEST(ActorReplication, DynamicActorRefRoundTrips) {
    for (uint32_t idx : {0u, 2u, 54u, 511u, 1023u}) {  // channel indices < 1024
        BitWriter w;
        ActorRepl::WriteNetGUID(w, NetGUIDRef{/*isDynamic=*/true, idx});
        std::vector<uint8_t> bytes = w.GetBytes();

        BitReader r(bytes);
        NetGUIDRef got = ActorRepl::ReadNetGUID(r);
        EXPECT_FALSE(r.IsOverflowed());
        EXPECT_TRUE(got.isDynamic);
        EXPECT_EQ(got.index, idx) << "dynamic chIndex " << idx;
    }
}

// The SerializeNewActor header (actor NetGUID + class NetGUID) round-trips,
// modelling the common case: an exported actor referencing a known (static) class.
TEST(ActorReplication, NewActorHeaderRoundTrips) {
    NewActorHeader in;
    in.classGuid = NetGUIDRef{/*isDynamic=*/false, 0xABCDEu};  // static class/archetype ref
    in.actorGuid = NetGUIDRef{/*isDynamic=*/true, 42u};        // dynamic (channel index)

    BitWriter w;
    ActorRepl::WriteNewActorHeader(w, in);

    const std::vector<uint8_t> bytes = w.GetBytes();  // keep alive (BitReader views it)
    BitReader r(bytes);
    NewActorHeader out = ActorRepl::ReadNewActorHeader(r);
    EXPECT_FALSE(r.IsOverflowed());
    EXPECT_EQ(out.classGuid.isDynamic, in.classGuid.isDynamic);
    EXPECT_EQ(out.classGuid.index, in.classGuid.index);
    EXPECT_EQ(out.actorGuid.isDynamic, in.actorGuid.isDynamic);
    EXPECT_EQ(out.actorGuid.index, in.actorGuid.index);
}

// Capture anchor: the real ch2 PlayerController open (Session A f1484) begins
// `60 c1 01 00 ...`. The first (flag) bit is the static/dynamic selector and must
// decode as STATIC (bit 0) - the leading object ref is the CLASS/ARCHETYPE (a static
// object), confirmed by the UE3 source (UnNetDrv.cpp: bit 0 = static).
TEST(ActorReplication, CapturePlayerControllerOpenClassRefIsStatic) {
    // Real bytes from docs/RS2V_ActorReplication_7258.md §2.2 (ch2 PC open prefix).
    const std::vector<uint8_t> openPrefix = {
        0x60, 0xc1, 0x01, 0x00, 0x1b, 0xcc, 0xea, 0x3f, 0x49, 0x04, 0x60, 0xe0};

    BitReader r(openPrefix);
    NetGUIDRef cls = ActorRepl::ReadNetGUID(r);
    EXPECT_FALSE(r.IsOverflowed());
    EXPECT_FALSE(cls.isDynamic) << "the leading object ref is the class/archetype (static)";
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
    ActorRepl::WritePropObject(w, 30, maxHandle, NetGUIDRef{/*isDynamic=*/false, 600u});

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
    EXPECT_FALSE(g.isDynamic);
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
    hdr.classGuid = NetGUIDRef{/*isDynamic=*/false, 90u};    // static PlayerController class ref
    hdr.actorGuid = NetGUIDRef{/*isDynamic=*/true, 2u};      // dynamic (channel index)

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
    EXPECT_FALSE(out.classGuid.isDynamic);
    EXPECT_EQ(out.classGuid.index, 90u);
    EXPECT_TRUE(out.actorGuid.isDynamic);
    EXPECT_EQ(out.actorGuid.index, 2u);

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
