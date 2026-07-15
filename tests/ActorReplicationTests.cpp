// tests/ActorReplicationTests.cpp
//
// Tests for the actor-channel replication primitives (src/Network/ActorReplication).
// The NetGUID/object-reference codec is the foundation of every per-session actor
// channel; these round-trip it and anchor it to real-capture bytes.
// Spec: docs/RS2V_ActorReplication_7258.md.

#include "TestFramework.h"

#include "Network/ActorReplication.h"
#include "Game/TeamMapping.h"
#include "Network/MantleReplication.h"
#include "Network/MovementReplication.h"
#include "Network/GameplayRpcReplication.h"
#include "Network/BitReader.h"
#include "Network/BitWriter.h"
#include "Network/ObjectiveReplication.h"

#include <vector>
#include <array>
#include <cstdint>
#include <limits>
#include <string>

using ActorRepl::NetGUIDRef;
using ActorRepl::ActorOpenHeader;

namespace {

std::vector<uint8_t> Hex(const char* text) {
    const std::string input = text ? text : "";
    std::vector<uint8_t> bytes;
    bytes.reserve(input.size() / 2);
    auto nibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
        return 0;
    };
    for (size_t i = 0; i + 1 < input.size(); i += 2) {
        bytes.push_back(static_cast<uint8_t>((nibble(input[i]) << 4) |
                                             nibble(input[i + 1])));
    }
    return bytes;
}

} // namespace

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
    for (uint32_t idx : {0u, 2u, 54u, 511u, 1023u}) {
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

// Capture anchor: Controller.PlayerReplicationInfo (handle 23) -> dynamic ch26
// is the exact 20-bit payload `17 6a 00` streamed by the official server.
TEST(ActorReplication, PlayerControllerPriLinkMatchesRetailCapture) {
    BitWriter w;
    w.SerializeInt(23, 531);
    ActorRepl::WriteNetGUID(w, NetGUIDRef{/*isDynamic=*/true, 26u});
    EXPECT_EQ(w.NumBits(), 20u);
    const std::vector<uint8_t> expected = {0x17, 0x6A, 0x00};
    EXPECT_EQ(w.GetBytes(), expected);
}

// Retail possession anchor. UE3 SerializeInt uses a value-dependent width, so these
// payloads must be generated through BitWriter rather than fixed-width hand packing.
// Non-bool RPC parameters also carry a one-bit "present" marker before their value.
TEST(ActorReplication, PawnPossessionPayloadsUseRangedHandlesAndDynamicRefs) {
    constexpr uint32_t kRoPcMaxHandle = 531;
    constexpr uint32_t kPawnMaxHandle = 168; // netfields_u_ROPawn handles 0..167
    constexpr uint32_t kPawnChannel = 209;

    struct PropertyCase {
        uint32_t handle;
        uint32_t maxHandle;
        uint32_t channel;
        size_t bits;
        std::vector<uint8_t> bytes;
    };
    const std::vector<PropertyCase> properties = {
        {52, kPawnMaxHandle, 2,            18, {0xB4, 0x02, 0x00}}, // Pawn.Controller
        {32, kPawnMaxHandle, 26,           19, {0x20, 0x35, 0x00}}, // Pawn.PRI
        {24, kRoPcMaxHandle, kPawnChannel, 20, {0x18, 0x46, 0x03}}, // PC.Pawn
    };
    for (const PropertyCase& c : properties) {
        BitWriter w;
        ActorRepl::WritePropObject(w, c.handle, c.maxHandle,
                                   NetGUIDRef{/*isDynamic=*/true, c.channel});
        EXPECT_EQ(w.NumBits(), c.bits) << "handle " << c.handle;
        const std::vector<uint8_t> encoded = w.GetBytes();
        EXPECT_EQ(encoded, c.bytes) << "handle " << c.handle;

        BitReader r(encoded.data(), encoded.size(), w.NumBits());
        EXPECT_EQ(r.SerializeInt(c.maxHandle), c.handle);
        const NetGUIDRef ref = ActorRepl::ReadNetGUID(r);
        EXPECT_TRUE(ref.isDynamic);
        EXPECT_EQ(ref.index, c.channel);
        EXPECT_FALSE(r.IsOverflowed());
        EXPECT_EQ(r.BitPos(), w.NumBits());
    }

    struct RpcCase {
        uint32_t handle;
        std::vector<uint8_t> bytes;
    };
    const std::vector<RpcCase> rpcs = {
        {85, {0x55, 0x8E, 0x06}}, // ClientRestart(Pawn)
        {43, {0x2B, 0x8E, 0x06}}, // GivePawn(Pawn)
        {150, {0x96, 0x8E, 0x06}}, // ClientOnPossess(Pawn)
    };
    for (const RpcCase& c : rpcs) {
        BitWriter w;
        w.SerializeInt(c.handle, kRoPcMaxHandle);
        w.WriteBit(true); // Pawn parameter present
        ActorRepl::WriteNetGUID(w, NetGUIDRef{/*isDynamic=*/true, kPawnChannel});
        EXPECT_EQ(w.NumBits(), 21u) << "handle " << c.handle;
        const std::vector<uint8_t> encoded = w.GetBytes();
        EXPECT_EQ(encoded, c.bytes) << "handle " << c.handle;

        BitReader r(encoded.data(), encoded.size(), w.NumBits());
        EXPECT_EQ(r.SerializeInt(kRoPcMaxHandle), c.handle);
        EXPECT_TRUE(r.ReadBit());
        const NetGUIDRef ref = ActorRepl::ReadNetGUID(r);
        EXPECT_TRUE(ref.isDynamic);
        EXPECT_EQ(ref.index, kPawnChannel);
        EXPECT_FALSE(r.IsOverflowed());
        EXPECT_EQ(r.BitPos(), w.NumBits());
    }

    // ROPawn.ClientPossessed() has no parameters and is exactly a seven-bit h57.
    BitWriter clientPossessed;
    clientPossessed.SerializeInt(57, kPawnMaxHandle);
    EXPECT_EQ(clientPossessed.NumBits(), 7u);
    EXPECT_EQ(clientPossessed.GetBytes(), std::vector<uint8_t>({0x39}));

    BitWriter clientOnDead;
    clientOnDead.SerializeInt(151, kRoPcMaxHandle);
    clientOnDead.WriteBit(false);
    EXPECT_EQ(clientOnDead.NumBits(), 10u);
    EXPECT_EQ(clientOnDead.GetBytes(), std::vector<uint8_t>({0x97, 0x00}));

    // ChangedRole closes the role/unit scene and opens Spawn Select; h150 in the
    // possession burst closes Spawn Select. This is the capture-exact initial-role
    // form: SquadIndex=255, ClassIndex=0, no lobby, show spawn select.
    BitWriter changedRole;
    changedRole.SerializeInt(210, kRoPcMaxHandle);
    changedRole.WriteBit(true);
    changedRole.WriteByte(255);
    changedRole.WriteBit(false);
    changedRole.WriteBit(false);
    changedRole.WriteBit(true);
    EXPECT_EQ(changedRole.NumBits(), 21u);
    EXPECT_EQ(changedRole.GetBytes(), std::vector<uint8_t>({0xD2, 0xFE, 0x13}));

    // Official retail possession burst, reconstructed field-by-field for an alive
    // owning pawn on ch209. The dead=true capture ends in 0x05; alive=false is 0x01.
    BitWriter burst;
    burst.SerializeInt(85, kRoPcMaxHandle);
    burst.WriteBit(true);
    ActorRepl::WriteNetGUID(burst, NetGUIDRef{/*isDynamic=*/true, kPawnChannel});
    burst.SerializeInt(87, kRoPcMaxHandle);
    burst.WriteBit(true);
    ActorRepl::WriteNetGUID(burst, NetGUIDRef{/*isDynamic=*/true, kPawnChannel});
    burst.WriteBit(true);
    burst.WriteFloat(0.0f);
    burst.SerializeInt(1, 6);
    burst.WriteFloat(2.0f);
    burst.WriteBit(false);
    burst.SerializeInt(61, kRoPcMaxHandle);
    burst.WriteBit(true);
    burst.WriteBit(false);
    burst.WriteString("FirstPerson");
    burst.WriteInt32(0);
    burst.SerializeInt(265, kRoPcMaxHandle);
    burst.WriteBit(false);
    burst.SerializeInt(150, kRoPcMaxHandle);
    burst.WriteBit(true);
    ActorRepl::WriteNetGUID(burst, NetGUIDRef{/*isDynamic=*/true, kPawnChannel});
    burst.SerializeInt(151, kRoPcMaxHandle);
    burst.WriteBit(false);

    EXPECT_EQ(burst.NumBits(), 323u);
    const std::vector<uint8_t> expectedBurst = {
        0x55,0x8E,0xE6,0xCA,0xD1,0x04,0x00,0x00,0x00,0x08,0x00,0x00,0x00,
        0x90,0x1E,0x31,0x00,0x00,0x00,0x18,0xA5,0xC9,0xCD,0xD1,0x41,0x95,
        0xC9,0xCD,0xBD,0xB9,0x01,0x00,0x00,0x00,0x00,0x24,0x64,0xE9,0x68,
        0x2E,0x01
    };
    EXPECT_EQ(burst.GetBytes(), expectedBurst);
}

// Official post-Join menu sequence: ClientShowTeamSelect is followed by the
// capture-exact 22-bit ClientGotoState payload `29 ce 1c` (ROPC handle 41 plus
// its two FName parameters). Keep the opaque name payload bit-exact.
TEST(ActorReplication, ClientGotoStatePayloadMatchesRetailCapture) {
    const std::vector<uint8_t> payload = {0x29, 0xCE, 0x1C};
    BitReader r(payload.data(), payload.size(), /*numBits=*/22);
    EXPECT_EQ(r.SerializeInt(531), 41u);
    EXPECT_FALSE(r.IsOverflowed());
    EXPECT_EQ(22u - r.BitPos(), 13u);
}

TEST(ActorReplication, PostLoadoutCameraTailMatchesRetailCapture) {
    constexpr uint32_t kRoPcMaxHandle = 531;
    BitWriter w;
    w.SerializeInt(28, kRoPcMaxHandle);
    w.WriteBit(false);
    w.SerializeInt(28, kRoPcMaxHandle);
    w.WriteBit(false);
    w.SerializeInt(168, kRoPcMaxHandle); // ClientCameraReset()
    w.SerializeInt(87, kRoPcMaxHandle);  // ClientSetViewTarget(ch2, transition)
    w.WriteBit(true);
    ActorRepl::WriteNetGUID(w, NetGUIDRef{/*isDynamic=*/true, 2u});
    w.WriteBit(true);
    w.WriteFloat(0.0f);
    w.SerializeInt(1, 6);
    w.WriteFloat(2.0f);
    w.WriteBit(false);

    EXPECT_EQ(w.NumBits(), 119u);
    EXPECT_EQ(w.GetBytes(), Hex("1c7080eaca02040000000800000010"));

    const std::vector<uint8_t> bytes = w.GetBytes();
    BitReader r(bytes.data(), bytes.size(), w.NumBits());
    EXPECT_EQ(r.SerializeInt(kRoPcMaxHandle), 28u);
    EXPECT_FALSE(r.ReadBit());
    EXPECT_EQ(r.SerializeInt(kRoPcMaxHandle), 28u);
    EXPECT_FALSE(r.ReadBit());
    EXPECT_EQ(r.SerializeInt(kRoPcMaxHandle), 168u);
    EXPECT_EQ(r.SerializeInt(kRoPcMaxHandle), 87u);
    EXPECT_TRUE(r.ReadBit());
    const NetGUIDRef viewTarget = ActorRepl::ReadNetGUID(r);
    EXPECT_TRUE(viewTarget.isDynamic);
    EXPECT_EQ(viewTarget.index, 2u);
    EXPECT_TRUE(r.ReadBit());
    EXPECT_FLOAT_EQ(r.ReadFloat(), 0.0f);
    EXPECT_EQ(r.SerializeInt(6), 1u);
    EXPECT_FLOAT_EQ(r.ReadFloat(), 2.0f);
    EXPECT_FALSE(r.ReadBit());
    EXPECT_FALSE(r.IsOverflowed());
    EXPECT_EQ(r.BitPos(), w.NumBits());
}

TEST(ActorReplication, ActiveGriMatchStateMatchesRetailCapture) {
    BitWriter w;
    ActorRepl::WritePropBool(w, ObjectiveRepl::kStopCountDown,
                             ObjectiveRepl::kGriMaxHandle, false);
    ActorRepl::WritePropBool(w, ObjectiveRepl::kMatchHasBegun,
                             ObjectiveRepl::kGriMaxHandle, true);

    EXPECT_EQ(w.NumBits(), 18u);
    EXPECT_EQ(w.GetBytes(), Hex("203e02"));

    const std::vector<uint8_t> bytes = w.GetBytes();
    BitReader r(bytes.data(), bytes.size(), w.NumBits());
    EXPECT_EQ(r.SerializeInt(ObjectiveRepl::kGriMaxHandle),
              ObjectiveRepl::kStopCountDown);
    EXPECT_FALSE(r.ReadBit());
    EXPECT_EQ(r.SerializeInt(ObjectiveRepl::kGriMaxHandle),
              ObjectiveRepl::kMatchHasBegun);
    EXPECT_TRUE(r.ReadBit());
    EXPECT_FALSE(r.IsOverflowed());
    EXPECT_EQ(r.BitPos(), w.NumBits());
}

TEST(ActorReplication, PawnWeaponAttachmentStateMatchesRetailCapture) {
    constexpr uint32_t kPawnMaxHandle = 168;
    struct AttachmentCase {
        uint8_t slot;
        uint32_t classRef;
    };
    const std::array<AttachmentCase, 5> attachments{{
        {0, 286936}, {1, 286946}, {2, 287063},
        {3, 286944}, {5, 286126},
    }};

    BitWriter list;
    for (const AttachmentCase& attachment : attachments) {
        list.SerializeInt(167, kPawnMaxHandle);
        list.WriteByte(attachment.slot);
        list.WriteBit(false);
        ActorRepl::WriteNetGUID(
            list, NetGUIDRef{/*isDynamic=*/false, attachment.classRef});
    }
    EXPECT_EQ(list.NumBits(), 245u);
    EXPECT_EQ(list.GetBytes(),
              Hex("a700608311004e03100723009c0a70154600381d001c8c00705a806b170100"));

    const std::vector<uint8_t> listBytes = list.GetBytes();
    BitReader listReader(listBytes.data(), listBytes.size(), list.NumBits());
    for (const AttachmentCase& attachment : attachments) {
        EXPECT_EQ(listReader.SerializeInt(kPawnMaxHandle), 167u);
        EXPECT_EQ(listReader.ReadByte(), attachment.slot);
        EXPECT_FALSE(listReader.ReadBit());
        const NetGUIDRef classRef = ActorRepl::ReadNetGUID(listReader);
        EXPECT_FALSE(classRef.isDynamic);
        EXPECT_EQ(classRef.index, attachment.classRef);
    }
    EXPECT_FALSE(listReader.IsOverflowed());
    EXPECT_EQ(listReader.BitPos(), list.NumBits());

    BitWriter current;
    ActorRepl::WritePropObject(
        current, 147, kPawnMaxHandle,
        NetGUIDRef{/*isDynamic=*/false, 286936u});
    EXPECT_EQ(current.NumBits(), 40u);
    EXPECT_EQ(current.GetBytes(), Hex("93b0c10800"));

    BitWriter cleared;
    ActorRepl::WritePropObject(
        cleared, 147, kPawnMaxHandle,
        NetGUIDRef{/*isDynamic=*/true, 0u});
    EXPECT_EQ(cleared.NumBits(), 19u);
    EXPECT_EQ(cleared.GetBytes(), Hex("930100"));
    const std::vector<uint8_t> clearedBytes = cleared.GetBytes();
    BitReader clearedReader(
        clearedBytes.data(), clearedBytes.size(), cleared.NumBits());
    EXPECT_EQ(clearedReader.SerializeInt(kPawnMaxHandle), 147u);
    const NetGUIDRef noneRef = ActorRepl::ReadNetGUID(clearedReader);
    EXPECT_TRUE(noneRef.isDynamic);
    EXPECT_EQ(noneRef.index, 0u);
    EXPECT_FALSE(clearedReader.IsOverflowed());
    EXPECT_EQ(clearedReader.BitPos(), cleared.NumBits());
}

TEST(ActorReplication, SpawnLoadoutGraphLinksPawnManagerAndWeaponChain) {
    constexpr uint32_t kPawnMaxHandle = 168;
    constexpr uint32_t kWeaponMaxHandle = 99;

    BitWriter pawnManager;
    ActorRepl::WritePropObject(pawnManager, 27, kPawnMaxHandle,
                               NetGUIDRef{/*isDynamic=*/true, 219u});
    const std::vector<uint8_t> pawnBytes = pawnManager.GetBytes();
    EXPECT_EQ(pawnManager.NumBits(), 19u);
    EXPECT_EQ(pawnBytes, Hex("1bb701"));
    BitReader pawnReader(pawnBytes.data(), pawnBytes.size(), pawnManager.NumBits());
    EXPECT_EQ(pawnReader.SerializeInt(kPawnMaxHandle), 27u);
    NetGUIDRef managerRef = ActorRepl::ReadNetGUID(pawnReader);
    EXPECT_TRUE(managerRef.isDynamic);
    EXPECT_EQ(managerRef.index, 219u);
    EXPECT_FALSE(pawnReader.IsOverflowed());

    BitWriter graph;
    ActorRepl::WritePropObject(graph, 23, kWeaponMaxHandle,
                               NetGUIDRef{/*isDynamic=*/true, 219u});
    ActorRepl::WritePropObject(graph, 24, kWeaponMaxHandle,
                               NetGUIDRef{/*isDynamic=*/true, 211u});
    graph.SerializeInt(25, kWeaponMaxHandle); // ClientGivenTo
    graph.WriteBit(true);                     // NewOwner present
    ActorRepl::WriteNetGUID(graph, NetGUIDRef{/*isDynamic=*/true, 209u});
    graph.WriteBit(false);                    // bDoNotActivate

    const std::vector<uint8_t> graphBytes = graph.GetBytes();
    EXPECT_EQ(graph.NumBits(), 56u);
    EXPECT_EQ(graphBytes, Hex("97db604e93391a"));
    BitReader r(graphBytes.data(), graphBytes.size(), graph.NumBits());
    EXPECT_EQ(r.SerializeInt(kWeaponMaxHandle), 23u);
    NetGUIDRef invManager = ActorRepl::ReadNetGUID(r);
    EXPECT_TRUE(invManager.isDynamic);
    EXPECT_EQ(invManager.index, 219u);
    EXPECT_EQ(r.SerializeInt(kWeaponMaxHandle), 24u);
    NetGUIDRef nextWeapon = ActorRepl::ReadNetGUID(r);
    EXPECT_TRUE(nextWeapon.isDynamic);
    EXPECT_EQ(nextWeapon.index, 211u);
    EXPECT_EQ(r.SerializeInt(kWeaponMaxHandle), 25u);
    EXPECT_TRUE(r.ReadBit());
    NetGUIDRef newOwner = ActorRepl::ReadNetGUID(r);
    EXPECT_TRUE(newOwner.isDynamic);
    EXPECT_EQ(newOwner.index, 209u);
    EXPECT_FALSE(r.ReadBit());
    EXPECT_FALSE(r.IsOverflowed());
    EXPECT_EQ(r.BitPos(), graph.NumBits());

    // Retail f27395 omits final ch214 h24 rather than explicitly serializing
    // None. ch214 also uses the 101-entry grenade field table.
    constexpr uint32_t kM18MaxHandle = 101;
    BitWriter finalGraph;
    ActorRepl::WritePropObject(finalGraph, 23, kM18MaxHandle,
                               NetGUIDRef{/*isDynamic=*/true, 219u});
    finalGraph.SerializeInt(25, kM18MaxHandle);
    finalGraph.WriteBit(true);
    ActorRepl::WriteNetGUID(finalGraph,
                            NetGUIDRef{/*isDynamic=*/true, 209u});
    finalGraph.WriteBit(false);
    const std::vector<uint8_t> finalBytes = finalGraph.GetBytes();
    BitReader finalReader(finalBytes.data(), finalBytes.size(), finalGraph.NumBits());
    EXPECT_EQ(finalReader.SerializeInt(kM18MaxHandle), 23u);
    const NetGUIDRef finalManager = ActorRepl::ReadNetGUID(finalReader);
    EXPECT_TRUE(finalManager.isDynamic);
    EXPECT_EQ(finalManager.index, 219u);
    EXPECT_EQ(finalReader.SerializeInt(kM18MaxHandle), 25u);
    EXPECT_TRUE(finalReader.ReadBit());
    const NetGUIDRef finalOwner = ActorRepl::ReadNetGUID(finalReader);
    EXPECT_TRUE(finalOwner.isDynamic);
    EXPECT_EQ(finalOwner.index, 209u);
    EXPECT_FALSE(finalReader.ReadBit());
    EXPECT_FALSE(finalReader.IsOverflowed());
    EXPECT_EQ(finalReader.BitPos(), finalGraph.NumBits());

    BitWriter switchBest;
    switchBest.SerializeInt(28, 531);
    switchBest.WriteBit(false);
    EXPECT_EQ(switchBest.NumBits(), 10u);
    EXPECT_EQ(switchBest.GetBytes(), Hex("1c00"));
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

TEST(ActorReplication, RewritesCapturedActorOpenClassAndLocationStructurally) {
    // Verbatim local-owner ROPawn opening from official capture f27394. The
    // canonical dedicated-server PackageMap resolves its leading archetype at
    // 286151; the installed retail layout resolves the same archetype at 286156.
    const std::vector<uint8_t> original = Hex(
        "8ebb0800bdce01f8eef70c0a80a043901ad00a009308409618409628409638409648409658409668"
        "e09178409688409198a090a8e091b84096c84091d8a090e83092f8409608419118a190283192384"
        "19648419158a190684196784176189003");
    constexpr uint32_t kOriginalBits = 763;
    constexpr uint32_t kCanonicalClassRef = 286151;
    constexpr uint32_t kInstalledClassRef = 286156;

    BitReader originalReader(original.data(), original.size(), kOriginalBits);
    const NetGUIDRef originalClass = ActorRepl::ReadNetGUID(originalReader);
    float originalX = 0.f, originalY = 0.f, originalZ = 0.f;
    ActorRepl::ReadCompressedVector(originalReader, originalX, originalY, originalZ);
    ASSERT_FALSE(originalReader.IsOverflowed());
    ASSERT_FALSE(originalClass.isDynamic);
    ASSERT_EQ(originalClass.index, kCanonicalClassRef);
    const size_t originalTailBits = originalReader.BitsLeft();

    std::vector<uint8_t> rewritten;
    uint32_t rewrittenBits = 0;
    ASSERT_TRUE(ActorRepl::RewriteCapturedActorOpenClassAndLocation(
        original.data(), original.size(), kOriginalBits,
        kCanonicalClassRef, kInstalledClassRef,
        -92368.33f, 6117.922f, -491.0f,
        rewritten, rewrittenBits));

    BitReader rewrittenReader(rewritten.data(), rewritten.size(), rewrittenBits);
    const NetGUIDRef rewrittenClass = ActorRepl::ReadNetGUID(rewrittenReader);
    float rewrittenX = 0.f, rewrittenY = 0.f, rewrittenZ = 0.f;
    ActorRepl::ReadCompressedVector(
        rewrittenReader, rewrittenX, rewrittenY, rewrittenZ);
    ASSERT_FALSE(rewrittenReader.IsOverflowed());
    EXPECT_FALSE(rewrittenClass.isDynamic);
    EXPECT_EQ(rewrittenClass.index, kInstalledClassRef);
    EXPECT_EQ(rewrittenX, -92368.0f);
    EXPECT_EQ(rewrittenY, 6118.0f);
    EXPECT_EQ(rewrittenZ, -491.0f);
    ASSERT_EQ(rewrittenReader.BitsLeft(), originalTailBits);

    // Compare from the independently decoded tail boundaries: the Location's
    // compressed width changed, but the opaque property graph must not.
    for (size_t bit = 0; bit < originalTailBits; ++bit) {
        EXPECT_EQ(rewrittenReader.ReadBit(), originalReader.ReadBit())
            << "opaque tail bit " << bit;
    }
    EXPECT_FALSE(originalReader.IsOverflowed());
    EXPECT_FALSE(rewrittenReader.IsOverflowed());
    EXPECT_EQ(originalReader.BitsLeft(), 0u);
    EXPECT_EQ(rewrittenReader.BitsLeft(), 0u);
}

TEST(ActorReplication, CapturedActorOpenClassRewriteFailsAtomicallyOnDrift) {
    const std::vector<uint8_t> original = Hex(
        "8ebb0800bdce01f8eef70c0a80a043901ad00a009308409618409628409638409648409658409668"
        "e09178409688409198a090a8e091b84096c84091d8a090e83092f8409608419118a190283192384"
        "19648419158a190684196784176189003");
    constexpr uint32_t kOriginalBits = 763;
    constexpr uint32_t kCanonicalClassRef = 286151;
    constexpr uint32_t kInstalledClassRef = 286156;

    auto rejected = [&](const uint8_t* data, size_t bytes, uint32_t bits,
                        uint32_t expectedClassRef,
                        uint32_t replacementClassRef,
                        float x = 1.f, float y = 2.f, float z = 3.f) {
        std::vector<uint8_t> output = {0xAA};
        uint32_t outputBits = 999;
        EXPECT_FALSE(ActorRepl::RewriteCapturedActorOpenClassAndLocation(
            data, bytes, bits, expectedClassRef, replacementClassRef,
            x, y, z, output, outputBits));
        EXPECT_TRUE(output.empty());
        EXPECT_EQ(outputBits, 0u);
    };

    rejected(nullptr, original.size(), kOriginalBits,
             kCanonicalClassRef, kInstalledClassRef);
    rejected(original.data(), original.size(), 0,
             kCanonicalClassRef, kInstalledClassRef);
    rejected(original.data(), original.size(),
             static_cast<uint32_t>(BitWriter::kMaxSaneBunchBits + 1u),
             kCanonicalClassRef, kInstalledClassRef);
    rejected(original.data(), original.size() - 1, kOriginalBits,
             kCanonicalClassRef, kInstalledClassRef);
    rejected(original.data(), original.size(), 31,
             kCanonicalClassRef, kInstalledClassRef); // truncated static ref
    rejected(original.data(), original.size(), 40,
             kCanonicalClassRef, kInstalledClassRef); // truncated Location
    rejected(original.data(), original.size(), kOriginalBits,
             kCanonicalClassRef + 1, kInstalledClassRef); // capture drift
    rejected(original.data(), original.size(), kOriginalBits,
             0, kInstalledClassRef);
    rejected(original.data(), original.size(), kOriginalBits,
             kCanonicalClassRef, 0);
    rejected(original.data(), original.size(), kOriginalBits,
             ActorRepl::kStaticObjectMax, kInstalledClassRef);
    rejected(original.data(), original.size(), kOriginalBits,
             kCanonicalClassRef, ActorRepl::kStaticObjectMax);
    rejected(original.data(), original.size(), kOriginalBits,
             kCanonicalClassRef, kInstalledClassRef,
             1048576.0f, 2.0f, 3.0f);

    BitWriter dynamicOpening;
    ActorRepl::WriteNetGUID(
        dynamicOpening, NetGUIDRef{/*isDynamic=*/true, 7u});
    ActorRepl::WriteCompressedVector(dynamicOpening, 1, 2, 3);
    dynamicOpening.WriteBit(true);
    const std::vector<uint8_t> dynamicBytes = dynamicOpening.GetBytes();
    rejected(dynamicBytes.data(), dynamicBytes.size(),
             static_cast<uint32_t>(dynamicOpening.NumBits()),
             7, kInstalledClassRef);

    // A static zero is not None and can never be an actor-open archetype.
    BitWriter zeroStaticOpening;
    zeroStaticOpening.WriteBit(false);
    zeroStaticOpening.SerializeInt(0, ActorRepl::kStaticObjectMax);
    ActorRepl::WriteCompressedVector(zeroStaticOpening, 1, 2, 3);
    const std::vector<uint8_t> zeroStaticBytes = zeroStaticOpening.GetBytes();
    rejected(zeroStaticBytes.data(), zeroStaticBytes.size(),
             static_cast<uint32_t>(zeroStaticOpening.NumBits()),
             1, kInstalledClassRef);

    // A source at the sane bunch ceiling is valid, but expanding its compressed
    // Location would make the rewritten actor opening unsafe and must fail before
    // any partial result is published.
    BitWriter ceilingOpening;
    ActorRepl::WriteNetGUID(
        ceilingOpening, NetGUIDRef{/*isDynamic=*/false, kCanonicalClassRef});
    ActorRepl::WriteCompressedVector(ceilingOpening, 0, 0, 0);
    while (ceilingOpening.NumBits() < BitWriter::kMaxSaneBunchBits) {
        ceilingOpening.WriteBit((ceilingOpening.NumBits() & 1u) != 0);
    }
    ASSERT_EQ(ceilingOpening.NumBits(), BitWriter::kMaxSaneBunchBits);
    const std::vector<uint8_t> ceilingBytes = ceilingOpening.GetBytes();
    rejected(ceilingBytes.data(), ceilingBytes.size(),
             static_cast<uint32_t>(ceilingOpening.NumBits()),
             kCanonicalClassRef, kInstalledClassRef,
             1048575.0f, 1048575.0f, 1048575.0f);

    EXPECT_EQ(original, Hex(
        "8ebb0800bdce01f8eef70c0a80a043901ad00a009308409618409628409638409648409658409668"
        "e09178409688409198a090a8e091b84096c84091d8a090e83092f8409608419118a190283192384"
        "19648419158a190684196784176189003"));
}

TEST(ActorReplication, RewritesCapturedPawnOpenLocationAndPreservesOpaqueTail) {
    // Verbatim local-owner ROPawn opening from official capture f27394. The
    // meaningful payload is 763 bits; the five byte-alignment pad bits are not
    // part of the captured opening and must not be copied into the result.
    const std::vector<uint8_t> original = Hex(
        "8ebb0800bdce01f8eef70c0a80a043901ad00a009308409618409628409638409648409658409668"
        "e09178409688409198a090a8e091b84096c84091d8a090e83092f8409608419118a190283192384"
        "19648419158a190684196784176189003");
    constexpr uint32_t kOriginalBits = 763;

    BitReader originalReader(original.data(), original.size(), kOriginalBits);
    const NetGUIDRef originalClass = ActorRepl::ReadNetGUID(originalReader);
    const size_t originalClassBits = originalReader.BitPos();
    float originalX = 0.f, originalY = 0.f, originalZ = 0.f;
    ActorRepl::ReadCompressedVector(originalReader, originalX, originalY, originalZ);
    ASSERT_FALSE(originalReader.IsOverflowed());
    EXPECT_FALSE(originalClass.isDynamic);
    EXPECT_EQ(originalClass.index, 286151u);
    EXPECT_EQ(originalX, -8981.0f);
    EXPECT_EQ(originalY, 7936.0f);
    EXPECT_EQ(originalZ, -517.0f);
    const size_t originalTailStart = originalReader.BitPos();
    const size_t originalTailBits = originalReader.BitsLeft();

    std::vector<uint8_t> rewritten;
    uint32_t rewrittenBits = 0;
    ASSERT_TRUE(ActorRepl::RewriteCapturedActorOpenLocation(
        original.data(), original.size(), kOriginalBits,
        123.0f, -456.0f, 78.0f, rewritten, rewrittenBits));

    BitReader rewrittenReader(rewritten.data(), rewritten.size(), rewrittenBits);
    const NetGUIDRef rewrittenClass = ActorRepl::ReadNetGUID(rewrittenReader);
    const size_t rewrittenClassBits = rewrittenReader.BitPos();
    float rewrittenX = 0.f, rewrittenY = 0.f, rewrittenZ = 0.f;
    ActorRepl::ReadCompressedVector(rewrittenReader, rewrittenX, rewrittenY, rewrittenZ);
    ASSERT_FALSE(rewrittenReader.IsOverflowed());
    EXPECT_EQ(rewrittenClass.isDynamic, originalClass.isDynamic);
    EXPECT_EQ(rewrittenClass.index, originalClass.index);
    EXPECT_EQ(rewrittenClassBits, originalClassBits);
    EXPECT_EQ(rewrittenX, 123.0f);
    EXPECT_EQ(rewrittenY, -456.0f);
    EXPECT_EQ(rewrittenZ, 78.0f);
    EXPECT_NE(rewrittenReader.BitPos(), originalTailStart)
        << "test target must exercise a different compressed-vector bit width";
    EXPECT_EQ(rewrittenReader.BitsLeft(), originalTailBits);

    BitReader originalClassReader(original.data(), original.size(), originalClassBits);
    BitReader rewrittenClassReader(rewritten.data(), rewritten.size(), rewrittenClassBits);
    for (size_t i = 0; i < originalClassBits; ++i) {
        EXPECT_EQ(rewrittenClassReader.ReadBit(), originalClassReader.ReadBit())
            << "class-reference bit " << i;
    }

    // Compare from each payload's independently decoded tail boundary. This
    // proves every property/RPC bit survives even though the prefix shifted.
    for (size_t i = 0; i < originalTailBits; ++i) {
        EXPECT_EQ(rewrittenReader.ReadBit(), originalReader.ReadBit())
            << "opaque tail bit " << i;
    }
    EXPECT_FALSE(originalReader.IsOverflowed());
    EXPECT_FALSE(rewrittenReader.IsOverflowed());
    EXPECT_EQ(originalReader.BitsLeft(), 0u);
    EXPECT_EQ(rewrittenReader.BitsLeft(), 0u);
}

TEST(ActorReplication, RewritesEveryCapturedInventoryOpenToPawnSpawnLocation) {
    struct CapturedOpen {
        uint16_t channel;
        uint32_t classRef;
        uint32_t bits;
        const char* hex;
    };
    const CapturedOpen opens[] = {
        {210, 286374, 301, "4cbd0800bdce01f8eef70ca3218cc6912b010000b03d000000c8fa010000648ec65c02000000"},
        {211, 286391, 301, "6ebd0800bdce01f8eef70ca3218cc6917b000000b015000000c83a000000648ec6fc00000000"},
        {212, 286464, 301, "00be0800bdce01f8eef70ca3218cc6911b000000b015000000c806000000648ec63c00000000"},
        {213, 286109, 152, "3abb0800bdce01f8eef70ca3218cc631733466"},
        {214, 286389, 262, "6abd0800bdce01f8eef70ca3218cc6911b000000b00d000000c81c8d7900000000"},
        {219, 82735, 405, "5e860200bdce01f8eef7cc68c868dc5b9a0e000000f8d3030000007fba00000000501f000000fce9050000803ffda0aaaa1008"},
    };

    for (const CapturedOpen& captured : opens) {
        const std::vector<uint8_t> original = Hex(captured.hex);
        std::vector<uint8_t> rewritten;
        uint32_t rewrittenBits = 0;
        ASSERT_TRUE(ActorRepl::RewriteCapturedActorOpenLocation(
            original.data(), original.size(), captured.bits,
            -13100.0f, 5239.0f, -520.0f,
            rewritten, rewrittenBits));

        BitReader r(rewritten.data(), rewritten.size(), rewrittenBits);
        const NetGUIDRef classRef = ActorRepl::ReadNetGUID(r);
        float x = 0.f, y = 0.f, z = 0.f;
        ActorRepl::ReadCompressedVector(r, x, y, z);
        EXPECT_FALSE(r.IsOverflowed());
        EXPECT_FALSE(classRef.isDynamic);
        EXPECT_EQ(classRef.index, captured.classRef)
            << "loadout channel " << captured.channel;
        EXPECT_EQ(x, -13100.0f);
        EXPECT_EQ(y, 5239.0f);
        EXPECT_EQ(z, -520.0f);
    }
}

TEST(ActorReplication, RelocatesExactSouthAndNorthOwningOpensToResortIsland) {
    constexpr float kResortIslandX = -92368.33f;
    constexpr float kResortIslandY = 6117.922f;
    constexpr float kResortIslandZ = -491.0f;

    struct Opening {
        const char* name;
        uint32_t classRef;
        uint32_t bits;
        const char* hex;
        std::vector<ActorRepl::CapturedDynamicChannelRewrite> rewrites;
    };
    const Opening openings[] = {
        {"South pawn", 286151u, 763u,
         "8ebb0800bdce01f8eef70c0a80a043901ad00a009308409618409628409638409648409658409668"
         "e09178409688409198a090a8e091b84096c84091d8a090e83092f8409608419118a190283192384"
         "19648419158a190684196784176189003", {}},
        {"South primary", 286374u, 301u,
         "4cbd0800bdce01f8eef70ca3218cc6912b010000b03d000000c8fa010000648ec65c02000000", {}},
        {"North pawn", 286147u, 782u,
         "86bb08008d4b1d5d27011904655000041d8224805600984400b2c400b24401b2c401b24402b2c402"
         "b244038fc403b244048ac4048544058fc405b244068ac40685448791c407b244088ac40885448991"
         "c409b2440a8ac40a85440bb2c40bb2c3801c", {{146, 4, 26}}},
        {"North primary", 286271u, 301u,
         "7ebc08008d4b1d5d27010dbd20f4c291eb010000b01d000000c86a01000064f6c2dc03000000",
         {{88, 94, 209}, {106, 94, 209}, {250, 94, 209}}},
    };

    for (const Opening& opening : openings) {
        const std::vector<uint8_t> source = Hex(opening.hex);
        std::vector<uint8_t> normalized = source;
        if (!opening.rewrites.empty()) {
            ASSERT_TRUE(ActorRepl::RewriteCapturedDynamicChannelRefs(
                source.data(), source.size(), opening.bits,
                opening.rewrites, normalized)) << opening.name;
        }

        std::vector<uint8_t> relocated;
        uint32_t relocatedBits = 0;
        ASSERT_TRUE(ActorRepl::RewriteCapturedActorOpenLocation(
            normalized.data(), normalized.size(), opening.bits,
            kResortIslandX, kResortIslandY, kResortIslandZ,
            relocated, relocatedBits)) << opening.name;

        BitReader reader(relocated.data(), relocated.size(), relocatedBits);
        const NetGUIDRef classRef = ActorRepl::ReadNetGUID(reader);
        float x = 0.0f, y = 0.0f, z = 0.0f;
        ActorRepl::ReadCompressedVector(reader, x, y, z);
        EXPECT_FALSE(reader.IsOverflowed()) << opening.name;
        EXPECT_FALSE(classRef.isDynamic) << opening.name;
        EXPECT_EQ(classRef.index, opening.classRef) << opening.name;
        // UE3 compressed vectors use appRound (floor(value + 0.5)).
        EXPECT_EQ(x, -92368.0f) << opening.name;
        EXPECT_EQ(y, 6118.0f) << opening.name;
        EXPECT_EQ(z, -491.0f) << opening.name;
    }
}

TEST(ActorReplication, NormalizesExactNorthOwningGraphChannelReferences) {
    struct CapturedTemplate {
        const char* name;
        uint32_t bits;
        const char* sourceHex;
        const char* normalizedHex;
        std::vector<ActorRepl::CapturedDynamicChannelRewrite> rewrites;
    };
    const CapturedTemplate templates[] = {
        {
            "pawn", 782,
            "86bb08008d4b1d5d27011904655000041d8224805600984400b2c400b24401b2c401b24402b2c402b244038fc403b244048ac4048544058fc405b244068ac40685448791c407b244088ac40885448991c409b2440a8ac40a85440bb2c40bb2c3801c",
            "86bb08008d4b1d5d27011904655000041d82d4805600984400b2c400b24401b2c401b24402b2c402b244038fc403b244048ac4048544058fc405b244068ac40685448791c407b244088ac40885448991c409b2440a8ac40a85440bb2c40bb2c3801c",
            {{146, 4, 26}},
        },
        {
            "AK Type56", 301,
            "7ebc08008d4b1d5d27010dbd20f4c291eb010000b01d000000c86a01000064f6c2dc03000000",
            "7ebc08008d4b1d5d27010da3218cc691eb010000b01d000000c86a010000648ec6dc03000000",
            {{88, 94, 209}, {106, 94, 209}, {250, 94, 209}},
        },
        {
            "Type67", 301,
            "a8c008008d4b1d5d27010dbd20f4c2911b000000b015000000c80600000064f6c23c00000000",
            "a8c008008d4b1d5d27010da3218cc6911b000000b015000000c806000000648ec63c00000000",
            {{88, 94, 209}, {106, 94, 209}, {250, 94, 209}},
        },
        {
            "Punji", 262,
            "4cc008008d4b1d5d27010dbd20f4c2911b000000b00d000000c8ec857900000000",
            "4cc008008d4b1d5d27010da3218cc6911b000000b00d000000c81c8d7900000000",
            {{88, 94, 209}, {106, 94, 209}, {211, 94, 209}},
        },
        {
            "inventory manager", 315,
            "5e8602008d4b1d5d27014d2f482fdcfb8b0e000000f8d305000000803a01000000503f000000fe01",
            "5e8602008d4b1d5d2701cd68c868dc5b9a0e000000f8d305000000803a01000000503f000000fe01",
            {{86, 94, 209}, {102, 94, 209}, {124, 95, 210}},
        },
    };

    auto readDynamicRefAt = [](const std::vector<uint8_t>& payload,
                               uint32_t payloadBits, size_t offset) {
        BitReader reader(payload.data(), payload.size(), payloadBits);
        for (size_t bit = 0; bit < offset; ++bit) (void)reader.ReadBit();
        return ActorRepl::ReadNetGUID(reader);
    };

    for (const CapturedTemplate& captured : templates) {
        const std::vector<uint8_t> source = Hex(captured.sourceHex);
        std::vector<uint8_t> normalized;
        ASSERT_TRUE(ActorRepl::RewriteCapturedDynamicChannelRefs(
            source.data(), source.size(), captured.bits,
            captured.rewrites, normalized)) << captured.name;
        EXPECT_EQ(normalized, Hex(captured.normalizedHex)) << captured.name;
        EXPECT_EQ(normalized.size(), source.size()) << captured.name;

        for (const auto& rewrite : captured.rewrites) {
            const NetGUIDRef before =
                readDynamicRefAt(source, captured.bits, rewrite.bitOffset);
            const NetGUIDRef after =
                readDynamicRefAt(normalized, captured.bits, rewrite.bitOffset);
            EXPECT_TRUE(before.isDynamic) << captured.name;
            EXPECT_EQ(before.index, rewrite.expectedChannel) << captured.name;
            EXPECT_TRUE(after.isDynamic) << captured.name;
            EXPECT_EQ(after.index, rewrite.replacementChannel) << captured.name;
        }
    }
}

TEST(ActorReplication, CapturedChannelNormalizationFailsAtomicallyOnDrift) {
    const std::vector<uint8_t> source = Hex(
        "7ebc08008d4b1d5d27010dbd20f4c291eb010000b01d000000c86a01000064f6c2dc03000000");
    constexpr uint32_t kBits = 301;

    auto rejected = [&](const uint8_t* data, size_t bytes, uint32_t bits,
                        std::vector<ActorRepl::CapturedDynamicChannelRewrite> rewrites) {
        std::vector<uint8_t> output = {0xAA};
        EXPECT_FALSE(ActorRepl::RewriteCapturedDynamicChannelRefs(
            data, bytes, bits, rewrites, output));
        EXPECT_TRUE(output.empty());
    };

    rejected(nullptr, source.size(), kBits, {{88, 94, 209}});
    rejected(source.data(), source.size() - 1, kBits, {{88, 94, 209}});
    rejected(source.data(), source.size(), 98, {{88, 94, 209}});
    rejected(source.data(), source.size(), kBits, {{88, 95, 209}}); // old ref drift
    rejected(source.data(), source.size(), kBits, {{87, 94, 209}}); // selector drift
    rejected(source.data(), source.size(), kBits, {{88, 94, 1024}});
    rejected(source.data(), source.size(), kBits,
             {{88, 94, 209}, {88, 94, 210}}); // duplicate/overlap
    rejected(source.data(), source.size(), kBits,
             {{88, 94, 209}, {95, 47, 210}}); // partial overlap
}

TEST(ActorReplication, RejectsInvalidCapturedActorOpenLocationRewriteInputs) {
    const std::vector<uint8_t> original = Hex(
        "8ebb0800bdce01f8eef70c0a80a043901ad00a009308409618409628409638409648409658409668"
        "e09178409688409198a090a8e091b84096c84091d8a090e83092f8409608419118a190283192384"
        "19648419158a190684196784176189003");
    constexpr uint32_t kOriginalBits = 763;

    auto rejected = [&](const uint8_t* data, size_t bytes, uint32_t bits,
                        float x, float y, float z) {
        std::vector<uint8_t> output = {0xAA};
        uint32_t outputBits = 999;
        const bool ok = ActorRepl::RewriteCapturedActorOpenLocation(
            data, bytes, bits, x, y, z, output, outputBits);
        EXPECT_FALSE(ok);
        EXPECT_TRUE(output.empty());
        EXPECT_EQ(outputBits, 0u);
    };

    rejected(nullptr, original.size(), kOriginalBits, 1, 2, 3);
    rejected(original.data(), original.size(), 0, 1, 2, 3);
    rejected(original.data(), original.size() - 1, kOriginalBits, 1, 2, 3);
    rejected(original.data(), original.size(), 40, 1, 2, 3); // truncated Location
    rejected(original.data(), original.size(), kOriginalBits,
             std::numeric_limits<float>::quiet_NaN(), 2, 3);
    rejected(original.data(), original.size(), kOriginalBits,
             std::numeric_limits<float>::infinity(), 2, 3);
    rejected(original.data(), original.size(), kOriginalBits, 1048576.0f, 2, 3);

    BitWriter dynamicClassOpening;
    ActorRepl::WriteNetGUID(
        dynamicClassOpening, NetGUIDRef{/*isDynamic=*/true, 7u});
    ActorRepl::WriteCompressedVector(dynamicClassOpening, 1, 2, 3);
    dynamicClassOpening.WriteBit(true); // an otherwise valid opaque tail
    const std::vector<uint8_t> dynamicBytes = dynamicClassOpening.GetBytes();
    rejected(dynamicBytes.data(), dynamicBytes.size(),
             static_cast<uint32_t>(dynamicClassOpening.NumBits()), 4, 5, 6);
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

    EXPECT_TRUE(b.bControl);
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

    // The semantic open flag must also survive the actual wire encoder. UE3 only
    // emits bOpen when bControl is true; this round-trip guards against constructing
    // an apparent open that becomes ordinary data for a nonexistent channel.
    PacketCodec::Packet packet;
    packet.packetId = 7;
    packet.bunches.push_back(b);
    const std::vector<uint8_t> wire = PacketCodec::Encode(
        packet, PacketCodec::kServerSendMaxPacketBytes);
    const PacketCodec::Packet decoded = PacketCodec::Decode(
        wire.data(), wire.size(), PacketCodec::kServerSendMaxPacketBytes);
    ASSERT_TRUE(decoded.ok);
    ASSERT_EQ(decoded.bunches.size(), 1u);
    EXPECT_TRUE(decoded.bunches[0].bControl);
    EXPECT_TRUE(decoded.bunches[0].bOpen);
    EXPECT_EQ(decoded.bunches[0].chIndex, 2u);
}

TEST(ActorReplication, DecodesCapturedUnreliableServerMove) {
    const std::vector<uint8_t> payload = Hex(
        "41468a0108d9d01cd66f7a8f96fabae400");
    const MovementRepl::DecodeResult move =
        MovementRepl::DecodeRoPlayerControllerMoves(payload.data(), payload.size(), 129);

    ASSERT_TRUE(move.valid);
    ASSERT_TRUE(move.hasClientLocation);
    EXPECT_EQ(move.rpcCount, 1u);
    EXPECT_EQ(move.latestClientLocation.x, -9008.0f);
    EXPECT_EQ(move.latestClientLocation.y, 8108.0f);
    EXPECT_EQ(move.latestClientLocation.z, -535.0f);
}

TEST(ActorReplication, DecodesCapturedReliableServerMoveWithTrailingResetTeamSwapDelay) {
    // Live retail packetlog session_1783999149618 seq6000: a reliable h65
    // ServerMove followed by zero-param h496 ResetTeamSwapDelay in one bunch.
    const std::vector<uint8_t> payload = Hex(
        "41521c080ac909d5a4af0452f09f081f7c");
    const MovementRepl::DecodeResult move =
        MovementRepl::DecodeRoPlayerControllerMoves(payload.data(), payload.size(), 135);

    ASSERT_TRUE(move.valid);
    ASSERT_TRUE(move.hasClientLocation);
    EXPECT_EQ(move.rpcCount, 1u);
    EXPECT_EQ(move.latestClientLocation.x, -2807.0f);
    EXPECT_EQ(move.latestClientLocation.y, 7827.0f);
    EXPECT_EQ(move.latestClientLocation.z, 74.0f);
}

TEST(ActorReplication, DecodesCapturedOldMoveThenServerMove) {
    const std::vector<uint8_t> payload = Hex(
        "400e008704b5584890254422419920b6dc00c0cdce01f8caf7e8ecaf5a0f");
    const MovementRepl::DecodeResult move =
        MovementRepl::DecodeRoPlayerControllerMoves(payload.data(), payload.size(), 237);

    ASSERT_TRUE(move.valid);
    ASSERT_TRUE(move.hasClientLocation);
    EXPECT_EQ(move.rpcCount, 2u);
    EXPECT_EQ(move.latestClientLocation.x, -8980.0f);
    EXPECT_EQ(move.latestClientLocation.y, 7936.0f);
    EXPECT_EQ(move.latestClientLocation.z, -526.0f);
}

TEST(ActorReplication, DecodesCapturedDualServerMove) {
    const std::vector<uint8_t> payload = Hex(
        "3f7e8fa8086579786a0300154440a83f46a32264cb9f46c1753e0a2220d49f0ee8e03f1dd0c10f");
    const MovementRepl::DecodeResult move =
        MovementRepl::DecodeRoPlayerControllerMoves(payload.data(), payload.size(), 308);

    ASSERT_TRUE(move.valid);
    ASSERT_TRUE(move.hasClientLocation);
    EXPECT_EQ(move.rpcCount, 1u);
    EXPECT_EQ(move.latestClientLocation.x, -6158.0f);
    EXPECT_EQ(move.latestClientLocation.y, 8355.0f);
    EXPECT_EQ(move.latestClientLocation.z, -395.0f);
}

TEST(ActorReplication, DecodesMantleServerMoveLocation) {
    BitWriter w;
    w.SerializeInt(MovementRepl::kMantleServerMove,
                   MovementRepl::kRoPcMaxHandle);
    w.WriteBit(true);  w.WriteFloat(12.5f);                    // TimeStamp
    w.WriteBit(true);  ActorRepl::WriteCompressedVector(w, 4, 5, 6); // RM velocity
    w.WriteBit(true);  ActorRepl::WriteCompressedVector(w, -120, 450, 72);
    w.WriteBit(true);  w.WriteByte(3);                         // MoveFlags
    w.WriteBit(true);  w.WriteByte(4);                         // ClientRoll
    w.WriteBit(true);  w.WriteInt32(0x12345678);               // View
    w.WriteBit(false);                                         // bIsInCover

    const std::vector<uint8_t> bytes = w.GetBytes();
    const MovementRepl::DecodeResult move =
        MovementRepl::DecodeRoPlayerControllerMoves(
            bytes.data(), bytes.size(), w.NumBits());
    ASSERT_TRUE(move.valid);
    ASSERT_TRUE(move.hasClientLocation);
    EXPECT_EQ(move.rpcCount, 1u);
    EXPECT_EQ(move.latestClientLocation.x, -120.0f);
    EXPECT_EQ(move.latestClientLocation.y, 450.0f);
    EXPECT_EQ(move.latestClientLocation.z, 72.0f);
}

TEST(ActorReplication, RejectsMovementWithNonFiniteTimestamp) {
    BitWriter w;
    w.SerializeInt(MovementRepl::kServerMove,
                   MovementRepl::kRoPcMaxHandle);
    w.WriteBit(true);
    w.WriteFloat(std::numeric_limits<float>::quiet_NaN());
    w.WriteBit(false); // InAccel
    w.WriteBit(false); // ClientLoc
    w.WriteBit(false); // MoveFlags
    w.WriteBit(false); // ClientRoll
    w.WriteBit(false); // View
    w.WriteBit(false); // FreeAimRot

    const std::vector<uint8_t> bytes = w.GetBytes();
    const MovementRepl::DecodeResult move =
        MovementRepl::DecodeRoPlayerControllerMoves(
            bytes.data(), bytes.size(), w.NumBits());
    EXPECT_FALSE(move.valid);
}

TEST(ActorReplication, DecodesBoundedServerAttemptMantle) {
    BitWriter w;
    w.SerializeInt(MantleRepl::kServerAttemptMantle,
                   MantleRepl::kRoPcMaxHandle);
    w.WriteBit(false); // quick vault, not a held climb
    w.WriteBit(true);  // LastGoodMantleInfo present
    w.WriteBit(true);  // bValidLastMantle
    ActorRepl::WriteCompressedVector(w, -6667, 8171, -405);
    ActorRepl::WriteCompressedVector(w, -1, -1, 0);
    ActorRepl::WriteCompressedVector(w, -6725, 8165, -409);

    const std::vector<uint8_t> bytes = w.GetBytes();
    const MantleRepl::Attempt attempt = MantleRepl::DecodeServerAttempt(
        bytes.data(), bytes.size(), w.NumBits());
    ASSERT_TRUE(attempt.valid);
    EXPECT_FALSE(attempt.wantsToClimb);
    ASSERT_TRUE(attempt.hasLastGoodInfo);
    ASSERT_TRUE(attempt.lastGood.valid);
    EXPECT_EQ(attempt.lastGood.hitLocation.x, -6667.0f);
    EXPECT_EQ(attempt.lastGood.hitLocation.y, 8171.0f);
    EXPECT_EQ(attempt.lastGood.hitLocation.z, -405.0f);
    EXPECT_EQ(attempt.lastGood.hitNormal.x, -1.0f);
    EXPECT_EQ(attempt.lastGood.hitNormal.y, -1.0f);
    EXPECT_EQ(attempt.lastGood.playerLocation.x, -6725.0f);
}

TEST(ActorReplication, ClientStartDynamicMantleMatchesRetailCapture) {
    MantleRepl::DynamicInfo info;
    info.canDynamicMantle = true;
    info.mantleCrouched = true;
    info.startLocation = Vector3(3313, 15863, 7);
    info.endLocation = Vector3(3313, 15787, 55);
    info.normal = Vector3(0, 100, 0);
    info.height = 72.5f;

    uint32_t bits = 0;
    const std::vector<uint8_t> bytes =
        MantleRepl::EncodeClientStartDynamicMantle(1, info, bits);
    EXPECT_EQ(bits, 176u);
    EXPECT_EQ(bytes, Hex("5887773cf3be7f00ec78e66aff066880e48000009142"));
}

TEST(ActorReplication, MantleResponseEncoderRejectsUnsafeServerState) {
    MantleRepl::DynamicInfo info;
    info.startLocation = {0.0f, 0.0f, 0.0f};
    info.endLocation = {10.0f, 0.0f, 48.0f};
    info.normal = {-100.0f, 0.0f, 0.0f};
    info.height = 48.0f;

    uint32_t bits = 123u;
    info.endLocation.x = std::numeric_limits<float>::infinity();
    EXPECT_TRUE(MantleRepl::EncodeClientStartDynamicMantle(1, info, bits).empty());
    EXPECT_EQ(bits, 0u);

    info.endLocation.x = 10.0f;
    EXPECT_TRUE(MantleRepl::EncodeClientStartDynamicMantle(0, info, bits).empty());
    EXPECT_EQ(bits, 0u);
}

TEST(ActorReplication, MantleDecoderRejectsOverflowingPayloadExtent) {
    const uint8_t byte = 0;
    const MantleRepl::Attempt attempt = MantleRepl::DecodeServerAttempt(
        &byte, std::numeric_limits<size_t>::max(), 1u);
    EXPECT_FALSE(attempt.valid);
}

TEST(ActorReplication, DecodesCapturedNoInfoMantlePrefix) {
    const std::vector<uint8_t> bytes = Hex("1801");
    const MantleRepl::Attempt attempt = MantleRepl::DecodeServerAttempt(
        bytes.data(), bytes.size(), 11);
    ASSERT_TRUE(attempt.valid);
    EXPECT_FALSE(attempt.wantsToClimb);
    EXPECT_FALSE(attempt.hasLastGoodInfo);
    EXPECT_EQ(attempt.consumedBits, 11u);
}

TEST(ActorReplication, PlayerControllerWalkerDecodesMantleThenMovement) {
    BitWriter w;
    w.SerializeInt(MantleRepl::kServerAttemptMantle,
                   MantleRepl::kRoPcMaxHandle);
    w.WriteBit(false); // bWantsToClimb
    w.WriteBit(false); // no LastGoodMantleInfo
    w.SerializeInt(MovementRepl::kServerMove,
                   MovementRepl::kRoPcMaxHandle);
    w.WriteBit(true);  w.WriteFloat(42.0f);
    w.WriteBit(false); // zero acceleration
    w.WriteBit(true);  ActorRepl::WriteCompressedVector(w, -12, 34, 56);
    w.WriteBit(true);  w.WriteByte(0x30); // crouch + jump
    w.WriteBit(false); // ClientRoll
    w.WriteBit(false); // View
    w.WriteBit(false); // FreeAimRot

    const std::vector<uint8_t> bytes = w.GetBytes();
    const auto decoded = GameplayRpc::DecodePlayerController(
        bytes.data(), bytes.size(), w.NumBits());
    ASSERT_TRUE(decoded.valid);
    ASSERT_TRUE(decoded.complete);
    ASSERT_EQ(decoded.events.size(), 2u);
    EXPECT_EQ(decoded.events[0].kind, GameplayRpc::PcKind::AttemptMantle);
    EXPECT_EQ(decoded.events[0].mantle.consumedBits, 11u);
    EXPECT_EQ(decoded.events[1].kind, GameplayRpc::PcKind::Movement);
    EXPECT_EQ(decoded.events[1].movement.clientLocation.x, -12.0f);
    EXPECT_EQ(decoded.events[1].movement.moveFlags, 0x30u);
}

TEST(ActorReplication, PlayerControllerWalkerDecodesCapturedPcHousekeepingBurst) {
    // Live retail client, 2026-07-14 20:47:27: h37 ServerShortTimeout followed
    // by h89 ServerSetSpectatorLocation. The 62-bit body resolves to the owning
    // PC open location and must remain two distinct RPC events.
    const std::vector<uint8_t> bytes = Hex("25b2dc6056ff4922");
    const auto decoded = GameplayRpc::DecodePlayerController(
        bytes.data(), bytes.size(), 62);

    ASSERT_TRUE(decoded.valid);
    ASSERT_TRUE(decoded.complete);
    EXPECT_EQ(decoded.consumedBits, 62u);
    ASSERT_EQ(decoded.events.size(), 2u);
    EXPECT_EQ(decoded.events[0].kind, GameplayRpc::PcKind::ShortTimeout);
    EXPECT_EQ(decoded.events[0].handle, 37u);
    EXPECT_EQ(decoded.events[1].kind,
              GameplayRpc::PcKind::SetSpectatorLocation);
    EXPECT_EQ(decoded.events[1].handle, 89u);
    EXPECT_TRUE(decoded.events[1].spectatorLocation.hasLocation);
    EXPECT_FLOAT_EQ(decoded.events[1].spectatorLocation.location.x, -831.0f);
    EXPECT_FLOAT_EQ(decoded.events[1].spectatorLocation.location.y, 4085.0f);
    EXPECT_FLOAT_EQ(decoded.events[1].spectatorLocation.location.z, 292.0f);
}

TEST(ActorReplication, PlayerControllerWalkerDecodesCapturedLevelVisibility) {
    // Same join: exact 164-bit h104 ServerUpdateLevelVisibility body.
    const std::vector<uint8_t> bytes =
        Hex("685a000000987222fa922a9b7b93a3030000000008");
    const auto decoded = GameplayRpc::DecodePlayerController(
        bytes.data(), bytes.size(), 164);

    ASSERT_TRUE(decoded.valid);
    ASSERT_TRUE(decoded.complete);
    EXPECT_EQ(decoded.consumedBits, 164u);
    ASSERT_EQ(decoded.events.size(), 1u);
    const auto& event = decoded.events[0];
    EXPECT_EQ(event.kind, GameplayRpc::PcKind::UpdateLevelVisibility);
    EXPECT_EQ(event.handle, 104u);
    EXPECT_TRUE(event.levelVisibility.hasPackageName);
    EXPECT_FALSE(event.levelVisibility.packageName.hardcoded);
    EXPECT_EQ(event.levelVisibility.packageName.text, "SND_Resort");
    EXPECT_EQ(event.levelVisibility.packageName.number, 0);
    EXPECT_TRUE(event.levelVisibility.visible);
}

TEST(ActorReplication, PlayerControllerHousekeepingTruncationIsTransactional) {
    const std::vector<uint8_t> burst = Hex("25b2dc6056ff4922");
    const auto truncatedLocation = GameplayRpc::DecodePlayerController(
        burst.data(), burst.size(), 61);
    EXPECT_FALSE(truncatedLocation.valid);
    EXPECT_FALSE(truncatedLocation.complete);
    ASSERT_EQ(truncatedLocation.events.size(), 1u);
    EXPECT_EQ(truncatedLocation.events[0].kind,
              GameplayRpc::PcKind::ShortTimeout);
    // The failed h89 trial never advances the committed walk cursor.
    EXPECT_EQ(truncatedLocation.consumedBits, 9u);

    const std::vector<uint8_t> visibility =
        Hex("685a000000987222fa922a9b7b93a3030000000008");
    const auto truncatedVisibility = GameplayRpc::DecodePlayerController(
        visibility.data(), visibility.size(), 163);
    EXPECT_FALSE(truncatedVisibility.valid);
    EXPECT_FALSE(truncatedVisibility.complete);
    EXPECT_TRUE(truncatedVisibility.events.empty());
    EXPECT_EQ(truncatedVisibility.consumedBits, 0u);
}

TEST(ActorReplication, PlayerControllerLevelVisibilityRejectsMalformedName) {
    const auto encodeDynamic = [](const std::string& text, int32_t number) {
        BitWriter writer;
        writer.SerializeInt(104, MovementRepl::kRoPcMaxHandle);
        writer.WriteBit(true);  // PackageName present
        writer.WriteBit(false); // serialized as FString + Number
        writer.WriteString(text);
        writer.WriteInt32(number);
        writer.WriteBit(true);  // bIsVisible is a bare bit
        return writer;
    };

    const BitWriter negativeNumber = encodeDynamic("SND_Resort", -1);
    const std::vector<uint8_t> negativeBytes = negativeNumber.GetBytes();
    const auto negative = GameplayRpc::DecodePlayerController(
        negativeBytes.data(), negativeBytes.size(), negativeNumber.NumBits());
    EXPECT_FALSE(negative.valid);
    EXPECT_FALSE(negative.complete);
    EXPECT_TRUE(negative.events.empty());
    EXPECT_EQ(negative.consumedBits, 0u);

    const BitWriter longName = encodeDynamic(
        std::string(GameplayRpc::kMaxPcPackageNameCharacters + 1u, 'A'), 0);
    const std::vector<uint8_t> longBytes = longName.GetBytes();
    const auto oversized = GameplayRpc::DecodePlayerController(
        longBytes.data(), longBytes.size(), longName.NumBits());
    EXPECT_FALSE(oversized.valid);
    EXPECT_FALSE(oversized.complete);
    EXPECT_TRUE(oversized.events.empty());
    EXPECT_EQ(oversized.consumedBits, 0u);
}

TEST(ActorReplication, PlayerControllerLevelVisibilitySupportsHardcodedName) {
    BitWriter writer;
    writer.SerializeInt(104, MovementRepl::kRoPcMaxHandle);
    writer.WriteBit(true); // PackageName present
    writer.WriteBit(true); // hardcoded EName selector
    writer.SerializeInt(GameplayRpc::kMaxNetworkedHardcodedName,
                        GameplayRpc::kMaxNetworkedHardcodedName + 1u);
    writer.WriteBit(false); // bIsVisible
    const std::vector<uint8_t> bytes = writer.GetBytes();

    const auto decoded = GameplayRpc::DecodePlayerController(
        bytes.data(), bytes.size(), writer.NumBits());
    ASSERT_TRUE(decoded.complete);
    ASSERT_EQ(decoded.events.size(), 1u);
    const auto& visibility = decoded.events[0].levelVisibility;
    EXPECT_TRUE(visibility.hasPackageName);
    EXPECT_TRUE(visibility.packageName.hardcoded);
    EXPECT_EQ(visibility.packageName.hardcodedIndex,
              GameplayRpc::kMaxNetworkedHardcodedName);
    EXPECT_FALSE(visibility.visible);
}

TEST(ActorReplication, PlayerControllerWalkerRejectsOversizedPayloadExtent) {
    std::vector<uint8_t> bytes(
        GameplayRpc::kMaxRpcPayloadBits / 8u + 1u, 0u);
    const auto decoded = GameplayRpc::DecodePlayerController(
        bytes.data(), bytes.size(), GameplayRpc::kMaxRpcPayloadBits + 1u);
    EXPECT_FALSE(decoded.valid);
    EXPECT_FALSE(decoded.complete);
    EXPECT_TRUE(decoded.events.empty());
    EXPECT_EQ(decoded.consumedBits, 0u);
}

TEST(ActorReplication, OmittedMoveFlagsDecodeToLogicalDefaultZero) {
    BitWriter w;
    const auto writeMove = [&w](bool withFlags, uint8_t flags) {
        w.SerializeInt(MovementRepl::kServerMove,
                       MovementRepl::kRoPcMaxHandle);
        w.WriteBit(false); // TimeStamp
        w.WriteBit(false); // InAccel
        w.WriteBit(false); // ClientLoc
        w.WriteBit(withFlags);
        if (withFlags) w.WriteByte(flags);
        w.WriteBit(false); // ClientRoll
        w.WriteBit(false); // View
        w.WriteBit(false); // FreeAimRot
    };
    writeMove(true, 0x30);
    writeMove(false, 0);
    const std::vector<uint8_t> bytes = w.GetBytes();
    const auto decoded = GameplayRpc::DecodePlayerController(
        bytes.data(), bytes.size(), w.NumBits());
    ASSERT_TRUE(decoded.complete);
    ASSERT_EQ(decoded.events.size(), 2u);
    EXPECT_EQ(decoded.events[0].movement.moveFlags, 0x30u);
    EXPECT_FALSE(decoded.events[1].movement.hasMoveFlags);
    EXPECT_EQ(decoded.events[1].movement.moveFlags, 0u);
}

TEST(ActorReplication, WeaponWalkerDecodesCapturedStartStopConcatenation) {
    const std::vector<uint8_t> bytes = Hex("1d1e");
    const auto decoded = GameplayRpc::DecodeWeapon(
        bytes.data(), bytes.size(), 16);
    ASSERT_TRUE(decoded.valid);
    ASSERT_TRUE(decoded.complete);
    ASSERT_EQ(decoded.events.size(), 2u);
    EXPECT_EQ(decoded.events[0].kind, GameplayRpc::WeaponKind::StartFire);
    EXPECT_EQ(decoded.events[0].fireMode, 0u);
    EXPECT_EQ(decoded.events[1].kind, GameplayRpc::WeaponKind::StopFire);
    EXPECT_EQ(decoded.events[1].fireMode, 0u);
}

TEST(ActorReplication, WeaponWalkerDecodesCapturedReloadRequest) {
    const std::vector<uint8_t> bytes = Hex("26");
    const auto decoded = GameplayRpc::DecodeWeapon(
        bytes.data(), bytes.size(), 6);
    ASSERT_TRUE(decoded.valid);
    ASSERT_TRUE(decoded.complete);
    ASSERT_EQ(decoded.events.size(), 1u);
    EXPECT_EQ(decoded.events[0].kind, GameplayRpc::WeaponKind::RequestReload);
}

TEST(ActorReplication, M61WeaponWalkerUsesCookedClassHandleRange) {
    // Capture-grounded inherited M61 fire RPCs. Against field-table Max=101,
    // mode zero is exactly 8 bits and mode one carries the optional mode byte.
    const std::vector<uint8_t> modeZero = Hex("1d1e");
    const auto overhand = GameplayRpc::DecodeWeapon(
        modeZero.data(), modeZero.size(), 16,
        GameplayRpc::kM61WeaponMaxHandle);
    ASSERT_TRUE(overhand.complete);
    ASSERT_EQ(overhand.events.size(), 2u);
    EXPECT_EQ(overhand.events[0].kind, GameplayRpc::WeaponKind::StartFire);
    EXPECT_EQ(overhand.events[0].fireMode, 0u);
    EXPECT_EQ(overhand.events[1].kind, GameplayRpc::WeaponKind::StopFire);
    EXPECT_EQ(overhand.events[1].fireMode, 0u);

    const std::vector<uint8_t> modeOne = Hex("9d019e01");
    const auto toss = GameplayRpc::DecodeWeapon(
        modeOne.data(), modeOne.size(), 32,
        GameplayRpc::kM61WeaponMaxHandle);
    ASSERT_TRUE(toss.complete);
    ASSERT_EQ(toss.events.size(), 2u);
    EXPECT_EQ(toss.events[0].kind, GameplayRpc::WeaponKind::StartFire);
    EXPECT_EQ(toss.events[0].fireMode, 1u);
    EXPECT_EQ(toss.events[1].kind, GameplayRpc::WeaponKind::StopFire);
    EXPECT_EQ(toss.events[1].fireMode, 1u);
}

TEST(ActorReplication, WeaponWalkerRejectsImpossibleClassRange) {
    const std::vector<uint8_t> bytes = Hex("1d");
    const auto decoded = GameplayRpc::DecodeWeapon(
        bytes.data(), bytes.size(), 8, 38);
    EXPECT_FALSE(decoded.valid);
    EXPECT_FALSE(decoded.complete);
    EXPECT_TRUE(decoded.events.empty());
}

TEST(ActorReplication, PackedViewProducesFiniteUnitAimDirections) {
    const auto forward = GameplayRpc::DirectionFromPackedView(0);
    ASSERT_TRUE(forward.valid);
    EXPECT_NEAR(forward.value.x, 1.0f, 1.0e-5f);
    EXPECT_NEAR(forward.value.y, 0.0f, 1.0e-5f);
    EXPECT_NEAR(forward.value.z, 0.0f, 1.0e-5f);

    const auto quarterYaw = GameplayRpc::DirectionFromPackedView(0x00004000);
    ASSERT_TRUE(quarterYaw.valid);
    EXPECT_NEAR(quarterYaw.value.x, 0.0f, 1.0e-5f);
    EXPECT_NEAR(quarterYaw.value.y, 1.0f, 1.0e-5f);
    EXPECT_NEAR(quarterYaw.value.z, 0.0f, 1.0e-5f);

    const auto quarterPitch = GameplayRpc::DirectionFromPackedView(0x40000000);
    ASSERT_TRUE(quarterPitch.valid);
    EXPECT_NEAR(quarterPitch.value.x, 0.0f, 1.0e-5f);
    EXPECT_NEAR(quarterPitch.value.z, 1.0f, 1.0e-5f);
}

TEST(ActorReplication, M61ThrowParametersClampCookAndPreserveUnits) {
    const auto overhand = GameplayRpc::BuildM61ThrowParameters(
        {1.0f, 0.0f, 0.0f}, GameplayRpc::kM61OverhandFireMode, 999.0);
    ASSERT_TRUE(overhand.valid);
    EXPECT_FLOAT_EQ(overhand.cookSeconds,
                    GameplayRpc::kM61MaximumCookSeconds);
    EXPECT_NEAR(overhand.fuseSeconds,
                GameplayRpc::kM61MinimumFuseSeconds, 1.0e-5f);
    EXPECT_FLOAT_EQ(overhand.baseSpeedUuPerSecond, 1200.0f);
    EXPECT_FLOAT_EQ(overhand.launchSpeedMetersPerSecond, 24.0f);

    const auto toss = GameplayRpc::BuildM61ThrowParameters(
        {1.0f, 0.0f, 0.0f}, GameplayRpc::kM61TossFireMode, 0.0);
    ASSERT_TRUE(toss.valid);
    EXPECT_FLOAT_EQ(toss.fuseSeconds, GameplayRpc::kM61FuseSeconds);
    EXPECT_FLOAT_EQ(toss.baseSpeedUuPerSecond, 400.0f);
    EXPECT_FLOAT_EQ(toss.tossZUuPerSecond, 150.0f);
    EXPECT_GT(toss.launchDirection.z, 0.0f);
    EXPECT_NEAR(toss.launchSpeedUuPerSecond,
                std::sqrt(400.0f * 400.0f + 150.0f * 150.0f), 1.0e-4f);

    const auto fullStrength = GameplayRpc::BuildM61ThrowParameters(
        {1.0f, 0.0f, 0.0f}, GameplayRpc::kM61TossFireMode, 1.0);
    ASSERT_TRUE(fullStrength.valid);
    EXPECT_FLOAT_EQ(fullStrength.baseSpeedUuPerSecond, 600.0f);
    EXPECT_FLOAT_EQ(fullStrength.fuseSeconds, 3.2f);

    // Type67 shares the source speed/bounce envelope but owns a 4.5-second
    // fuse. Keep that variant server-side; this does not assert an ungrounded
    // projectile actor class or wire schema.
    const auto type67 = GameplayRpc::BuildType67ThrowParameters(
        {1.0f, 0.0f, 0.0f}, GameplayRpc::kM61OverhandFireMode, 1.0);
    ASSERT_TRUE(type67.valid);
    EXPECT_FLOAT_EQ(type67.cookSeconds, 1.0f);
    EXPECT_FLOAT_EQ(type67.fuseSeconds, 3.5f);
    EXPECT_FLOAT_EQ(type67.baseSpeedUuPerSecond, 1200.0f);

    const auto heldType67 = GameplayRpc::BuildType67ThrowParameters(
        {1.0f, 0.0f, 0.0f}, GameplayRpc::kM61OverhandFireMode, 999.0);
    ASSERT_TRUE(heldType67.valid);
    EXPECT_FLOAT_EQ(heldType67.cookSeconds,
                    GameplayRpc::kType67MaximumCookSeconds);
    EXPECT_NEAR(heldType67.fuseSeconds,
                GameplayRpc::kM61MinimumFuseSeconds, 1.0e-5f);

    const auto invalid = GameplayRpc::BuildM61ThrowParameters(
        {}, GameplayRpc::kM61OverhandFireMode, 0.0);
    EXPECT_FALSE(invalid.valid);
}

TEST(ActorReplication, PawnWalkerDecodesCapturedMantleStart) {
    const std::vector<uint8_t> bytes = Hex("55");
    const auto decoded = GameplayRpc::DecodePawn(
        bytes.data(), bytes.size(), 7);
    ASSERT_TRUE(decoded.valid);
    ASSERT_TRUE(decoded.complete);
    ASSERT_EQ(decoded.events.size(), 1u);
    EXPECT_EQ(decoded.events[0].kind, GameplayRpc::PawnKind::MantleStarted);
}

TEST(ActorReplication, PlayerControllerWalkerDecodesCapturedSpecialMove) {
    const std::vector<uint8_t> bytes = Hex("32870000803f7eefffff03");
    const auto decoded = GameplayRpc::DecodePlayerController(
        bytes.data(), bytes.size(), 82);
    ASSERT_TRUE(decoded.valid);
    ASSERT_TRUE(decoded.complete);
    ASSERT_EQ(decoded.events.size(), 1u);
    EXPECT_EQ(decoded.events[0].kind, GameplayRpc::PcKind::DoSpecialMove);
    EXPECT_EQ(decoded.events[0].specialMove.move, 1u);
    EXPECT_EQ(decoded.events[0].specialMove.joyUp, 1.0f);
    EXPECT_EQ(decoded.events[0].specialMove.joyRight, 0.0f);
    EXPECT_EQ(decoded.events[0].specialMove.rotYaw, -1057);
}

TEST(ActorReplication, PlayerControllerWalkerRejectsNonFiniteSpecialMoveAxis) {
    BitWriter w;
    w.SerializeInt(306, MovementRepl::kRoPcMaxHandle);
    w.WriteBit(true);
    w.SerializeInt(1, MantleRepl::kSpecialMoveMax);
    w.WriteBit(true);
    w.WriteFloat(std::numeric_limits<float>::quiet_NaN());
    w.WriteBit(false); // PlayerJoyRight
    w.WriteBit(false); // RotYaw

    const std::vector<uint8_t> bytes = w.GetBytes();
    const auto decoded = GameplayRpc::DecodePlayerController(
        bytes.data(), bytes.size(), w.NumBits());
    EXPECT_FALSE(decoded.valid);
    EXPECT_FALSE(decoded.complete);
    EXPECT_TRUE(decoded.events.empty());
}

TEST(ActorReplication, PlayerControllerWalkerRejectsSpecialMoveSentinel) {
    BitWriter w;
    w.SerializeInt(306, MovementRepl::kRoPcMaxHandle);
    w.WriteBit(true);
    w.SerializeInt(MantleRepl::kSpecialMoveMax - 1,
                   MantleRepl::kSpecialMoveMax); // SM_MAX sentinel (23)
    w.WriteBit(false); // JoyUp
    w.WriteBit(false); // JoyRight
    w.WriteBit(false); // RotYaw
    const std::vector<uint8_t> bytes = w.GetBytes();
    const auto decoded = GameplayRpc::DecodePlayerController(
        bytes.data(), bytes.size(), w.NumBits());
    EXPECT_FALSE(decoded.valid);
    EXPECT_FALSE(decoded.complete);
    EXPECT_TRUE(decoded.events.empty());
}

TEST(ActorReplication, UnknownZeroHandleAfterKnownPcRpcIsIncompleteAndTransactional) {
    BitWriter w;
    w.SerializeInt(79, MovementRepl::kRoPcMaxHandle); // ServerUse, no body
    const size_t knownBits = w.NumBits();
    w.SerializeInt(0, MovementRepl::kRoPcMaxHandle);  // unknown h0
    const std::vector<uint8_t> bytes = w.GetBytes();
    const auto decoded = GameplayRpc::DecodePlayerController(
        bytes.data(), bytes.size(), w.NumBits());
    ASSERT_TRUE(decoded.valid);
    EXPECT_FALSE(decoded.complete);
    EXPECT_TRUE(decoded.stoppedOnUnknown);
    EXPECT_EQ(decoded.unknownHandle, 0u);
    ASSERT_EQ(decoded.events.size(), 1u);
    EXPECT_EQ(decoded.consumedBits, knownBits);
}

TEST(ActorReplication, UnknownZeroHandleAfterReloadIsIncompleteAndTransactional) {
    BitWriter w;
    w.SerializeInt(38, 99); // ServerRequestReload, no body
    const size_t knownBits = w.NumBits();
    w.SerializeInt(0, 99);
    const std::vector<uint8_t> bytes = w.GetBytes();
    const auto decoded = GameplayRpc::DecodeWeapon(
        bytes.data(), bytes.size(), w.NumBits());
    ASSERT_TRUE(decoded.valid);
    EXPECT_FALSE(decoded.complete);
    EXPECT_TRUE(decoded.stoppedOnUnknown);
    EXPECT_EQ(decoded.unknownHandle, 0u);
    ASSERT_EQ(decoded.events.size(), 1u);
    EXPECT_EQ(decoded.consumedBits, knownBits);
}

TEST(ActorReplication, InventoryCurrentWeaponAbsentAndPresentUsePresenceBit) {
    BitWriter absent;
    absent.SerializeInt(25, 34);
    absent.WriteBit(false);
    EXPECT_EQ(absent.NumBits(), 6u);
    EXPECT_EQ(absent.GetBytes(), Hex("19"));
    const std::vector<uint8_t> absentBytes = absent.GetBytes();
    const auto noWeapon = GameplayRpc::DecodeInventoryManager(
        absentBytes.data(), absentBytes.size(), absent.NumBits());
    ASSERT_TRUE(noWeapon.complete);
    ASSERT_EQ(noWeapon.events.size(), 1u);
    EXPECT_FALSE(noWeapon.events[0].hasDesiredWeapon);

    BitWriter present;
    present.SerializeInt(25, 34);
    present.WriteBit(true);
    ActorRepl::WriteNetGUID(
        present, ActorRepl::NetGUIDRef{/*isDynamic=*/true, 210u});
    const std::vector<uint8_t> presentBytes = present.GetBytes();
    const auto weapon = GameplayRpc::DecodeInventoryManager(
        presentBytes.data(), presentBytes.size(), present.NumBits());
    ASSERT_TRUE(weapon.complete);
    ASSERT_EQ(weapon.events.size(), 1u);
    EXPECT_TRUE(weapon.events[0].hasDesiredWeapon);
    EXPECT_TRUE(weapon.events[0].desiredWeapon.isDynamic);
    EXPECT_EQ(weapon.events[0].desiredWeapon.index, 210u);
    EXPECT_EQ(present.NumBits(), 17u);
    EXPECT_EQ(presentBytes, Hex("796900"));
}

TEST(ActorReplication, ObjectiveArrayElementIsExact24BitTriple) {
    const std::vector<ObjectiveRepl::ArrayElement> cases = {
        {174, 15, 1}, {175, 4, 9}, {176, 3, 127},
        {177, 2, 254}, {178, 1, 80}, {179, 0, 0}
    };
    for (const auto& c : cases) {
        uint32_t bits = 0;
        const std::vector<uint8_t> encoded =
            ObjectiveRepl::EncodeArrayElements({c}, bits);
        EXPECT_EQ(bits, 24u);
        ASSERT_EQ(encoded.size(), 3u);
        EXPECT_EQ(encoded[0], static_cast<uint8_t>(c.handle));
        EXPECT_EQ(encoded[1], c.slot);
        EXPECT_EQ(encoded[2], c.value);

        BitReader r(encoded.data(), encoded.size(), bits);
        EXPECT_EQ(r.SerializeInt(ObjectiveRepl::kGriMaxHandle), c.handle);
        EXPECT_EQ(r.ReadByte(), c.slot);
        EXPECT_EQ(r.ReadByte(), c.value);
        EXPECT_FALSE(r.IsOverflowed());
        EXPECT_EQ(r.BitPos(), 24u);
    }
}

TEST(ActorReplication, GriStaticIntArrayElementUsesIndexThenInt32) {
    BitWriter w;
    ObjectiveRepl::WriteIntArrayElement(
        w, ObjectiveRepl::kSuPointsHeld, 1, 0x12345678);

    EXPECT_EQ(w.NumBits(), 48u);
    const std::vector<uint8_t> bytes = w.GetBytes();
    ASSERT_EQ(bytes.size(), 6u);
    EXPECT_EQ(bytes[0], static_cast<uint8_t>(ObjectiveRepl::kSuPointsHeld));
    EXPECT_EQ(bytes[1], 1u);

    BitReader r(bytes.data(), bytes.size(), w.NumBits());
    EXPECT_EQ(r.SerializeInt(ObjectiveRepl::kGriMaxHandle),
              ObjectiveRepl::kSuPointsHeld);
    EXPECT_EQ(r.ReadByte(), 1u);
    EXPECT_EQ(r.ReadInt32(), 0x12345678);
    EXPECT_FALSE(r.IsOverflowed());
    EXPECT_EQ(r.BitsLeft(), 0u);
}

TEST(ActorReplication, SupremacyGriSnapshotUsesRetailTeamOrderAndSignedScore) {
    BitWriter w;
    // Static team arrays use retail order: 0=NVA/North, 1=US/South.
    ObjectiveRepl::WriteIntArrayElement(
        w, ObjectiveRepl::kSuPointsHeld, 0, 1);
    ObjectiveRepl::WriteIntArrayElement(
        w, ObjectiveRepl::kSuPointsHeld, 1, 8);
    ActorRepl::WritePropInt(w, ObjectiveRepl::kSuCurrentScore,
                            ObjectiveRepl::kGriMaxHandle, -37);
    ActorRepl::WritePropInt(w, ObjectiveRepl::kSuTargetScore,
                            ObjectiveRepl::kGriMaxHandle, 500);

    const std::vector<uint8_t> bytes = w.GetBytes();
    BitReader r(bytes.data(), bytes.size(), w.NumBits());
    EXPECT_EQ(r.SerializeInt(ObjectiveRepl::kGriMaxHandle),
              ObjectiveRepl::kSuPointsHeld);
    EXPECT_EQ(r.ReadByte(), 0u);
    EXPECT_EQ(r.ReadInt32(), 1);
    EXPECT_EQ(r.SerializeInt(ObjectiveRepl::kGriMaxHandle),
              ObjectiveRepl::kSuPointsHeld);
    EXPECT_EQ(r.ReadByte(), 1u);
    EXPECT_EQ(r.ReadInt32(), 8);
    EXPECT_EQ(r.SerializeInt(ObjectiveRepl::kGriMaxHandle),
              ObjectiveRepl::kSuCurrentScore);
    EXPECT_EQ(r.ReadInt32(), -37);
    EXPECT_EQ(r.SerializeInt(ObjectiveRepl::kGriMaxHandle),
              ObjectiveRepl::kSuTargetScore);
    EXPECT_EQ(r.ReadInt32(), 500);
    EXPECT_FALSE(r.IsOverflowed());
    EXPECT_EQ(r.BitsLeft(), 0u);
}

TEST(ActorReplication, SkirmishGriSnapshotUsesRetailWireTypes) {
    BitWriter w;
    ObjectiveRepl::WriteIntArrayElement(
        w, ObjectiveRepl::kAllSpawnWindows, 0, 100);
    ObjectiveRepl::WriteIntArrayElement(
        w, ObjectiveRepl::kSpawnWindowCloseTime, 1, 200);
    ActorRepl::WritePropInt(w, ObjectiveRepl::kPlayedRoundsCount,
                            ObjectiveRepl::kGriMaxHandle, 2);
    ActorRepl::WritePropInt(w, ObjectiveRepl::kNextLockDownTime,
                            ObjectiveRepl::kGriMaxHandle, 140);
    ActorRepl::WritePropBool(w, ObjectiveRepl::kSuddenDeath,
                             ObjectiveRepl::kGriMaxHandle, true);
    ActorRepl::WritePropBool(w, ObjectiveRepl::kOverTime,
                             ObjectiveRepl::kGriMaxHandle, true);
    ActorRepl::WritePropByte(w, ObjectiveRepl::kOvertimeAdvantage,
                             ObjectiveRepl::kGriMaxHandle,
                             TeamMapping::kRetailUs);
    ObjectiveRepl::WriteArrayElement(
        w, ObjectiveRepl::kPlayersAliveCount, TeamMapping::kRetailNva, 3);
    ObjectiveRepl::WriteArrayElement(
        w, ObjectiveRepl::kPlayersAliveCount, TeamMapping::kRetailUs, 5);

    const std::vector<uint8_t> bytes = w.GetBytes();
    BitReader r(bytes.data(), bytes.size(), w.NumBits());
    EXPECT_EQ(r.SerializeInt(ObjectiveRepl::kGriMaxHandle),
              ObjectiveRepl::kAllSpawnWindows);
    EXPECT_EQ(r.ReadByte(), 0u);
    EXPECT_EQ(r.ReadInt32(), 100);
    EXPECT_EQ(r.SerializeInt(ObjectiveRepl::kGriMaxHandle),
              ObjectiveRepl::kSpawnWindowCloseTime);
    EXPECT_EQ(r.ReadByte(), 1u);
    EXPECT_EQ(r.ReadInt32(), 200);
    EXPECT_EQ(r.SerializeInt(ObjectiveRepl::kGriMaxHandle),
              ObjectiveRepl::kPlayedRoundsCount);
    EXPECT_EQ(r.ReadInt32(), 2);
    EXPECT_EQ(r.SerializeInt(ObjectiveRepl::kGriMaxHandle),
              ObjectiveRepl::kNextLockDownTime);
    EXPECT_EQ(r.ReadInt32(), 140);
    EXPECT_EQ(r.SerializeInt(ObjectiveRepl::kGriMaxHandle),
              ObjectiveRepl::kSuddenDeath);
    EXPECT_TRUE(r.ReadBit());
    EXPECT_EQ(r.SerializeInt(ObjectiveRepl::kGriMaxHandle),
              ObjectiveRepl::kOverTime);
    EXPECT_TRUE(r.ReadBit());
    EXPECT_EQ(r.SerializeInt(ObjectiveRepl::kGriMaxHandle),
              ObjectiveRepl::kOvertimeAdvantage);
    EXPECT_EQ(r.ReadByte(), TeamMapping::kRetailUs);
    EXPECT_EQ(r.SerializeInt(ObjectiveRepl::kGriMaxHandle),
              ObjectiveRepl::kPlayersAliveCount);
    EXPECT_EQ(r.ReadByte(), TeamMapping::kRetailNva);
    EXPECT_EQ(r.ReadByte(), 3u);
    EXPECT_EQ(r.SerializeInt(ObjectiveRepl::kGriMaxHandle),
              ObjectiveRepl::kPlayersAliveCount);
    EXPECT_EQ(r.ReadByte(), TeamMapping::kRetailUs);
    EXPECT_EQ(r.ReadByte(), 5u);
    EXPECT_FALSE(r.IsOverflowed());
    EXPECT_EQ(r.BitsLeft(), 0u);
}

TEST(ActorReplication, ObjectiveBaselineExplicitlyEnablesOverview) {
    BitWriter w;
    w.SerializeInt(ObjectiveRepl::kDisableObjectiveOverview,
                   ObjectiveRepl::kGriMaxHandle);
    w.WriteBit(false);
    EXPECT_EQ(w.NumBits(), 8u);
    const std::vector<uint8_t> bytes = w.GetBytes();
    EXPECT_EQ(bytes, std::vector<uint8_t>({0x64}));

    BitReader r(bytes.data(), bytes.size(), w.NumBits());
    EXPECT_EQ(r.SerializeInt(ObjectiveRepl::kGriMaxHandle),
              ObjectiveRepl::kDisableObjectiveOverview);
    EXPECT_FALSE(r.ReadBit());
    EXPECT_FALSE(r.IsOverflowed());
}

TEST(ActorReplication, ObjectiveBaselineAdvertisesRetailTerritoryTeamRoles) {
    BitWriter w;
    ActorRepl::WritePropBool(w, ObjectiveRepl::kDisableObjectiveOverview,
                             ObjectiveRepl::kGriMaxHandle, false);
    ActorRepl::WritePropBool(w, ObjectiveRepl::kAlliesAreAttacking,
                             ObjectiveRepl::kGriMaxHandle, true);
    ActorRepl::WritePropByte(w, ObjectiveRepl::kDefendingTeam,
                             ObjectiveRepl::kGriMaxHandle,
                             TeamMapping::kRetailNva);

    const std::vector<uint8_t> bytes = w.GetBytes();
    BitReader r(bytes.data(), bytes.size(), w.NumBits());
    EXPECT_EQ(r.SerializeInt(ObjectiveRepl::kGriMaxHandle),
              ObjectiveRepl::kDisableObjectiveOverview);
    EXPECT_FALSE(r.ReadBit());
    EXPECT_EQ(r.SerializeInt(ObjectiveRepl::kGriMaxHandle),
              ObjectiveRepl::kAlliesAreAttacking);
    EXPECT_TRUE(r.ReadBit());
    EXPECT_EQ(r.SerializeInt(ObjectiveRepl::kGriMaxHandle),
              ObjectiveRepl::kDefendingTeam);
    EXPECT_EQ(r.ReadByte(), TeamMapping::kRetailNva);
    EXPECT_FALSE(r.IsOverflowed());
    EXPECT_EQ(r.BitPos(), w.NumBits());
}

TEST(ActorReplication, ObjectiveBaselineTimerScalarsMatchGriWireTypes) {
    BitWriter w;
    ActorRepl::WritePropInt(w, ObjectiveRepl::kTimeLimit,
                            ObjectiveRepl::kGriMaxHandle, 600);
    ActorRepl::WritePropInt(w, ObjectiveRepl::kRemainingTime,
                            ObjectiveRepl::kGriMaxHandle, 487);
    ActorRepl::WritePropInt(w, ObjectiveRepl::kElapsedTime,
                            ObjectiveRepl::kGriMaxHandle, 113);
    ActorRepl::WritePropInt(w, ObjectiveRepl::kRemainingMinute,
                            ObjectiveRepl::kGriMaxHandle, 485);

    const std::vector<uint8_t> bytes = w.GetBytes();
    BitReader r(bytes.data(), bytes.size(), w.NumBits());
    EXPECT_EQ(r.SerializeInt(ObjectiveRepl::kGriMaxHandle),
              ObjectiveRepl::kTimeLimit);
    EXPECT_EQ(r.ReadInt32(), 600);
    EXPECT_EQ(r.SerializeInt(ObjectiveRepl::kGriMaxHandle),
              ObjectiveRepl::kRemainingTime);
    EXPECT_EQ(r.ReadInt32(), 487);
    EXPECT_EQ(r.SerializeInt(ObjectiveRepl::kGriMaxHandle),
              ObjectiveRepl::kElapsedTime);
    EXPECT_EQ(r.ReadInt32(), 113);
    EXPECT_EQ(r.SerializeInt(ObjectiveRepl::kGriMaxHandle),
              ObjectiveRepl::kRemainingMinute);
    EXPECT_EQ(r.ReadInt32(), 485);
    EXPECT_FALSE(r.IsOverflowed());
    EXPECT_EQ(r.BitPos(), w.NumBits());
}

TEST(ActorReplication, LiveGriTeamMenuScalarsUseRetailWireTypes) {
    BitWriter w;
    ActorRepl::WritePropBool(w, ObjectiveRepl::kBalanceTeams,
                             ObjectiveRepl::kGriMaxHandle, true);
    ActorRepl::WritePropByte(w, ObjectiveRepl::kMaxTeamDifference,
                             ObjectiveRepl::kGriMaxHandle, 2);
    ActorRepl::WritePropByte(w, ObjectiveRepl::kMaxPlayers,
                             ObjectiveRepl::kGriMaxHandle, 64);

    const std::vector<uint8_t> bytes = w.GetBytes();
    BitReader r(bytes.data(), bytes.size(), w.NumBits());
    EXPECT_EQ(r.SerializeInt(ObjectiveRepl::kGriMaxHandle),
              ObjectiveRepl::kBalanceTeams);
    EXPECT_TRUE(r.ReadBit());
    EXPECT_EQ(r.SerializeInt(ObjectiveRepl::kGriMaxHandle),
              ObjectiveRepl::kMaxTeamDifference);
    EXPECT_EQ(r.ReadByte(), 2u);
    EXPECT_EQ(r.SerializeInt(ObjectiveRepl::kGriMaxHandle),
              ObjectiveRepl::kMaxPlayers);
    EXPECT_EQ(r.ReadByte(), 64u);
    EXPECT_FALSE(r.IsOverflowed());
    EXPECT_EQ(r.BitsLeft(), 0u);
}

TEST(ActorReplication, ObjectiveCappersStructMatchesRetailCapture) {
    BitWriter w;
    ObjectiveRepl::WriteCappersElement(w, 2, 5, 0);
    EXPECT_EQ(w.NumBits(), 47u);
    const std::vector<uint8_t> bytes = w.GetBytes();
    EXPECT_EQ(bytes, Hex("7c8102000000"));

    BitReader r(bytes.data(), bytes.size(), w.NumBits());
    EXPECT_EQ(r.SerializeInt(ObjectiveRepl::kGriMaxHandle),
              ObjectiveRepl::kObjCappers);
    EXPECT_EQ(r.ReadByte(), 2u);
    EXPECT_EQ(r.ReadByte(), 5u);
    EXPECT_EQ(r.ReadByte(), 0u);
    EXPECT_EQ(r.ReadByte(), 0u);
    EXPECT_EQ(r.ReadByte(), 0u);
    EXPECT_FALSE(r.IsOverflowed());
    EXPECT_EQ(r.BitsLeft(), 0u);
}

TEST(ActorReplication, PlayerObjectiveEntryAndExitMatchRetailCapture) {
    BitWriter entry;
    ObjectiveRepl::WriteObjectiveIndex(entry, 3);
    ObjectiveRepl::WriteObjectiveName(entry, "Farm");
    EXPECT_EQ(entry.NumBits(), 98u);
    EXPECT_EQ(entry.GetBytes(), Hex("4b0786160000001885c9b50100"));

    BitWriter exit;
    ObjectiveRepl::WriteObjectiveName(exit, "");
    EXPECT_EQ(exit.NumBits(), 41u);
    EXPECT_EQ(exit.GetBytes(), Hex("430100000000"));
}

TEST(ActorReplication, ResortObjectiveFixtureMatchesRetailCapture) {
    std::vector<ObjectiveRepl::ArrayElement> fields;
    for (uint8_t slot = 0; slot < 5; ++slot) {
        fields.push_back({ObjectiveRepl::kRepIndices, slot, slot});
    }
    const uint8_t capturedStatus[5] = {17, 17, 80, 80, 16};
    for (uint8_t slot = 0; slot < 5; ++slot) {
        fields.push_back({ObjectiveRepl::kStatus, slot, capturedStatus[slot]});
    }
    fields.push_back({ObjectiveRepl::kForceRatio, 2, 254});

    uint32_t bits = 0;
    const std::vector<uint8_t> encoded =
        ObjectiveRepl::EncodeArrayElements(fields, bits);
    const std::vector<uint8_t> expected = {
        0xB3,0x00,0x00, 0xB3,0x01,0x01, 0xB3,0x02,0x02,
        0xB3,0x03,0x03, 0xB3,0x04,0x04,
        0xB2,0x00,0x11, 0xB2,0x01,0x11, 0xB2,0x02,0x50,
        0xB2,0x03,0x50, 0xB2,0x04,0x10,
        0xB1,0x02,0xFE
    };
    EXPECT_EQ(bits, 264u);
    EXPECT_EQ(encoded, expected);
}

TEST(ActorReplication, ObjectiveStatusAndQuantizationTruthTable) {
    EXPECT_EQ(ObjectiveRepl::RetailOwner(0), 2u);
    EXPECT_EQ(ObjectiveRepl::RetailOwner(1), 1u);
    EXPECT_EQ(ObjectiveRepl::RetailOwner(2), 0u);

    EXPECT_EQ(TeamMapping::RetailToServer(0), 2u);
    EXPECT_EQ(TeamMapping::RetailToServer(1), 1u);
    EXPECT_EQ(TeamMapping::ServerToRetail(1), 1u);
    EXPECT_EQ(TeamMapping::ServerToRetail(2), 0u);

    EXPECT_EQ(ObjectiveRepl::PackStatus(1, false, false, 0, true, false), 17u);
    EXPECT_EQ(ObjectiveRepl::PackStatus(0, false, true, 0, true, false), 80u);
    EXPECT_EQ(ObjectiveRepl::PackStatus(0, false, false, 0, true, false), 16u);
    EXPECT_EQ(ObjectiveRepl::PackStatus(2, true, true, 1, true, true), 0xFAu);

    EXPECT_EQ(ObjectiveRepl::QuantizeProgress(-1.0f), 0u);
    EXPECT_EQ(ObjectiveRepl::QuantizeProgress(0.0f), 0u);
    EXPECT_EQ(ObjectiveRepl::QuantizeProgress(0.5f), 127u);
    EXPECT_EQ(ObjectiveRepl::QuantizeProgress(1.0f), 255u);
    EXPECT_EQ(ObjectiveRepl::QuantizeProgress(2.0f), 255u);

    EXPECT_EQ(ObjectiveRepl::QuantizeForceRatio(0, 0), 0u);
    EXPECT_EQ(ObjectiveRepl::QuantizeForceRatio(1, 0), 254u);
    EXPECT_EQ(ObjectiveRepl::QuantizeForceRatio(1, 1), 127u);
    EXPECT_EQ(ObjectiveRepl::QuantizeForceRatio(0, 1), 0u);
}

RS2V_TEST_MAIN()
