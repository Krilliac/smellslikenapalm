#include "TestFramework.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Game/RoleSystem.h"
#include "Network/ActorReplication.h"
#include "Network/BitReader.h"
#include "Network/BitWriter.h"
#include "Network/RetailBootstrap.h"
#include "Network/RoleSelectionReplication.h"

namespace {

std::vector<uint8_t> Hex(const char *text) {
  std::vector<uint8_t> bytes;
  for (size_t i = 0; text[i] != '\0'; i += 2) {
    auto nibble = [](char value) -> uint8_t {
      if (value >= '0' && value <= '9')
        return value - '0';
      if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
      if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
      return 0;
    };
    bytes.push_back(
        static_cast<uint8_t>((nibble(text[i]) << 4u) | nibble(text[i + 1])));
  }
  return bytes;
}

struct EncodedRequest {
  std::vector<uint8_t> bytes;
  size_t bits = 0;
};

EncodedRequest
EncodeRequest(bool southDesired, bool rolePresent,
              ActorRepl::NetGUIDRef roleRef, bool weaponPresent = false,
              uint8_t primaryIndex = 0, uint8_t secondaryIndex = 0,
              bool tankPresent = false, ActorRepl::NetGUIDRef tankRef = {},
              bool closeMenu = false, uint32_t followingHandle = 0) {
  BitWriter writer;
  writer.SerializeInt(RoleSelectionRepl::kSelectRoleByClassHandle,
                      RoleSelectionRepl::kRoPlayerControllerMaxHandle);
  writer.WriteBit(southDesired);
  writer.WriteBit(rolePresent);
  if (rolePresent)
    ActorRepl::WriteNetGUID(writer, roleRef);
  writer.WriteBit(weaponPresent);
  if (weaponPresent) {
    writer.WriteByte(primaryIndex);
    writer.WriteByte(255);
    writer.WriteByte(255);
    writer.WriteByte(secondaryIndex);
    writer.WriteByte(0);
  }
  writer.WriteBit(tankPresent);
  if (tankPresent)
    ActorRepl::WriteNetGUID(writer, tankRef);
  writer.WriteBit(false); // bAllowTeamTank
  writer.WriteBit(false); // bDesiredContext
  writer.WriteBit(closeMenu);
  if (followingHandle != 0) {
    writer.SerializeInt(followingHandle,
                        RoleSelectionRepl::kRoPlayerControllerMaxHandle);
    if (followingHandle == RoleSelectionRepl::kServerSetReadyToSpawnHandle) {
      writer.WriteBit(true); // explicit ReadyStatus
      writer.WriteBits(2u, 2u);
    }
  }
  return {writer.GetBytes(), writer.NumBits()};
}

} // namespace

TEST(RoleSelectionReplication, ClassifiesOnlyGroundedFourByTwoProfiles) {
  using Profile = RoleSelectionRepl::GroundedRoleProfile;
  struct MapCase {
    const char *mapUrl;
    const char *modeName;
    uint32_t profileRoleRegistryObjectBase;
    Profile canonicalExpected;
    Profile installedExpected;
  };
  const MapCase maps[] = {
      {"VNTE-Resort", "Territories",
       RetailBootstrap::kCapturedRoGameObjectBase,
       Profile::CanonicalResort, Profile::Unsupported},
      {"VNTE-CuChi", "Territories",
       RetailBootstrap::kCapturedRoGameObjectBase,
       Profile::CanonicalCuChi, Profile::InstalledCuChi},
      {"VNSU-HueCity", "Supremacy", 0u, Profile::Unsupported,
       Profile::Unsupported},
      {"VNSK-Compound", "Skirmish", 0u, Profile::Unsupported,
       Profile::InstalledCompound},
  };
  struct ArtifactCase {
    std::string_view variant;
    uint32_t actualObjectBase;
    bool roleRegistryGrounded;
    bool canonical;
  };
  const ArtifactCase artifacts[] = {
      {RoleSelectionRepl::kCanonicalArtifactVariant,
       RoleSelectionRepl::kCanonicalRoGameObjectBase, true, true},
      {RoleSelectionRepl::kInstalledArtifactVariant,
       RoleSelectionRepl::kInstalledRoGameObjectBase, false, false},
  };

  for (const MapCase &map : maps) {
    for (const ArtifactCase &artifact : artifacts) {
      const Profile expected = artifact.canonical
                                   ? map.canonicalExpected
                                   : map.installedExpected;
      EXPECT_EQ(RoleSelectionRepl::ClassifyGroundedRoleProfile(
                    map.mapUrl, map.modeName,
                    map.profileRoleRegistryObjectBase, artifact.variant,
                    artifact.actualObjectBase,
                    artifact.roleRegistryGrounded),
                expected);
    }
  }
}

TEST(RoleSelectionReplication, GroundedProfileClassifierRejectsLayoutDrift) {
  using Profile = RoleSelectionRepl::GroundedRoleProfile;
  auto classifyCanonicalResort =
      [](std::string_view mapUrl, std::string_view modeName,
         uint32_t profileBase, std::string_view variant,
         uint32_t artifactBase, bool roleRegistryGrounded) {
        return RoleSelectionRepl::ClassifyGroundedRoleProfile(
            mapUrl, modeName, profileBase, variant, artifactBase,
            roleRegistryGrounded);
      };

  EXPECT_EQ(classifyCanonicalResort(
                "VNTE-Resort", "Skirmish",
                RetailBootstrap::kCapturedRoGameObjectBase, "canonical",
                RoleSelectionRepl::kCanonicalRoGameObjectBase, true),
            Profile::Unsupported);
  EXPECT_EQ(classifyCanonicalResort(
                "VNTE-Resort", "Territories",
                RetailBootstrap::kCapturedRoGameObjectBase + 1u, "canonical",
                RoleSelectionRepl::kCanonicalRoGameObjectBase, true),
            Profile::Unsupported);
  EXPECT_EQ(classifyCanonicalResort(
                "VNTE-Resort", "Territories",
                RetailBootstrap::kCapturedRoGameObjectBase, "canonical",
                RoleSelectionRepl::kCanonicalRoGameObjectBase + 1u, true),
            Profile::Unsupported);
  EXPECT_EQ(classifyCanonicalResort(
                "VNTE-Resort", "Territories",
                RetailBootstrap::kCapturedRoGameObjectBase, "canonical",
                RoleSelectionRepl::kCanonicalRoGameObjectBase, false),
            Profile::Unsupported);
  EXPECT_EQ(classifyCanonicalResort(
                "VNTE-Resort", "Territories",
                RetailBootstrap::kCapturedRoGameObjectBase, "Canonical",
                RoleSelectionRepl::kCanonicalRoGameObjectBase, true),
            Profile::Unsupported);

  // UE map/mode identity matching remains case-insensitive after an exact
  // profile has been resolved; only process-policy artifact tokens are exact.
  EXPECT_EQ(classifyCanonicalResort(
                "vnte-resort", "territories",
                RetailBootstrap::kCapturedRoGameObjectBase, "canonical",
                RoleSelectionRepl::kCanonicalRoGameObjectBase, true),
            Profile::CanonicalResort);

  EXPECT_EQ(RoleSelectionRepl::ClassifyGroundedRoleProfile(
                "VNSK-Compound", "Skirmish", 1u, "installed",
                RoleSelectionRepl::kInstalledRoGameObjectBase, false),
            Profile::Unsupported);
  EXPECT_EQ(RoleSelectionRepl::ClassifyGroundedRoleProfile(
                "VNSK-Compound", "Skirmish", 0u, "Installed",
                RoleSelectionRepl::kInstalledRoGameObjectBase, false),
            Profile::Unsupported);
  EXPECT_EQ(RoleSelectionRepl::ClassifyGroundedRoleProfile(
                "VNSK-Compound", "Skirmish", 0u, "installed",
                RoleSelectionRepl::kInstalledRoGameObjectBase + 1u, false),
            Profile::Unsupported);

  EXPECT_EQ(RoleSelectionRepl::ClassifyGroundedRoleProfile(
                "VNTE-CuChi", "Territories",
                RetailBootstrap::kCapturedRoGameObjectBase, "installed",
                RoleSelectionRepl::kInstalledRoGameObjectBase, false),
            Profile::InstalledCuChi);
  EXPECT_EQ(RoleSelectionRepl::ClassifyGroundedRoleProfile(
                "VNTE-CuChi", "Territories",
                RetailBootstrap::kCapturedRoGameObjectBase + 1u, "installed",
                RoleSelectionRepl::kInstalledRoGameObjectBase, false),
            Profile::Unsupported);
  EXPECT_EQ(RoleSelectionRepl::ClassifyGroundedRoleProfile(
                "VNTE-CuChi", "Territories",
                RetailBootstrap::kCapturedRoGameObjectBase, "Installed",
                RoleSelectionRepl::kInstalledRoGameObjectBase, false),
            Profile::Unsupported);
  EXPECT_EQ(RoleSelectionRepl::ClassifyGroundedRoleProfile(
                "VNTE-CuChi", "Territories",
                RetailBootstrap::kCapturedRoGameObjectBase, "installed",
                RoleSelectionRepl::kInstalledRoGameObjectBase + 1u, false),
            Profile::Unsupported);
}

