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
using ActorRepl::ActorOpenHeader;

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
    for (uint32_t idx : {0u, 2u, 54u, 511u, 1023u, 1500u, 2047u}) {  // channel indices < 2048
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

// ChangedTeams (ROPlayerController handle 172) RPC body encodes to a client-decodable
// bit layout. Mirrors src/Network/ConnectionManager.cpp's SelectTeam handler and the
// retail client's param decode. Proves the bytes we send are well-formed WITHOUT a live
// client. Param layout per UE3-src UnScript.cpp:2980-3010 (non-bool -> [Send][value];
// bool -> bare value bit). GameTypeClass idx 69601 = ROGameInfoTerritories (capture h33).
TEST(ActorReplication, ChangedTeamsRpcBodyRoundTrips) {
    constexpr uint32_t kMaxHandle = 531;   // ROPlayerController ClassNetCache maxHandle
    constexpr uint32_t kHandle    = 172;   // ChangedTeams
    constexpr uint32_t kClassIx   = 69601; // ROGameInfoTerritories static class index
    for (uint8_t teamId : {uint8_t{0}, uint8_t{1}}) {
        // --- encode exactly as the server does ---
        BitWriter fw;
        fw.SerializeInt(kHandle, kMaxHandle);
        if (teamId != 0) { fw.WriteBit(true); fw.WriteByte(teamId); }   // byte TeamIndex
        else             { fw.WriteBit(false); }
        fw.WriteBit(true);                                              // bShowRoleSelection
        fw.WriteBit(true);                                             // GameTypeClass Send
        ActorRepl::WriteNetGUID(fw, NetGUIDRef{/*isDynamic=*/false, kClassIx});
        fw.WriteBit(false);                                            // bTeamBalancing
        fw.WriteBit(false);                                            // bShowLobby

        // --- decode mirroring the client ---
        std::vector<uint8_t> bytes = fw.GetBytes();
        BitReader r(bytes.data(), bytes.size(), fw.NumBits());
        EXPECT_EQ(r.SerializeInt(kMaxHandle), kHandle);
        const bool sendTeam = r.ReadBit();
        EXPECT_EQ(sendTeam, teamId != 0) << "teamId " << int(teamId);
        const uint8_t gotTeam = sendTeam ? r.ReadByte() : uint8_t{0};
        EXPECT_EQ(gotTeam, teamId);
        EXPECT_TRUE(r.ReadBit());                       // bShowRoleSelection == true
        EXPECT_TRUE(r.ReadBit());                       // GameTypeClass present
        const NetGUIDRef gc = ActorRepl::ReadNetGUID(r);
        EXPECT_FALSE(gc.isDynamic);
        EXPECT_EQ(gc.index, kClassIx);                  // resolves to ROGameInfoTerritories
        EXPECT_FALSE(r.ReadBit());                      // bTeamBalancing == false
        EXPECT_FALSE(r.ReadBit());                      // bShowLobby == false
        EXPECT_FALSE(r.IsOverflowed());                 // consumed cleanly, no over-read
    }
}

// FVector::SerializeCompressed round-trips at integer precision (components are
// rounded to ints by the codec), including the zero vector and signed values.
TEST(ActorReplication, CompressedVectorRoundTrips) {
    struct V { float x, y, z; };
    for (const V& v : {V{0,0,0}, V{1,2,3}, V{-5,17,-1023}, V{1024,-2048,4095}}) {
        BitWriter w;
        ActorRepl::WriteCompressedVector(w, v.x, v.y, v.z);
        const std::vector<uint8_t> bytes = w.GetBytes();
        BitReader r(bytes.data(), bytes.size(), w.NumBits());
        float x, y, z;
        ActorRepl::ReadCompressedVector(r, x, y, z);
        EXPECT_FALSE(r.IsOverflowed());
        EXPECT_EQ(x, v.x); EXPECT_EQ(y, v.y); EXPECT_EQ(z, v.z);
        EXPECT_EQ(r.BitPos(), w.NumBits());
    }
}

// FRotator::SerializeCompressed: top byte per component, presence-bit gated.
TEST(ActorReplication, CompressedRotatorRoundTrips) {
    struct R { uint16_t p, y, r; };
    for (const R& rot : {R{0,0,0}, R{0,0x4000,0}, R{0x8000,0x4000,0xC000}}) {
        BitWriter w;
        ActorRepl::WriteCompressedRotator(w, rot.p, rot.y, rot.r);
        const std::vector<uint8_t> bytes = w.GetBytes();
        BitReader r(bytes.data(), bytes.size(), w.NumBits());
        uint16_t p, y, rl;
        ActorRepl::ReadCompressedRotator(r, p, y, rl);
        EXPECT_FALSE(r.IsOverflowed());
        // Only the top byte (>>8 <<8) survives the codec.
        EXPECT_EQ(p, rot.p & 0xFF00); EXPECT_EQ(y, rot.y & 0xFF00); EXPECT_EQ(rl, rot.r & 0xFF00);
    }
}

// The actor-open header (class ref + compressed Location + NetPlayerIndex for a PC)
// writes and reads back, modelling the owning client's PlayerController open.
TEST(ActorReplication, ActorOpenHeaderRoundTrips) {
    ActorOpenHeader in;
    in.classRef = NetGUIDRef{/*isDynamic=*/false, 0xABCDEu};  // static PlayerController class
    in.locX = 100; in.locY = -250; in.locZ = 64;
    in.isPlayerController = true;
    in.netPlayerIndex = 0;  // owning client

    BitWriter w;
    ActorRepl::WriteActorOpenHeader(w, in);
    const std::vector<uint8_t> bytes = w.GetBytes();
    BitReader r(bytes.data(), bytes.size(), w.NumBits());

    NetGUIDRef cls = ActorRepl::ReadNetGUID(r);
    EXPECT_FALSE(cls.isDynamic);
    EXPECT_EQ(cls.index, 0xABCDEu);
    float x, y, z; ActorRepl::ReadCompressedVector(r, x, y, z);
    EXPECT_EQ(x, 100); EXPECT_EQ(y, -250); EXPECT_EQ(z, 64);
    EXPECT_EQ(r.ReadByte(), 0u);  // NetPlayerIndex = raw byte (8 bits)
    EXPECT_FALSE(r.IsOverflowed());
    EXPECT_EQ(r.BitPos(), w.NumBits());
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
    ActorOpenHeader hdr;
    hdr.classRef = NetGUIDRef{/*isDynamic=*/false, 90u};  // static PlayerController class ref
    hdr.isPlayerController = true;
    hdr.netPlayerIndex = 0;

    PacketCodec::Bunch b = ActorRepl::MakeOpeningActorBunch(
        /*chIndex=*/2, /*chSeq=*/1, hdr,
        [&](BitWriter& w) {
            // Role/RemoteRole are byte(enum ENetRole) -> 3-bit enum encoding (UE3
            // appCeilLogTwo(NumEnums-1)), NOT a plain 8-bit byte.
            ActorRepl::WritePropByte(w, 6, maxHandle, 2, /*numBits=*/3);  // RemoteRole=AutonomousProxy
            ActorRepl::WritePropByte(w, 7, maxHandle, 3, /*numBits=*/3);  // Role=Authority
            ActorRepl::WritePropBool(w, 8, maxHandle, true); // bNetOwner
        });

    EXPECT_FALSE(b.bControl);
    EXPECT_TRUE(b.bOpen);
    EXPECT_TRUE(b.bReliable);
    EXPECT_EQ(b.chIndex, 2u);
    EXPECT_EQ(b.chType, 2u);
    EXPECT_EQ(b.chSequence, 1u);

    BitReader r(b.payload.data(), b.payload.size(), b.payloadBits);
    NetGUIDRef cls = ActorRepl::ReadNetGUID(r);
    EXPECT_FALSE(cls.isDynamic);
    EXPECT_EQ(cls.index, 90u);
    float lx, ly, lz; ActorRepl::ReadCompressedVector(r, lx, ly, lz);  // Location
    EXPECT_EQ(r.ReadByte(), 0u);      // NetPlayerIndex = raw byte (8 bits)

    EXPECT_EQ(r.SerializeInt(maxHandle), 6u);  EXPECT_EQ(r.ReadBits(3), 2u);  // 3-bit enum
    EXPECT_EQ(r.SerializeInt(maxHandle), 7u);  EXPECT_EQ(r.ReadBits(3), 3u);  // 3-bit enum
    EXPECT_EQ(r.SerializeInt(maxHandle), 8u);  EXPECT_TRUE(r.ReadBit());
    EXPECT_FALSE(r.IsOverflowed());
    EXPECT_EQ(r.BitPos(), static_cast<size_t>(b.payloadBits));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
