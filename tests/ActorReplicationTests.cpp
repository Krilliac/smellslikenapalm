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

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