TEST(RoleSelectionReplication, DecodesExactInterimRetailCapture) {
  // rs2_realserver_capture.pcapng frame 2326, ch2 reliable seq24. The numeric
  // class ref remains tied to the legacy capture grounding token.
  const std::vector<uint8_t> payload = Hex("af465c150008f0ff0f0000b26b0b");
  const auto decoded = RoleSelectionRepl::DecodeRoleSelectionBunch(
      payload.data(), payload.size(), 109);

  ASSERT_TRUE(decoded.valid());
  EXPECT_EQ(
      decoded.following,
      RoleSelectionRepl::FollowingRpcPattern::InterimReadyAndResetSpectate);
  EXPECT_EQ(decoded.rpc.consumedBits, 88u);
  EXPECT_TRUE(decoded.rpc.southDesired);
  EXPECT_FALSE(decoded.rpc.roleInfoClass.isDynamic);
  EXPECT_EQ(decoded.rpc.roleInfoClass.index, 87492u);
  EXPECT_TRUE(decoded.rpc.weaponSelection.presentOnWire);
  EXPECT_EQ(decoded.rpc.weaponSelection.primaryWeaponIndex, 0u);
  EXPECT_EQ(decoded.rpc.weaponSelection.primaryWeaponLevel, 255u);
  EXPECT_EQ(decoded.rpc.weaponSelection.primaryWeaponAmmo, 255u);
  EXPECT_EQ(decoded.rpc.weaponSelection.secondaryWeaponIndex, 0u);
  EXPECT_EQ(decoded.rpc.weaponSelection.secondaryWeaponLevel, 0u);
  EXPECT_FALSE(decoded.rpc.tankSelectionPresent);
  EXPECT_FALSE(decoded.rpc.closeMenu);
}

TEST(RoleSelectionReplication, DecodesExactNorthInterimRetailCapture) {
  // Resort North frame 61948. h175 consumes 88 bits, then h434(status=2)
  // ends the 100-bit bunch; unlike the South fixture, there is no h182 tail.
  const std::vector<uint8_t> payload = Hex("af4456150008f0ff0f0000b20b");
  const auto decoded = RoleSelectionRepl::DecodeRoleSelectionBunch(
      payload.data(), payload.size(), 100);

  ASSERT_TRUE(decoded.valid());
  EXPECT_EQ(decoded.following,
            RoleSelectionRepl::FollowingRpcPattern::InterimReadyOnly);
  EXPECT_EQ(decoded.rpc.consumedBits, 88u);
  EXPECT_FALSE(decoded.rpc.southDesired);
  EXPECT_FALSE(decoded.rpc.roleInfoClass.isDynamic);
  EXPECT_EQ(decoded.rpc.roleInfoClass.index,
            RoleSelectionRepl::kResortNorthRiflemanRoleInfoObjectRef);
  EXPECT_TRUE(decoded.rpc.weaponSelection.presentOnWire);
  EXPECT_EQ(decoded.rpc.weaponSelection.primaryWeaponIndex, 0u);
  EXPECT_EQ(decoded.rpc.weaponSelection.primaryWeaponLevel, 255u);
  EXPECT_EQ(decoded.rpc.weaponSelection.primaryWeaponAmmo, 255u);
  EXPECT_EQ(decoded.rpc.weaponSelection.secondaryWeaponIndex, 0u);
  EXPECT_EQ(decoded.rpc.weaponSelection.secondaryWeaponLevel, 0u);
  EXPECT_FALSE(decoded.rpc.tankSelectionPresent);
  EXPECT_FALSE(decoded.rpc.closeMenu);
}

TEST(RoleSelectionReplication, DecodesExactFinalRetailCapture) {
  // rs2_realserver_capture.pcapng frame 2533, ch2 reliable seq31.
  const std::vector<uint8_t> payload = Hex("af465c150080c301");
  const auto decoded = RoleSelectionRepl::DecodeRoleSelectionBunch(
      payload.data(), payload.size(), 57);

  ASSERT_TRUE(decoded.valid());
  EXPECT_EQ(decoded.following,
            RoleSelectionRepl::FollowingRpcPattern::FinalAutoSelectSquad);
  EXPECT_EQ(decoded.rpc.consumedBits, 48u);
  EXPECT_TRUE(decoded.rpc.southDesired);
  EXPECT_EQ(decoded.rpc.roleInfoClass.index, 87492u);
  EXPECT_FALSE(decoded.rpc.weaponSelection.presentOnWire);
  EXPECT_TRUE(decoded.rpc.closeMenu);
}

TEST(RoleSelectionReplication, DecodesExactNorthFinalRetailCapture) {
  // Resort North frame 61985. Frame 61989 supplies the corresponding exact
  // compound h210+h211 server transition tested below.
  const std::vector<uint8_t> payload = Hex("af4456150080c301");
  const auto decoded = RoleSelectionRepl::DecodeRoleSelectionBunch(
      payload.data(), payload.size(), 57);

  ASSERT_TRUE(decoded.valid());
  EXPECT_EQ(decoded.following,
            RoleSelectionRepl::FollowingRpcPattern::FinalAutoSelectSquad);
  EXPECT_EQ(decoded.rpc.consumedBits, 48u);
  EXPECT_FALSE(decoded.rpc.southDesired);
  EXPECT_EQ(decoded.rpc.roleInfoClass.index,
            RoleSelectionRepl::kResortNorthRiflemanRoleInfoObjectRef);
  EXPECT_FALSE(decoded.rpc.weaponSelection.presentOnWire);
  EXPECT_TRUE(decoded.rpc.closeMenu);
}

TEST(RoleSelectionReplication, DecodesSourceGroundedCuChiFinalRequests) {
  struct CuChiCase {
    const char *hex;
    bool south;
    uint32_t roleObjectRef;
  };
  const CuChiCase cases[] = {
      {"af265c150080c301", true,
       RoleSelectionRepl::kCuChiSouthGruntRoleInfoObjectRef},
      {"af6456150080c301", false,
       RoleSelectionRepl::kCuChiNorthGuerillaRoleInfoObjectRef},
  };

  for (const CuChiCase &entry : cases) {
    const std::vector<uint8_t> payload = Hex(entry.hex);
    const auto decoded = RoleSelectionRepl::DecodeRoleSelectionBunch(
        payload.data(), payload.size(), 57u);

    ASSERT_TRUE(decoded.valid());
    EXPECT_EQ(decoded.following,
              RoleSelectionRepl::FollowingRpcPattern::FinalAutoSelectSquad);
    EXPECT_EQ(decoded.rpc.consumedBits, 48u);
    EXPECT_EQ(decoded.rpc.southDesired, entry.south);
    EXPECT_FALSE(decoded.rpc.roleInfoClass.isDynamic);
    EXPECT_EQ(decoded.rpc.roleInfoClass.index, entry.roleObjectRef);
    EXPECT_FALSE(decoded.rpc.weaponSelection.presentOnWire);
    EXPECT_TRUE(decoded.rpc.closeMenu);
  }
}

TEST(RoleSelectionReplication,
     DecodesAndGroundsSourceExactInstalledCuChiFinalRequests) {
  struct InstalledCuChiCase {
    const char *hex;
    bool south;
    uint32_t serverTeam;
    uint32_t roleObjectRef;
  };
  const InstalledCuChiCase cases[] = {
      {"af365c150080c301", true, RoleSelectionRepl::kCuChiUsServerTeam,
       RoleSelectionRepl::kInstalledCuChiSouthGruntRoleInfoObjectRef},
      {"af7456150080c301", false, RoleSelectionRepl::kCuChiNlfServerTeam,
       RoleSelectionRepl::kInstalledCuChiNorthGuerillaRoleInfoObjectRef},
  };

  for (const InstalledCuChiCase &entry : cases) {
    const std::vector<uint8_t> payload = Hex(entry.hex);
    const auto decoded = RoleSelectionRepl::DecodeRoleSelectionBunch(
        payload.data(), payload.size(), 57u);

    ASSERT_TRUE(decoded.valid());
    EXPECT_EQ(decoded.following,
              RoleSelectionRepl::FollowingRpcPattern::FinalAutoSelectSquad);
    EXPECT_EQ(decoded.rpc.consumedBits, 48u);
    EXPECT_EQ(decoded.rpc.southDesired, entry.south);
    EXPECT_FALSE(decoded.rpc.roleInfoClass.isDynamic);
    EXPECT_EQ(decoded.rpc.roleInfoClass.index, entry.roleObjectRef);
    EXPECT_FALSE(decoded.rpc.weaponSelection.presentOnWire);
    EXPECT_TRUE(decoded.rpc.closeMenu);

    const auto grounded = RoleSelectionRepl::ResolveGroundedCuChiInfantry(
        decoded.rpc, "VNTE-CuChi", "Territories",
        RetailBootstrap::kCapturedRoGameObjectBase,
        RoleSelectionRepl::kInstalledArtifactVariant,
        RoleSelectionRepl::kInstalledRoGameObjectBase, entry.serverTeam);
    ASSERT_TRUE(grounded.valid());
    EXPECT_EQ(grounded.role.roleInfoObjectRef, entry.roleObjectRef);
    EXPECT_EQ(grounded.role.classIndex,
              RoleSelectionRepl::kCuChiInfantryClassIndex);
  }
}

TEST(RoleSelectionReplication, DecodesExactInstalledCompoundLivePayloads) {
  struct LiveCase {
    const char *hex;
    size_t bits;
    bool south;
    uint32_t classRef;
    bool weaponPresent;
    uint8_t primaryIndex;
    uint8_t primaryLevel;
    uint8_t primaryAmmo;
    bool closeMenu;
    size_t consumedBits;
    RoleSelectionRepl::FollowingRpcPattern following;
  };

  const LiveCase cases[] = {
      {"af565c150008f0ff0f0000b20b", 100, true, 87493, true, 0, 255,
       255, false, 88,
       RoleSelectionRepl::FollowingRpcPattern::InterimReadyOnly},
      {"af565c150080c3b300", 67, true, 87493, false, 0, 0, 0, true,
       48, RoleSelectionRepl::FollowingRpcPattern::
               FinalAutoSelectSquadAndDefaultSpectatorLocation},
      {"af565c150080c301", 57, true, 87493, false, 0, 0, 0, true, 48,
       RoleSelectionRepl::FollowingRpcPattern::FinalAutoSelectSquad},
      {"af565f150018f0ff0f0000", 88, true, 87541, true, 1, 255, 255,
       false, 88, RoleSelectionRepl::FollowingRpcPattern::None},
      {"af565f1500180000000080c3b300", 107, true, 87541, true, 1, 0,
       0, true, 88,
       RoleSelectionRepl::FollowingRpcPattern::
           FinalAutoSelectSquadAndDefaultSpectatorLocation},
      {"af565f1500180000000080c301", 97, true, 87541, true, 1, 0, 0,
       true, 88,
       RoleSelectionRepl::FollowingRpcPattern::FinalAutoSelectSquad},
      {"af94561500180000000080c3b300", 107, false, 87401, true, 1, 0,
       0, true, 88,
       RoleSelectionRepl::FollowingRpcPattern::
           FinalAutoSelectSquadAndDefaultSpectatorLocation},
  };

  for (const LiveCase &live : cases) {
    const std::vector<uint8_t> payload = Hex(live.hex);
    const auto decoded = RoleSelectionRepl::DecodeRoleSelectionBunch(
        payload.data(), payload.size(), live.bits);
    ASSERT_TRUE(decoded.valid());
    EXPECT_EQ(decoded.following, live.following);
    EXPECT_EQ(decoded.rpc.consumedBits, live.consumedBits);
    EXPECT_EQ(decoded.rpc.southDesired, live.south);
    EXPECT_FALSE(decoded.rpc.roleInfoClass.isDynamic);
    EXPECT_EQ(decoded.rpc.roleInfoClass.index, live.classRef);
    EXPECT_EQ(decoded.rpc.weaponSelection.presentOnWire, live.weaponPresent);
    EXPECT_EQ(decoded.rpc.weaponSelection.primaryWeaponIndex,
              live.primaryIndex);
    EXPECT_EQ(decoded.rpc.weaponSelection.primaryWeaponLevel,
              live.primaryLevel);
    EXPECT_EQ(decoded.rpc.weaponSelection.primaryWeaponAmmo,
              live.primaryAmmo);
    EXPECT_EQ(decoded.rpc.closeMenu, live.closeMenu);

    const auto grounded = RoleSelectionRepl::ResolveGroundedCompoundRole(
        decoded.rpc, "VNSK-Compound", "Skirmish", "installed",
        RoleSelectionRepl::kInstalledRoGameObjectBase,
        live.south ? RoleSelectionRepl::kCompoundUsServerTeam
                   : RoleSelectionRepl::kCompoundNlfServerTeam);
    ASSERT_TRUE(grounded.valid());
  }
}

TEST(RoleSelectionReplication, CompoundHousekeepingTailIsExactAndBounded) {
  const std::vector<uint8_t> exact = Hex("af565c150080c3b300");

  // The final bit is h89's optional Vector presence flag. A present value has
  // no captured compressed-vector payload in this tranche and stays closed.
  std::vector<uint8_t> presentVector = exact;
  presentVector.back() |= 0x04u;
  EXPECT_EQ(RoleSelectionRepl::DecodeRoleSelectionBunch(
                presentVector.data(), presentVector.size(), 67)
                .error,
            RoleSelectionRepl::DecodeError::UnsupportedFollowingRpc);

  // Even a zero bit after the exact h89(false) form is trailing data rather
  // than padding: payloadBits is authoritative for a UE bunch.
  EXPECT_EQ(RoleSelectionRepl::DecodeRoleSelectionBunch(
                exact.data(), exact.size(), 68)
                .error,
            RoleSelectionRepl::DecodeError::TrailingBits);

  // Starting another handle without enough bits is a structural truncation.
  const std::vector<uint8_t> h451Only = Hex("af565c150080c301");
  EXPECT_EQ(RoleSelectionRepl::DecodeRoleSelectionBunch(
                h451Only.data(), h451Only.size(), 58)
                .error,
            RoleSelectionRepl::DecodeError::Truncated);
}

TEST(RoleSelectionReplication, DecodeOneIsTransactionalForEveryTruncation) {
  const std::vector<uint8_t> payload = Hex("af465c150080c301");
  for (size_t validBits = 0; validBits < 48; ++validBits) {
    BitReader reader(payload.data(), payload.size(), validBits);
    RoleSelectionRepl::SelectRoleByClass output;
    output.southDesired = true;
    output.roleInfoClass.index = 123456;
    output.consumedBits = 999;
    RoleSelectionRepl::DecodeError error = RoleSelectionRepl::DecodeError::None;

    EXPECT_FALSE(RoleSelectionRepl::DecodeOne(reader, output, error));
    EXPECT_EQ(reader.BitPos(), 0u);
    EXPECT_TRUE(output.southDesired);
    EXPECT_EQ(output.roleInfoClass.index, 123456u);
    EXPECT_EQ(output.consumedBits, 999u);
    EXPECT_NE(error, RoleSelectionRepl::DecodeError::None);
  }

  const std::vector<uint8_t> weaponPayload =
      Hex("af565f150018f0ff0f0000");
  for (size_t validBits = 0; validBits < 88; ++validBits) {
    BitReader reader(weaponPayload.data(), weaponPayload.size(), validBits);
    RoleSelectionRepl::SelectRoleByClass output;
    output.southDesired = false;
    output.roleInfoClass.index = 654321;
    output.consumedBits = 777;
    RoleSelectionRepl::DecodeError error = RoleSelectionRepl::DecodeError::None;

    EXPECT_FALSE(RoleSelectionRepl::DecodeOne(reader, output, error));
    EXPECT_EQ(reader.BitPos(), 0u);
    EXPECT_FALSE(output.southDesired);
    EXPECT_EQ(output.roleInfoClass.index, 654321u);
    EXPECT_EQ(output.consumedBits, 777u);
    EXPECT_NE(error, RoleSelectionRepl::DecodeError::None);
  }
}

TEST(RoleSelectionReplication,
     DecoderRejectsMissingDynamicZeroAndTankButAcceptsStaticRoleRefs) {
  const ActorRepl::NetGUIDRef grounded{false, 87492};

  auto missing = EncodeRequest(true, false, grounded);
  auto result = RoleSelectionRepl::DecodeRoleSelectionBunch(
      missing.bytes.data(), missing.bytes.size(), missing.bits);
  EXPECT_EQ(result.error, RoleSelectionRepl::DecodeError::MissingRoleInfoClass);

  auto dynamic = EncodeRequest(true, true, ActorRepl::NetGUIDRef{true, 26});
  result = RoleSelectionRepl::DecodeRoleSelectionBunch(
      dynamic.bytes.data(), dynamic.bytes.size(), dynamic.bits);
  EXPECT_EQ(result.error, RoleSelectionRepl::DecodeError::DynamicRoleInfoClass);

  auto syntactic =
      EncodeRequest(true, true, ActorRepl::NetGUIDRef{false, 123456});
  result = RoleSelectionRepl::DecodeRoleSelectionBunch(
      syntactic.bytes.data(), syntactic.bytes.size(), syntactic.bits);
  ASSERT_TRUE(result.valid());
  EXPECT_EQ(result.rpc.roleInfoClass.index, 123456u);

  auto zero = EncodeRequest(true, true, ActorRepl::NetGUIDRef{false, 0});
  result = RoleSelectionRepl::DecodeRoleSelectionBunch(
      zero.bytes.data(), zero.bytes.size(), zero.bits);
  EXPECT_EQ(result.error, RoleSelectionRepl::DecodeError::ZeroRoleInfoClass);

  BitReader zeroReader(zero.bytes.data(), zero.bytes.size(), zero.bits);
  RoleSelectionRepl::SelectRoleByClass unchanged;
  unchanged.roleInfoClass.index = 42;
  RoleSelectionRepl::DecodeError zeroError =
      RoleSelectionRepl::DecodeError::None;
  EXPECT_FALSE(
      RoleSelectionRepl::DecodeOne(zeroReader, unchanged, zeroError));
  EXPECT_EQ(zeroError, RoleSelectionRepl::DecodeError::ZeroRoleInfoClass);
  EXPECT_EQ(zeroReader.BitPos(), 0u);
  EXPECT_EQ(unchanged.roleInfoClass.index, 42u);

  auto tank = EncodeRequest(true, true, grounded, false, 0, 0, true,
                            ActorRepl::NetGUIDRef{false, 60245});
  result = RoleSelectionRepl::DecodeRoleSelectionBunch(
      tank.bytes.data(), tank.bytes.size(), tank.bits);
  EXPECT_EQ(result.error,
            RoleSelectionRepl::DecodeError::UnsupportedTankSelection);
}

TEST(RoleSelectionReplication,
     ResortSouthGroundingUsesCapturedClassRefAndTransition) {
  auto encoded = EncodeRequest(true, true, ActorRepl::NetGUIDRef{false, 87492},
                               true, 0, 0);
  const auto decoded = RoleSelectionRepl::DecodeRoleSelectionBunch(
      encoded.bytes.data(), encoded.bytes.size(), encoded.bits);
  ASSERT_TRUE(decoded.valid());

  auto grounded = RoleSelectionRepl::ResolveGroundedResortInfantry(
      decoded.rpc, "VNTE-Resort", 1);
  ASSERT_TRUE(grounded.valid());
  EXPECT_EQ(grounded.role.roleInfoObjectRef,
            RoleSelectionRepl::kResortSouthGruntRoleInfoObjectRef);
  EXPECT_EQ(grounded.role.classIndex, 0u);
  ASSERT_TRUE(grounded.role.changedRole.has_value());
  EXPECT_EQ(grounded.role.changedRole->squadIndex, 255u);
  EXPECT_EQ(grounded.role.changedRole->classIndex, 0u);
  EXPECT_FALSE(grounded.role.changedRole->showLobby);
  EXPECT_TRUE(grounded.role.changedRole->showSpawnSelect);
  EXPECT_EQ(grounded.role.squadIndex, 8u);
  EXPECT_EQ(grounded.role.roleIndex, 5u);

  EXPECT_EQ(RoleSelectionRepl::ResolveGroundedResortInfantry(decoded.rpc,
                                                             "VNTE-CuChi", 1)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedMap);
  EXPECT_EQ(RoleSelectionRepl::ResolveGroundedResortInfantry(decoded.rpc,
                                                             "VNTE-Resort", 3)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedTeam);
  EXPECT_EQ(RoleSelectionRepl::ResolveGroundedResortInfantry(
                decoded.rpc, "VNTE-Resort", 257u)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedTeam);

  auto wrongIntent = decoded.rpc;
  wrongIntent.southDesired = false;
  EXPECT_EQ(RoleSelectionRepl::ResolveGroundedResortInfantry(wrongIntent,
                                                             "VNTE-Resort", 1)
                .error,
            RoleSelectionRepl::GroundingError::TeamIntentMismatch);

  auto wrongWeapon = decoded.rpc;
  wrongWeapon.weaponSelection.primaryWeaponIndex = 1;
  EXPECT_EQ(RoleSelectionRepl::ResolveGroundedResortInfantry(wrongWeapon,
                                                             "VNTE-Resort", 1)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedWeaponSelection);

  auto wrongRole = decoded.rpc;
  wrongRole.roleInfoClass.index = 87493;
  EXPECT_EQ(RoleSelectionRepl::ResolveGroundedResortInfantry(wrongRole,
                                                             "VNTE-Resort", 1)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedRoleInfoObject);
}

TEST(RoleSelectionReplication,
     ResortNorthGroundingUsesCapturedCompoundTransition) {
  const std::vector<uint8_t> payload = Hex("af4456150008f0ff0f0000b20b");
  const auto decoded = RoleSelectionRepl::DecodeRoleSelectionBunch(
      payload.data(), payload.size(), 100);
  ASSERT_TRUE(decoded.valid());

  auto grounded = RoleSelectionRepl::ResolveGroundedResortInfantry(
      decoded.rpc, "vnte-resort", 2);
  ASSERT_TRUE(grounded.valid());
  EXPECT_EQ(grounded.role.roleInfoObjectRef,
            RoleSelectionRepl::kResortNorthRiflemanRoleInfoObjectRef);
  EXPECT_EQ(grounded.role.classIndex, 0u);
  ASSERT_TRUE(grounded.role.changedRole.has_value());
  EXPECT_EQ(grounded.role.changedRole->squadIndex, 255u);
  EXPECT_EQ(grounded.role.changedRole->classIndex, 0u);
  EXPECT_FALSE(grounded.role.changedRole->showLobby);
  EXPECT_TRUE(grounded.role.changedRole->showSpawnSelect);
  ASSERT_TRUE(grounded.role.changedRole->followingChangedSquad.has_value());
  EXPECT_EQ(grounded.role.changedRole->followingChangedSquad->squadIndex, 2u);
  EXPECT_EQ(grounded.role.changedRole->followingChangedSquad->roleIndex, 3u);
  EXPECT_EQ(grounded.role.squadIndex, 2u);
  EXPECT_EQ(grounded.role.roleIndex, 3u);

  auto wrongIntent = decoded.rpc;
  wrongIntent.southDesired = true;
  EXPECT_EQ(RoleSelectionRepl::ResolveGroundedResortInfantry(wrongIntent,
                                                             "VNTE-Resort", 2)
                .error,
            RoleSelectionRepl::GroundingError::TeamIntentMismatch);

  auto southObject = decoded.rpc;
  southObject.roleInfoClass.index =
      RoleSelectionRepl::kResortSouthGruntRoleInfoObjectRef;
  EXPECT_EQ(RoleSelectionRepl::ResolveGroundedResortInfantry(southObject,
                                                             "VNTE-Resort", 2)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedRoleInfoObject);

  auto ungroundedWeapon = decoded.rpc;
  ungroundedWeapon.weaponSelection.primaryWeaponLevel = 254;
  EXPECT_EQ(RoleSelectionRepl::ResolveGroundedResortInfantry(ungroundedWeapon,
                                                             "VNTE-Resort", 2)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedWeaponSelection);

  auto unsupportedFlags = decoded.rpc;
  unsupportedFlags.desiredContext = true;
  EXPECT_EQ(RoleSelectionRepl::ResolveGroundedResortInfantry(unsupportedFlags,
                                                             "VNTE-Resort", 2)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedSelectionFlags);
}

TEST(RoleSelectionReplication,
     DecoderIsSyntacticWhileCuChiGroundingAuthorizesExactRoleClasses) {
  for (const auto role : {
           std::pair<bool, uint32_t>{
               true, RoleSelectionRepl::kCuChiSouthGruntRoleInfoObjectRef},
           std::pair<bool, uint32_t>{
               false, RoleSelectionRepl::kCuChiNorthGuerillaRoleInfoObjectRef},
       }) {
    const auto encoded = EncodeRequest(
        role.first, true, ActorRepl::NetGUIDRef{false, role.second},
        true, 0, 0);
    const auto decoded = RoleSelectionRepl::DecodeRoleSelectionBunch(
        encoded.bytes.data(), encoded.bytes.size(), encoded.bits);
    ASSERT_TRUE(decoded.valid());
    EXPECT_EQ(decoded.rpc.roleInfoClass.index, role.second);
    EXPECT_EQ(decoded.rpc.southDesired, role.first);
  }

  const auto adjacent = EncodeRequest(
      true, true, ActorRepl::NetGUIDRef{false, 87491}, true, 0, 0);
  const auto adjacentDecoded = RoleSelectionRepl::DecodeRoleSelectionBunch(
      adjacent.bytes.data(), adjacent.bytes.size(), adjacent.bits);
  ASSERT_TRUE(adjacentDecoded.valid());
  EXPECT_EQ(RoleSelectionRepl::ResolveGroundedCuChiInfantry(
                adjacentDecoded.rpc, "VNTE-CuChi", "Territories",
                RetailBootstrap::kCapturedRoGameObjectBase,
                RoleSelectionRepl::kCanonicalArtifactVariant,
                RoleSelectionRepl::kCanonicalRoGameObjectBase,
                RoleSelectionRepl::kCuChiUsServerTeam)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedRoleInfoObject);
}

TEST(RoleSelectionReplication, CuChiDecoderAcceptsStructurallyCompleteTails) {
  const ActorRepl::NetGUIDRef south{
      false, RoleSelectionRepl::kCuChiSouthGruntRoleInfoObjectRef};

  const auto lone = EncodeRequest(true, true, south, true, 0, 0);
  auto decoded = RoleSelectionRepl::DecodeRoleSelectionBunch(
      lone.bytes.data(), lone.bytes.size(), lone.bits);
  ASSERT_TRUE(decoded.valid());
  EXPECT_EQ(decoded.following, RoleSelectionRepl::FollowingRpcPattern::None);

  const auto final = EncodeRequest(
      true, true, south, false, 0, 0, false, {}, true,
      RoleSelectionRepl::kServerAutoSelectSquadHandle);
  decoded = RoleSelectionRepl::DecodeRoleSelectionBunch(
      final.bytes.data(), final.bytes.size(), final.bits);
  ASSERT_TRUE(decoded.valid());
  EXPECT_EQ(decoded.following,
            RoleSelectionRepl::FollowingRpcPattern::FinalAutoSelectSquad);

  const auto inferredInterim = EncodeRequest(
      true, true, south, true, 0, 0, false, {}, false,
      RoleSelectionRepl::kServerSetReadyToSpawnHandle);
  decoded = RoleSelectionRepl::DecodeRoleSelectionBunch(
      inferredInterim.bytes.data(), inferredInterim.bytes.size(),
      inferredInterim.bits);
  ASSERT_TRUE(decoded.valid());
  EXPECT_EQ(decoded.following,
            RoleSelectionRepl::FollowingRpcPattern::InterimReadyOnly);
}

TEST(RoleSelectionReplication,
     CuChiGroundingAcceptsBothExactTeamsWithoutSquadSnapshot) {
  RoleSelectionRepl::SelectRoleByClass south;
  south.southDesired = true;
  south.roleInfoClass = {
      false, RoleSelectionRepl::kCuChiSouthGruntRoleInfoObjectRef};
  south.weaponSelection.presentOnWire = true;
  south.weaponSelection.primaryWeaponLevel = 255;
  south.weaponSelection.primaryWeaponAmmo = 255;

  const auto groundedSouth =
      RoleSelectionRepl::ResolveGroundedCuChiInfantry(
          south, "VNTE-CuChi", "Territories",
          RetailBootstrap::kCapturedRoGameObjectBase,
          RoleSelectionRepl::kCanonicalArtifactVariant,
          RoleSelectionRepl::kCanonicalRoGameObjectBase,
          RoleSelectionRepl::kCuChiUsServerTeam);
  ASSERT_TRUE(groundedSouth.valid());
  EXPECT_EQ(groundedSouth.role.roleInfoObjectRef,
            RoleSelectionRepl::kCuChiSouthGruntRoleInfoObjectRef);
  EXPECT_EQ(groundedSouth.role.classIndex, 0u);
  EXPECT_FALSE(groundedSouth.role.changedRole.has_value());
  EXPECT_EQ(groundedSouth.role.squadIndex, 255u);
  EXPECT_EQ(groundedSouth.role.roleIndex, 255u);

  RoleSelectionRepl::SelectRoleByClass north = south;
  north.southDesired = false;
  north.roleInfoClass.index =
      RoleSelectionRepl::kCuChiNorthGuerillaRoleInfoObjectRef;
  const auto groundedNorth =
      RoleSelectionRepl::ResolveGroundedCuChiInfantry(
          north, "vnte-cuchi", "territories",
          RetailBootstrap::kCapturedRoGameObjectBase,
          RoleSelectionRepl::kCanonicalArtifactVariant,
          RoleSelectionRepl::kCanonicalRoGameObjectBase,
          RoleSelectionRepl::kCuChiNlfServerTeam);
  ASSERT_TRUE(groundedNorth.valid());
  EXPECT_EQ(groundedNorth.role.roleInfoObjectRef,
            RoleSelectionRepl::kCuChiNorthGuerillaRoleInfoObjectRef);
  EXPECT_EQ(groundedNorth.role.classIndex, 0u);
  EXPECT_FALSE(groundedNorth.role.changedRole.has_value());
  EXPECT_EQ(groundedNorth.role.squadIndex, 255u);
  EXPECT_EQ(groundedNorth.role.roleIndex, 255u);
}

TEST(RoleSelectionReplication,
     CuChiGroundingRejectsCrossArtifactRoleRefsAndLayoutDrift) {
  auto requestFor = [](bool south, uint32_t roleObjectRef) {
    RoleSelectionRepl::SelectRoleByClass rpc;
    rpc.southDesired = south;
    rpc.roleInfoClass = {false, roleObjectRef};
    return rpc;
  };
  auto resolve = [&](const RoleSelectionRepl::SelectRoleByClass& rpc,
                     std::string_view artifactVariant,
                     uint32_t profileObjectBase,
                     uint32_t artifactObjectBase,
                     uint32_t serverTeam) {
    return RoleSelectionRepl::ResolveGroundedCuChiInfantry(
        rpc, "VNTE-CuChi", "Territories", profileObjectBase,
        artifactVariant, artifactObjectBase, serverTeam);
  };

  struct ArtifactRoleCase {
    bool south;
    uint32_t serverTeam;
    uint32_t canonicalRoleObjectRef;
    uint32_t installedRoleObjectRef;
  };
  const ArtifactRoleCase cases[] = {
      {true, RoleSelectionRepl::kCuChiUsServerTeam,
       RoleSelectionRepl::kCuChiSouthGruntRoleInfoObjectRef,
       RoleSelectionRepl::kInstalledCuChiSouthGruntRoleInfoObjectRef},
      {false, RoleSelectionRepl::kCuChiNlfServerTeam,
       RoleSelectionRepl::kCuChiNorthGuerillaRoleInfoObjectRef,
       RoleSelectionRepl::kInstalledCuChiNorthGuerillaRoleInfoObjectRef},
  };
  for (const ArtifactRoleCase &entry : cases) {
    const auto canonical =
        requestFor(entry.south, entry.canonicalRoleObjectRef);
    const auto installed =
        requestFor(entry.south, entry.installedRoleObjectRef);

    ASSERT_TRUE(resolve(canonical,
                        RoleSelectionRepl::kCanonicalArtifactVariant,
                        RetailBootstrap::kCapturedRoGameObjectBase,
                        RoleSelectionRepl::kCanonicalRoGameObjectBase,
                        entry.serverTeam)
                    .valid());
    EXPECT_EQ(resolve(installed,
                      RoleSelectionRepl::kCanonicalArtifactVariant,
                      RetailBootstrap::kCapturedRoGameObjectBase,
                      RoleSelectionRepl::kCanonicalRoGameObjectBase,
                      entry.serverTeam)
                  .error,
              RoleSelectionRepl::GroundingError::UnsupportedRoleInfoObject);

    ASSERT_TRUE(resolve(installed,
                        RoleSelectionRepl::kInstalledArtifactVariant,
                        RetailBootstrap::kCapturedRoGameObjectBase,
                        RoleSelectionRepl::kInstalledRoGameObjectBase,
                        entry.serverTeam)
                    .valid());
    EXPECT_EQ(resolve(canonical,
                      RoleSelectionRepl::kInstalledArtifactVariant,
                      RetailBootstrap::kCapturedRoGameObjectBase,
                      RoleSelectionRepl::kInstalledRoGameObjectBase,
                      entry.serverTeam)
                  .error,
              RoleSelectionRepl::GroundingError::UnsupportedRoleInfoObject);
  }

  const auto installedSouth = requestFor(
      true, RoleSelectionRepl::kInstalledCuChiSouthGruntRoleInfoObjectRef);

  EXPECT_EQ(resolve(installedSouth,
                    RoleSelectionRepl::kInstalledArtifactVariant,
                    RetailBootstrap::kCapturedRoGameObjectBase + 1u,
                    RoleSelectionRepl::kInstalledRoGameObjectBase,
                    RoleSelectionRepl::kCuChiUsServerTeam)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedPackageMapBase);
  EXPECT_EQ(resolve(installedSouth,
                    RoleSelectionRepl::kInstalledArtifactVariant,
                    RetailBootstrap::kCapturedRoGameObjectBase,
                    RoleSelectionRepl::kInstalledRoGameObjectBase + 1u,
                    RoleSelectionRepl::kCuChiUsServerTeam)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedPackageMapBase);
  EXPECT_EQ(resolve(installedSouth, "Installed",
                    RetailBootstrap::kCapturedRoGameObjectBase,
                    RoleSelectionRepl::kInstalledRoGameObjectBase,
                    RoleSelectionRepl::kCuChiUsServerTeam)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedArtifactVariant);
}

TEST(RoleSelectionReplication,
     CuChiGroundingRejectsWrongMapModeBaseTeamIntentAndRole) {
  RoleSelectionRepl::SelectRoleByClass rpc;
  rpc.southDesired = true;
  rpc.roleInfoClass = {
      false, RoleSelectionRepl::kCuChiSouthGruntRoleInfoObjectRef};

  auto resolve = [&](const RoleSelectionRepl::SelectRoleByClass& request,
                     std::string_view map, std::string_view mode,
                     uint32_t objectBase, uint32_t team) {
    return RoleSelectionRepl::ResolveGroundedCuChiInfantry(
        request, map, mode, objectBase,
        RoleSelectionRepl::kCanonicalArtifactVariant,
        RoleSelectionRepl::kCanonicalRoGameObjectBase, team);
  };

  EXPECT_EQ(resolve(rpc, "VNTE-Resort", "Territories",
                    RetailBootstrap::kCapturedRoGameObjectBase, 1)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedMap);
  EXPECT_EQ(resolve(rpc, "VNTE-CuChi", "Skirmish",
                    RetailBootstrap::kCapturedRoGameObjectBase, 1)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedMode);
  EXPECT_EQ(resolve(rpc, "VNTE-CuChi", "Territories",
                    RetailBootstrap::kCapturedRoGameObjectBase + 1u, 1)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedPackageMapBase);
  EXPECT_EQ(resolve(rpc, "VNTE-CuChi", "Territories",
                    RetailBootstrap::kCapturedRoGameObjectBase, 3)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedTeam);
  EXPECT_EQ(resolve(rpc, "VNTE-CuChi", "Territories",
                    RetailBootstrap::kCapturedRoGameObjectBase, 257u)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedTeam);
  EXPECT_EQ(resolve(rpc, "VNTE-CuChi", "Territories",
                    RetailBootstrap::kCapturedRoGameObjectBase, 258u)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedTeam);

  auto wrongIntent = rpc;
  wrongIntent.southDesired = false;
  EXPECT_EQ(resolve(wrongIntent, "VNTE-CuChi", "Territories",
                    RetailBootstrap::kCapturedRoGameObjectBase, 1)
                .error,
            RoleSelectionRepl::GroundingError::TeamIntentMismatch);

  auto wrongRole = rpc;
  wrongRole.roleInfoClass.index =
      RoleSelectionRepl::kCuChiNorthGuerillaRoleInfoObjectRef;
  EXPECT_EQ(resolve(wrongRole, "VNTE-CuChi", "Territories",
                    RetailBootstrap::kCapturedRoGameObjectBase, 1)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedRoleInfoObject);

  auto dynamicRole = rpc;
  dynamicRole.roleInfoClass.isDynamic = true;
  EXPECT_EQ(resolve(dynamicRole, "VNTE-CuChi", "Territories",
                    RetailBootstrap::kCapturedRoGameObjectBase, 1)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedRoleInfoObject);

  auto wrongWeapon = rpc;
  wrongWeapon.weaponSelection.presentOnWire = true;
  wrongWeapon.weaponSelection.primaryWeaponIndex = 1;
  wrongWeapon.weaponSelection.primaryWeaponLevel = 255;
  wrongWeapon.weaponSelection.primaryWeaponAmmo = 255;
  EXPECT_EQ(resolve(wrongWeapon, "VNTE-CuChi", "Territories",
                    RetailBootstrap::kCapturedRoGameObjectBase, 1)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedWeaponSelection);

  auto tankSelection = rpc;
  tankSelection.tankSelectionPresent = true;
  EXPECT_EQ(resolve(tankSelection, "VNTE-CuChi", "Territories",
                    RetailBootstrap::kCapturedRoGameObjectBase, 1)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedTankSelection);

  auto unsupportedFlags = rpc;
  unsupportedFlags.allowTeamTank = true;
  EXPECT_EQ(resolve(unsupportedFlags, "VNTE-CuChi", "Territories",
                    RetailBootstrap::kCapturedRoGameObjectBase, 1)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedSelectionFlags);
}

TEST(RoleSelectionReplication,
     CompoundGroundingMapsExactInstalledTeamRegistriesAndLimits) {
  struct GroundedCase {
    bool south;
    uint32_t team;
    uint32_t classRef;
    uint8_t classIndex;
    uint8_t limit;
  };
  const GroundedCase cases[] = {
      {false, RoleSelectionRepl::kCompoundNlfServerTeam,
       RoleSelectionRepl::kCompoundNorthGuerillaRoleClassRef, 0, 255},
      {false, RoleSelectionRepl::kCompoundNlfServerTeam,
       RoleSelectionRepl::kCompoundNorthScoutRoleClassRef, 1, 2},
      {false, RoleSelectionRepl::kCompoundNlfServerTeam,
       RoleSelectionRepl::kCompoundNorthMachineGunnerRoleClassRef, 2, 2},
      {false, RoleSelectionRepl::kCompoundNlfServerTeam,
       RoleSelectionRepl::kCompoundNorthSniperRoleClassRef, 3, 1},
      {false, RoleSelectionRepl::kCompoundNlfServerTeam,
       RoleSelectionRepl::kCompoundNorthSapperRoleClassRef, 4, 2},
      {true, RoleSelectionRepl::kCompoundUsServerTeam,
       RoleSelectionRepl::kCompoundSouthGruntRoleClassRef, 0, 255},
      {true, RoleSelectionRepl::kCompoundUsServerTeam,
       RoleSelectionRepl::kCompoundSouthPointmanRoleClassRef, 1, 2},
      {true, RoleSelectionRepl::kCompoundUsServerTeam,
       RoleSelectionRepl::kCompoundSouthMachineGunnerRoleClassRef, 2, 2},
      {true, RoleSelectionRepl::kCompoundUsServerTeam,
       RoleSelectionRepl::kCompoundSouthMarksmanRoleClassRef, 3, 1},
      {true, RoleSelectionRepl::kCompoundUsServerTeam,
       RoleSelectionRepl::kCompoundSouthEngineerRoleClassRef, 4, 2},
  };

  for (const GroundedCase &entry : cases) {
    RoleSelectionRepl::SelectRoleByClass rpc;
    rpc.southDesired = entry.south;
    rpc.roleInfoClass = {false, entry.classRef};
    const auto grounded = RoleSelectionRepl::ResolveGroundedCompoundRole(
        rpc, "vnsk-compound", "skirmish", "installed",
        RoleSelectionRepl::kInstalledRoGameObjectBase, entry.team);
    ASSERT_TRUE(grounded.valid());
    EXPECT_EQ(grounded.role.roleInfoObjectRef, entry.classRef);
    EXPECT_EQ(grounded.role.classIndex, entry.classIndex);
    EXPECT_EQ(grounded.role.roleLimit, entry.limit);
    EXPECT_FALSE(grounded.role.changedRole.has_value());
    EXPECT_EQ(grounded.role.squadIndex, 255u);
    EXPECT_EQ(grounded.role.roleIndex, 255u);
  }
}

TEST(RoleSelectionReplication,
     CompoundGroundingRejectsWrongContextAndUnregisteredClasses) {
  RoleSelectionRepl::SelectRoleByClass rpc;
  rpc.southDesired = true;
  rpc.roleInfoClass = {
      false, RoleSelectionRepl::kCompoundSouthGruntRoleClassRef};

  auto resolve = [&](const RoleSelectionRepl::SelectRoleByClass &request,
                     std::string_view map, std::string_view mode,
                     std::string_view variant, uint32_t objectBase,
                     uint32_t team) {
    return RoleSelectionRepl::ResolveGroundedCompoundRole(
        request, map, mode, variant, objectBase, team);
  };

  EXPECT_EQ(resolve(rpc, "VNTE-Resort", "Skirmish", "installed",
                    RoleSelectionRepl::kInstalledRoGameObjectBase, 1)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedMap);
  EXPECT_EQ(resolve(rpc, "VNSK-Compound", "Territories", "installed",
                    RoleSelectionRepl::kInstalledRoGameObjectBase, 1)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedMode);
  EXPECT_EQ(resolve(rpc, "VNSK-Compound", "Skirmish", "Installed",
                    RoleSelectionRepl::kInstalledRoGameObjectBase, 1)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedArtifactVariant);
  EXPECT_EQ(resolve(rpc, "VNSK-Compound", "Skirmish", "installed",
                    RoleSelectionRepl::kInstalledRoGameObjectBase + 1u, 1)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedPackageMapBase);
  EXPECT_EQ(resolve(rpc, "VNSK-Compound", "Skirmish", "installed",
                    RoleSelectionRepl::kInstalledRoGameObjectBase, 3)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedTeam);

  auto wrongIntent = rpc;
  wrongIntent.southDesired = false;
  EXPECT_EQ(resolve(wrongIntent, "VNSK-Compound", "Skirmish", "installed",
                    RoleSelectionRepl::kInstalledRoGameObjectBase, 1)
                .error,
            RoleSelectionRepl::GroundingError::TeamIntentMismatch);

  auto wrongTeamClass = rpc;
  wrongTeamClass.roleInfoClass.index =
      RoleSelectionRepl::kCompoundNorthGuerillaRoleClassRef;
  EXPECT_EQ(resolve(wrongTeamClass, "VNSK-Compound", "Skirmish", "installed",
                    RoleSelectionRepl::kInstalledRoGameObjectBase, 1)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedRoleInfoObject);

  auto adjacentClass = rpc;
  adjacentClass.roleInfoClass.index =
      RoleSelectionRepl::kCompoundSouthGruntRoleClassRef + 1u;
  EXPECT_EQ(resolve(adjacentClass, "VNSK-Compound", "Skirmish", "installed",
                    RoleSelectionRepl::kInstalledRoGameObjectBase, 1)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedRoleInfoObject);

  auto dynamicClass = rpc;
  dynamicClass.roleInfoClass.isDynamic = true;
  EXPECT_EQ(resolve(dynamicClass, "VNSK-Compound", "Skirmish", "installed",
                    RoleSelectionRepl::kInstalledRoGameObjectBase, 1)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedRoleInfoObject);

  auto tank = rpc;
  tank.tankSelectionPresent = true;
  EXPECT_EQ(resolve(tank, "VNSK-Compound", "Skirmish", "installed",
                    RoleSelectionRepl::kInstalledRoGameObjectBase, 1)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedTankSelection);

  auto flags = rpc;
  flags.desiredContext = true;
  EXPECT_EQ(resolve(flags, "VNSK-Compound", "Skirmish", "installed",
                    RoleSelectionRepl::kInstalledRoGameObjectBase, 1)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedSelectionFlags);
}

TEST(RoleSelectionReplication,
     CompoundGroundingAcceptsOnlySourceGroundedWeaponSelections) {
  auto request = [](bool south, uint32_t classRef, uint8_t primaryIndex,
                    uint8_t primaryLevel, uint8_t primaryAmmo,
                    uint8_t secondaryIndex = 0,
                    uint8_t secondaryLevel = 0) {
    RoleSelectionRepl::SelectRoleByClass rpc;
    rpc.southDesired = south;
    rpc.roleInfoClass = {false, classRef};
    rpc.weaponSelection.presentOnWire = true;
    rpc.weaponSelection.primaryWeaponIndex = primaryIndex;
    rpc.weaponSelection.primaryWeaponLevel = primaryLevel;
    rpc.weaponSelection.primaryWeaponAmmo = primaryAmmo;
    rpc.weaponSelection.secondaryWeaponIndex = secondaryIndex;
    rpc.weaponSelection.secondaryWeaponLevel = secondaryLevel;
    return rpc;
  };
  auto resolve = [](const RoleSelectionRepl::SelectRoleByClass &rpc,
                    uint32_t team) {
    return RoleSelectionRepl::ResolveGroundedCompoundRole(
        rpc, "VNSK-Compound", "Skirmish", "installed",
        RoleSelectionRepl::kInstalledRoGameObjectBase, team);
  };

  // Existing/default sentinel form from the live Grunt request.
  EXPECT_TRUE(resolve(request(true,
                              RoleSelectionRepl::
                                  kCompoundSouthGruntRoleClassRef,
                              0, 255, 255),
                      RoleSelectionRepl::kCompoundUsServerTeam)
                  .valid());

  // The Pointman live sequence first advertises index 1 with default sentinels,
  // then confirms the concrete level/ammo-zero selection.
  EXPECT_TRUE(resolve(request(true,
                              RoleSelectionRepl::
                                  kCompoundSouthPointmanRoleClassRef,
                              1, 255, 255),
                      RoleSelectionRepl::kCompoundUsServerTeam)
                  .valid());
  EXPECT_TRUE(resolve(request(true,
                              RoleSelectionRepl::
                                  kCompoundSouthPointmanRoleClassRef,
                              1, 0, 0),
                      RoleSelectionRepl::kCompoundUsServerTeam)
                  .valid());
  EXPECT_TRUE(resolve(request(false,
                              RoleSelectionRepl::
                                  kCompoundNorthGuerillaRoleClassRef,
                              1, 0, 0),
                      RoleSelectionRepl::kCompoundNlfServerTeam)
                  .valid());

  EXPECT_EQ(resolve(request(true,
                            RoleSelectionRepl::
                                kCompoundSouthMachineGunnerRoleClassRef,
                            1, 0, 0),
                    RoleSelectionRepl::kCompoundUsServerTeam)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedWeaponSelection);
  EXPECT_EQ(resolve(request(true,
                            RoleSelectionRepl::
                                kCompoundSouthPointmanRoleClassRef,
                            2, 0, 0),
                    RoleSelectionRepl::kCompoundUsServerTeam)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedWeaponSelection);
  EXPECT_EQ(resolve(request(true,
                            RoleSelectionRepl::
                                kCompoundSouthPointmanRoleClassRef,
                            1, 1, 0),
                    RoleSelectionRepl::kCompoundUsServerTeam)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedWeaponSelection);
  EXPECT_EQ(resolve(request(true,
                            RoleSelectionRepl::
                                kCompoundSouthPointmanRoleClassRef,
                            1, 0, 0, 1, 0),
                    RoleSelectionRepl::kCompoundUsServerTeam)
                .error,
            RoleSelectionRepl::GroundingError::UnsupportedWeaponSelection);
}

TEST(RoleSelectionReplication, EmitsCapturedOwnerPriAndChangedRoleOrder) {
  BitWriter classWriter;
  RoleSelectionRepl::WriteOwnerPriClassIndex(classWriter, 0);
  const std::vector<uint8_t> classBytes = classWriter.GetBytes();
  EXPECT_EQ(classWriter.NumBits(), 15u);
  EXPECT_EQ(classBytes, Hex("4f00")); // f2350 h79 prefix, zero-padded
  BitReader classReader(classBytes.data(), classBytes.size(),
                        classWriter.NumBits());
  EXPECT_EQ(classReader.SerializeInt(
                RoleSelectionRepl::kRoPlayerReplicationInfoMaxHandle),
            RoleSelectionRepl::kPriClassIndexHandle);
  EXPECT_EQ(classReader.ReadByte(), 0u);
  EXPECT_EQ(classReader.BitsLeft(), 0u);

  BitWriter assignmentWriter;
  RoleSelectionRepl::WriteOwnerPriRoleAssignment(assignmentWriter, 8, 5);
  const std::vector<uint8_t> assignmentBytes = assignmentWriter.GetBytes();
  EXPECT_EQ(assignmentWriter.NumBits(), 30u);
  EXPECT_EQ(assignmentBytes, Hex("51046801")); // exact f2537 ch26 payload
  BitReader assignmentReader(assignmentBytes.data(), assignmentBytes.size(),
                             assignmentWriter.NumBits());
  EXPECT_EQ(assignmentReader.SerializeInt(
                RoleSelectionRepl::kRoPlayerReplicationInfoMaxHandle),
            RoleSelectionRepl::kPriSquadIndexHandle);
  EXPECT_EQ(assignmentReader.ReadByte(), 8u);
  EXPECT_EQ(assignmentReader.SerializeInt(
                RoleSelectionRepl::kRoPlayerReplicationInfoMaxHandle),
            RoleSelectionRepl::kPriRoleIndexHandle);
  EXPECT_EQ(assignmentReader.ReadByte(), 5u);
  EXPECT_EQ(assignmentReader.BitsLeft(), 0u);

  uint32_t changedBits = 0;
  const std::vector<uint8_t> changed =
      RoleSelectionRepl::EncodeChangedRole(255, 0, false, true, changedBits);
  EXPECT_EQ(changedBits, 21u);
  EXPECT_EQ(changed, Hex("d2fe13")); // exact f2537 h210 prefix, padded
  BitReader changedReader(changed.data(), changed.size(), changedBits);
  EXPECT_EQ(changedReader.SerializeInt(
                RoleSelectionRepl::kRoPlayerControllerMaxHandle),
            210u);
  ASSERT_TRUE(changedReader.ReadBit());
  EXPECT_EQ(changedReader.ReadByte(), 255u);
  EXPECT_FALSE(changedReader.ReadBit());
  EXPECT_FALSE(changedReader.ReadBit());
  EXPECT_TRUE(changedReader.ReadBit());
  EXPECT_EQ(changedReader.BitsLeft(), 0u);
}

TEST(RoleSelectionReplication,
     EmitsCapturedResortNorthCombinedTransitionAndPriState) {
  // North f61973 publishes h79 ClassIndex=0. The property encoding is the same
  // capture-grounded owner-PRI primitive used above.
  BitWriter classWriter;
  RoleSelectionRepl::WriteOwnerPriClassIndex(classWriter, 0);
  EXPECT_EQ(classWriter.NumBits(), 15u);
  EXPECT_EQ(classWriter.GetBytes(), Hex("4f00"));

  // North f61989 reliable ch2 seq47 is exactly h210(255,0,false,true), then
  // h211(2,3), with no padding between the RPCs.
  RoleSelectionRepl::ChangedRoleEvidence transitionEvidence{
      255, 0, false, true,
      RoleSelectionRepl::ChangedSquadEvidence{2, 3}};
  uint32_t transitionBits = 0;
  const std::vector<uint8_t> transition =
      RoleSelectionRepl::EncodeChangedRoleTransition(transitionEvidence,
                                                     transitionBits);
  EXPECT_EQ(transitionBits, 48u);
  EXPECT_EQ(transition, Hex("d2fe735a8103"));

  BitReader transitionReader(transition.data(), transition.size(),
                             transitionBits);
  EXPECT_EQ(transitionReader.SerializeInt(
                RoleSelectionRepl::kRoPlayerControllerMaxHandle),
            RoleSelectionRepl::kChangedRoleHandle);
  ASSERT_TRUE(transitionReader.ReadBit());
  EXPECT_EQ(transitionReader.ReadByte(), 255u);
  EXPECT_FALSE(transitionReader.ReadBit());
  EXPECT_FALSE(transitionReader.ReadBit());
  EXPECT_TRUE(transitionReader.ReadBit());
  EXPECT_EQ(transitionReader.SerializeInt(
                RoleSelectionRepl::kRoPlayerControllerMaxHandle),
            RoleSelectionRepl::kChangedSquadHandle);
  ASSERT_TRUE(transitionReader.ReadBit());
  EXPECT_EQ(transitionReader.ReadByte(), 2u);
  ASSERT_TRUE(transitionReader.ReadBit());
  EXPECT_EQ(transitionReader.ReadByte(), 3u);
  EXPECT_EQ(transitionReader.BitsLeft(), 0u);

  // North f62024 independently publishes h81 SquadIndex=2, then h80
  // RoleIndex=3 as the later owner-PRI property confirmation.
  BitWriter assignmentWriter;
  RoleSelectionRepl::WriteOwnerPriRoleAssignment(assignmentWriter, 2, 3);
  const std::vector<uint8_t> assignment = assignmentWriter.GetBytes();
  EXPECT_EQ(assignmentWriter.NumBits(), 30u);
  EXPECT_EQ(assignment, Hex("5101e800"));

  BitReader reader(assignment.data(), assignment.size(),
                   assignmentWriter.NumBits());
  EXPECT_EQ(
      reader.SerializeInt(RoleSelectionRepl::kRoPlayerReplicationInfoMaxHandle),
      RoleSelectionRepl::kPriSquadIndexHandle);
  EXPECT_EQ(reader.ReadByte(), 2u);
  EXPECT_EQ(
      reader.SerializeInt(RoleSelectionRepl::kRoPlayerReplicationInfoMaxHandle),
      RoleSelectionRepl::kPriRoleIndexHandle);
  EXPECT_EQ(reader.ReadByte(), 3u);
  EXPECT_EQ(reader.BitsLeft(), 0u);
}

TEST(RoleSelectionReplication,
     EmitsCompoundNorthFirstRuntimeSquadTransition) {
  RoleSystem roles(nullptr);
  const RetailSquadAssignment assignment = roles.AutoAssignRetailSquad(
      1u, RoleSelectionRepl::kCompoundNlfServerTeam);
  ASSERT_TRUE(assignment.IsValid());
  EXPECT_EQ(assignment.squadIndex, 0u);
  EXPECT_EQ(assignment.roleIndex, 0u);

  RoleSelectionRepl::ChangedRoleEvidence transitionEvidence{
      255, 0, false, true,
      RoleSelectionRepl::ChangedSquadEvidence{assignment.squadIndex,
                                               assignment.roleIndex}};
  uint32_t transitionBits = 0;
  const std::vector<uint8_t> transition =
      RoleSelectionRepl::EncodeChangedRoleTransition(transitionEvidence,
                                                     transitionBits);
  EXPECT_EQ(transitionBits, 32u);
  EXPECT_EQ(transition, Hex("d2fe731a"));
}

TEST(RoleSelectionReplication, EmitsStandaloneChangedSquadWithRetailDefaults) {
  uint32_t changedSquadBits = 0;
  const std::vector<uint8_t> changedSquad =
      RoleSelectionRepl::EncodeChangedSquad(
          RoleSelectionRepl::ChangedSquadEvidence{2, 3}, changedSquadBits);
  EXPECT_EQ(changedSquadBits, 27u);
  EXPECT_EQ(changedSquad, Hex("d30a1c00"));

  BitReader reader(changedSquad.data(), changedSquad.size(),
                   changedSquadBits);
  EXPECT_EQ(reader.SerializeInt(
                RoleSelectionRepl::kRoPlayerControllerMaxHandle),
            RoleSelectionRepl::kChangedSquadHandle);
  ASSERT_TRUE(reader.ReadBit());
  EXPECT_EQ(reader.ReadByte(), 2u);
  ASSERT_TRUE(reader.ReadBit());
  EXPECT_EQ(reader.ReadByte(), 3u);
  EXPECT_EQ(reader.BitsLeft(), 0u);

  uint32_t defaultBits = 0;
  const std::vector<uint8_t> defaults =
      RoleSelectionRepl::EncodeChangedSquad(
          RoleSelectionRepl::ChangedSquadEvidence{0, 0}, defaultBits);
  EXPECT_EQ(defaultBits, 11u);
  EXPECT_EQ(defaults, Hex("d300"));
  BitReader defaultReader(defaults.data(), defaults.size(), defaultBits);
  EXPECT_EQ(defaultReader.SerializeInt(
                RoleSelectionRepl::kRoPlayerControllerMaxHandle),
            RoleSelectionRepl::kChangedSquadHandle);
  EXPECT_FALSE(defaultReader.ReadBit());
  EXPECT_FALSE(defaultReader.ReadBit());
  EXPECT_EQ(defaultReader.BitsLeft(), 0u);
}

RS2V_TEST_MAIN()
