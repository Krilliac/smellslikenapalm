#include "Network/RoleSelectionReplication.h"

#include <cctype>

#include "Network/BitWriter.h"
#include "Network/RetailBootstrap.h"

namespace RoleSelectionRepl {
namespace {

bool IsValidBuffer(const uint8_t *payload, size_t payloadBytes,
                   size_t payloadBits) noexcept {
  if (!payload || payloadBits == 0 ||
      payloadBits > kMaximumRoleSelectionBunchBits) {
    return false;
  }
  const size_t requiredBytes =
      payloadBits / 8u + static_cast<size_t>((payloadBits % 8u) != 0);
  return payloadBytes >= requiredBytes;
}

bool EqualsInsensitive(std::string_view lhs, std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size())
    return false;
  for (size_t i = 0; i < lhs.size(); ++i) {
    const auto left = static_cast<unsigned char>(lhs[i]);
    const auto right = static_cast<unsigned char>(rhs[i]);
    if (std::tolower(left) != std::tolower(right))
      return false;
  }
  return true;
}

bool DecodeNoParameterRpc(BitReader &reader, uint32_t expectedHandle,
                          DecodeError &error) {
  BitReader trial = reader;
  const uint32_t handle = trial.SerializeInt(kRoPlayerControllerMaxHandle);
  if (trial.IsOverflowed()) {
    error = DecodeError::Truncated;
    return false;
  }
  if (handle != expectedHandle) {
    error = DecodeError::UnsupportedFollowingRpc;
    return false;
  }
  reader = trial;
  error = DecodeError::None;
  return true;
}

bool HasCapturedDefaultWeaponSelection(
    const WeaponSelectionInfo &selection) noexcept {
  if (!selection.presentOnWire)
    return true;
  return selection.primaryWeaponIndex == 0u &&
         selection.primaryWeaponLevel == 255u &&
         selection.primaryWeaponAmmo == 255u &&
         selection.secondaryWeaponIndex == 0u &&
         selection.secondaryWeaponLevel == 0u;
}

void WriteChangedSquad(BitWriter &writer,
                       const ChangedSquadEvidence &evidence) {
  writer.SerializeInt(kChangedSquadHandle, kRoPlayerControllerMaxHandle);
  writer.WriteBit(evidence.squadIndex != 0u);
  if (evidence.squadIndex != 0u)
    writer.WriteByte(evidence.squadIndex);
  writer.WriteBit(evidence.roleIndex != 0u);
  if (evidence.roleIndex != 0u)
    writer.WriteByte(evidence.roleIndex);
}

struct CompoundRoleEntry {
  bool southTeam;
  uint32_t classRef;
  uint8_t classIndex;
  uint8_t roleLimit;
  bool primaryWeaponIndexOneGrounded;
  bool capturedIndexOneDefault;
};

constexpr CompoundRoleEntry kCompoundRoles[] = {
    {false, kCompoundNorthGuerillaRoleClassRef,
     kCompoundRiflemanClassIndex, kCompoundRiflemanLimit,
     /*primaryWeaponIndexOneGrounded=*/true,
     /*capturedIndexOneDefault=*/false},
    {false, kCompoundNorthScoutRoleClassRef, kCompoundPointmanClassIndex,
     kCompoundPointmanLimit, false, false},
    {false, kCompoundNorthMachineGunnerRoleClassRef,
     kCompoundMachineGunnerClassIndex, kCompoundMachineGunnerLimit, false,
     false},
    {false, kCompoundNorthSniperRoleClassRef, kCompoundMarksmanClassIndex,
     kCompoundMarksmanLimit, false, false},
    {false, kCompoundNorthSapperRoleClassRef, kCompoundEngineerClassIndex,
     kCompoundEngineerLimit, false, false},
    {true, kCompoundSouthGruntRoleClassRef, kCompoundRiflemanClassIndex,
     kCompoundRiflemanLimit, false, false},
    {true, kCompoundSouthPointmanRoleClassRef, kCompoundPointmanClassIndex,
     kCompoundPointmanLimit,
     /*primaryWeaponIndexOneGrounded=*/true,
     /*capturedIndexOneDefault=*/true},
    {true, kCompoundSouthMachineGunnerRoleClassRef,
     kCompoundMachineGunnerClassIndex, kCompoundMachineGunnerLimit, false,
     false},
    {true, kCompoundSouthMarksmanRoleClassRef, kCompoundMarksmanClassIndex,
     kCompoundMarksmanLimit, false, false},
    {true, kCompoundSouthEngineerRoleClassRef, kCompoundEngineerClassIndex,
     kCompoundEngineerLimit, false, false},
};

const CompoundRoleEntry *FindCompoundRole(bool southTeam,
                                          uint32_t classRef) noexcept {
  for (const CompoundRoleEntry &role : kCompoundRoles) {
    if (role.southTeam == southTeam && role.classRef == classRef)
      return &role;
  }
  return nullptr;
}

bool HasGroundedCompoundWeaponSelection(
    const WeaponSelectionInfo &selection,
    const CompoundRoleEntry &role) noexcept {
  if (!selection.presentOnWire)
    return true;

  const bool noSecondary = selection.secondaryWeaponIndex == 0u &&
                           selection.secondaryWeaponLevel == 0u;
  if (!noSecondary)
    return false;

  // Existing captures use the 255 sentinels for an unchanged/default choice.
  if (selection.primaryWeaponLevel == 255u &&
      selection.primaryWeaponAmmo == 255u) {
    return selection.primaryWeaponIndex == 0u ||
           (selection.primaryWeaponIndex == 1u &&
            role.capturedIndexOneDefault);
  }

  // The live installed-package final request supplies concrete zero-valued
  // level/ammo fields. Permit index 1 only for roles whose source metadata
  // independently proves that choice exists.
  return role.primaryWeaponIndexOneGrounded &&
         selection.primaryWeaponIndex == 1u &&
         selection.primaryWeaponLevel == 0u &&
         selection.primaryWeaponAmmo == 0u;
}

} // namespace

GroundedRoleProfile ClassifyGroundedRoleProfile(
    std::string_view mapUrl, std::string_view modeName,
    uint32_t profileRoleRegistryObjectBase,
    std::string_view artifactVariant, uint32_t artifactRoGameObjectBase,
    bool roleRegistryGrounded) noexcept {
  if (artifactVariant == kCanonicalArtifactVariant &&
      profileRoleRegistryObjectBase ==
          RetailBootstrap::kCapturedRoGameObjectBase &&
      artifactRoGameObjectBase == kCanonicalRoGameObjectBase &&
      roleRegistryGrounded) {
    if (EqualsInsensitive(mapUrl, "VNTE-Resort") &&
        EqualsInsensitive(modeName, "Territories")) {
      return GroundedRoleProfile::CanonicalResort;
    }
    if (EqualsInsensitive(mapUrl, "VNTE-CuChi") &&
        EqualsInsensitive(modeName, "Territories")) {
      return GroundedRoleProfile::CanonicalCuChi;
    }
  }

  if (artifactVariant == kInstalledArtifactVariant &&
      profileRoleRegistryObjectBase == 0u &&
      artifactRoGameObjectBase == kInstalledRoGameObjectBase &&
      EqualsInsensitive(mapUrl, "VNSK-Compound") &&
      EqualsInsensitive(modeName, "Skirmish")) {
    return GroundedRoleProfile::InstalledCompound;
  }

  return GroundedRoleProfile::Unsupported;
}

bool DecodeOne(BitReader &reader, SelectRoleByClass &output,
               DecodeError &error) {
  BitReader trial = reader;
  SelectRoleByClass decoded;
  const size_t startBit = trial.BitPos();

  const uint32_t handle = trial.SerializeInt(kRoPlayerControllerMaxHandle);
  if (trial.IsOverflowed()) {
    error = DecodeError::Truncated;
    return false;
  }
  if (handle != kSelectRoleByClassHandle) {
    error = DecodeError::UnsupportedHandle;
    return false;
  }

  decoded.southDesired = trial.ReadBit();

  const bool roleInfoPresent = trial.ReadBit();
  if (!roleInfoPresent) {
    if (trial.IsOverflowed()) {
      error = DecodeError::Truncated;
    } else {
      error = DecodeError::MissingRoleInfoClass;
    }
    return false;
  }
  decoded.roleInfoClass = ActorRepl::ReadNetGUID(trial);
  if (trial.IsOverflowed()) {
    error = DecodeError::Truncated;
    return false;
  }
  if (decoded.roleInfoClass.isDynamic) {
    error = DecodeError::DynamicRoleInfoClass;
    return false;
  }
  if (decoded.roleInfoClass.index == 0u) {
    error = DecodeError::ZeroRoleInfoClass;
    return false;
  }

  decoded.weaponSelection.presentOnWire = trial.ReadBit();
  if (decoded.weaponSelection.presentOnWire) {
    decoded.weaponSelection.primaryWeaponIndex = trial.ReadByte();
    decoded.weaponSelection.primaryWeaponLevel = trial.ReadByte();
    decoded.weaponSelection.primaryWeaponAmmo = trial.ReadByte();
    decoded.weaponSelection.secondaryWeaponIndex = trial.ReadByte();
    decoded.weaponSelection.secondaryWeaponLevel = trial.ReadByte();
  }
  if (trial.IsOverflowed()) {
    error = DecodeError::Truncated;
    return false;
  }

  decoded.tankSelectionPresent = trial.ReadBit();
  if (decoded.tankSelectionPresent) {
    decoded.tankSelection = ActorRepl::ReadNetGUID(trial);
    if (trial.IsOverflowed()) {
      error = DecodeError::Truncated;
      return false;
    }
    // No vehicle-role metadata is grounded in this tranche.  Decode the
    // complete object reference first so a truncated input remains a
    // truncation rather than being mislabeled as a supported request.
    error = DecodeError::UnsupportedTankSelection;
    return false;
  }

  decoded.allowTeamTank = trial.ReadBit();
  decoded.desiredContext = trial.ReadBit();
  decoded.closeMenu = trial.ReadBit();
  if (trial.IsOverflowed()) {
    error = DecodeError::Truncated;
    return false;
  }

  decoded.consumedBits = trial.BitPos() - startBit;
  reader = trial;
  output = decoded;
  error = DecodeError::None;
  return true;
}

DecodeResult DecodeRoleSelectionBunch(const uint8_t *payload,
                                      size_t payloadBytes, size_t payloadBits) {
  DecodeResult result;
  if (!payload || payloadBits == 0)
    return result;
  if (payloadBits > kMaximumRoleSelectionBunchBits) {
    result.error = DecodeError::Oversized;
    return result;
  }
  if (!IsValidBuffer(payload, payloadBytes, payloadBits))
    return result;

  BitReader reader(payload, payloadBytes, payloadBits);
  DecodeError error = DecodeError::None;
  if (!DecodeOne(reader, result.rpc, error)) {
    result.error = error;
    return result;
  }
  if (reader.BitsLeft() == 0) {
    result.error = DecodeError::None;
    return result;
  }

  if (result.rpc.closeMenu) {
    if (!DecodeNoParameterRpc(reader, kServerAutoSelectSquadHandle, error)) {
      result.error = error;
      return result;
    }
    if (reader.BitsLeft() == 0) {
      result.following = FollowingRpcPattern::FinalAutoSelectSquad;
      result.error = DecodeError::None;
      return result;
    }

    const uint32_t spectatorHandle =
        reader.SerializeInt(kRoPlayerControllerMaxHandle);
    if (reader.IsOverflowed()) {
      result.error = DecodeError::Truncated;
      return result;
    }
    if (spectatorHandle != kServerSetSpectatorLocationHandle) {
      result.error = DecodeError::UnsupportedFollowingRpc;
      return result;
    }
    const bool spectatorLocationPresent = reader.ReadBit();
    if (reader.IsOverflowed()) {
      result.error = DecodeError::Truncated;
      return result;
    }
    if (spectatorLocationPresent) {
      // No non-default Vector form has yet been captured as part of this
      // reliable role-selection bunch. Keep it fail-closed until its exact
      // compressed-vector tail is grounded.
      result.error = DecodeError::UnsupportedFollowingRpc;
      return result;
    }
    if (reader.BitsLeft() != 0) {
      result.error = DecodeError::TrailingBits;
      return result;
    }
    result.following =
        FollowingRpcPattern::FinalAutoSelectSquadAndDefaultSpectatorLocation;
    result.error = DecodeError::None;
    return result;
  }

  const uint32_t readyHandle =
      reader.SerializeInt(kRoPlayerControllerMaxHandle);
  if (reader.IsOverflowed()) {
    result.error = DecodeError::Truncated;
    return result;
  }
  if (readyHandle != kServerSetReadyToSpawnHandle) {
    result.error = DecodeError::UnsupportedFollowingRpc;
    return result;
  }
  const bool readyPresent = reader.ReadBit();
  const uint8_t readyStatus =
      readyPresent ? static_cast<uint8_t>(reader.ReadBits(2)) : 0u;
  if (reader.IsOverflowed()) {
    result.error = DecodeError::Truncated;
    return result;
  }
  // Both capture-grounded interim requests carry an explicit status value 2.
  // Other values are syntactically decodable but have no established meaning
  // for this path, so do not broaden the accepted protocol by inference.
  if (!readyPresent || readyStatus != 2u) {
    result.error = DecodeError::InvalidReadyStatus;
    return result;
  }

  // Role authorization is intentionally separate from structural decoding.
  // Both exact observed interim shapes are complete at this point or after one
  // no-parameter h182.
  if (reader.BitsLeft() == 0) {
    result.following = FollowingRpcPattern::InterimReadyOnly;
    result.error = DecodeError::None;
    return result;
  }
  if (!DecodeNoParameterRpc(reader, kServerResetSpectateModeHandle, error)) {
    result.error = error;
    return result;
  }
  if (reader.BitsLeft() != 0) {
    result.error = DecodeError::TrailingBits;
    return result;
  }
  result.following = FollowingRpcPattern::InterimReadyAndResetSpectate;
  result.error = DecodeError::None;
  return result;
}

GroundingResult ResolveGroundedResortInfantry(const SelectRoleByClass &rpc,
                                              std::string_view mapUrl,
                                              uint32_t serverTeamId) {
  GroundingResult result;
  if (!EqualsInsensitive(mapUrl, "VNTE-Resort")) {
    result.error = GroundingError::UnsupportedMap;
    return result;
  }
  if (serverTeamId != kResortUsServerTeam &&
      serverTeamId != kResortNvaServerTeam) {
    result.error = GroundingError::UnsupportedTeam;
    return result;
  }
  const bool southTeam = serverTeamId == kResortUsServerTeam;
  if (rpc.southDesired != southTeam) {
    result.error = GroundingError::TeamIntentMismatch;
    return result;
  }
  const uint32_t expectedObjectRef =
      southTeam ? kResortSouthGruntRoleInfoObjectRef
                : kResortNorthRiflemanRoleInfoObjectRef;
  if (rpc.roleInfoClass.isDynamic ||
      rpc.roleInfoClass.index != expectedObjectRef) {
    result.error = GroundingError::UnsupportedRoleInfoObject;
    return result;
  }
  if (rpc.tankSelectionPresent) {
    result.error = GroundingError::UnsupportedTankSelection;
    return result;
  }
  if (!HasCapturedDefaultWeaponSelection(rpc.weaponSelection)) {
    result.error = GroundingError::UnsupportedWeaponSelection;
    return result;
  }
  if (rpc.allowTeamTank || rpc.desiredContext) {
    result.error = GroundingError::UnsupportedSelectionFlags;
    return result;
  }

  result.role.roleInfoObjectRef = expectedObjectRef;
  if (southTeam) {
    result.role.classIndex = kResortSouthGruntClassIndex;
    result.role.changedRole = ChangedRoleEvidence{
        kResortSouthGruntChangedRoleSquadIndex, kResortSouthGruntClassIndex,
        /*showLobby=*/false,
        /*showSpawnSelect=*/true};
    result.role.squadIndex = kResortSouthGruntSquadIndex;
    result.role.roleIndex = kResortSouthGruntRoleIndex;
  } else {
    result.role.classIndex = kResortNvaRiflemanClassIndex;
    // Exact retail frame 61989: h210(255,0,false,true) followed immediately by
    // h211(2,3) in one reliable ch2 bunch. Frame 62024 independently confirms
    // the same h81/h80 owner-PRI squad/role state.
    result.role.changedRole = ChangedRoleEvidence{
        kResortNvaRiflemanChangedRoleSquadIndex,
        kResortNvaRiflemanClassIndex,
        /*showLobby=*/false,
        /*showSpawnSelect=*/true,
        ChangedSquadEvidence{kResortNvaRiflemanSquadIndex,
                             kResortNvaRiflemanRoleIndex}};
    result.role.squadIndex = kResortNvaRiflemanSquadIndex;
    result.role.roleIndex = kResortNvaRiflemanRoleIndex;
  }
  result.role.primaryWeaponIndex = rpc.weaponSelection.primaryWeaponIndex;
  result.role.secondaryWeaponIndex = rpc.weaponSelection.secondaryWeaponIndex;
  result.error = GroundingError::None;
  return result;
}

GroundingResult ResolveGroundedCuChiInfantry(
    const SelectRoleByClass &rpc, std::string_view mapUrl,
    std::string_view modeName, uint32_t roGameObjectBase,
    uint32_t serverTeamId) {
  GroundingResult result;
  if (!EqualsInsensitive(mapUrl, "VNTE-CuChi")) {
    result.error = GroundingError::UnsupportedMap;
    return result;
  }
  if (!EqualsInsensitive(modeName, "Territories")) {
    result.error = GroundingError::UnsupportedMode;
    return result;
  }
  if (roGameObjectBase != RetailBootstrap::kCapturedRoGameObjectBase) {
    result.error = GroundingError::UnsupportedPackageMapBase;
    return result;
  }
  if (serverTeamId != kCuChiUsServerTeam &&
      serverTeamId != kCuChiNlfServerTeam) {
    result.error = GroundingError::UnsupportedTeam;
    return result;
  }

  const bool southTeam = serverTeamId == kCuChiUsServerTeam;
  if (rpc.southDesired != southTeam) {
    result.error = GroundingError::TeamIntentMismatch;
    return result;
  }
  const uint32_t expectedObjectRef =
      southTeam ? kCuChiSouthGruntRoleInfoObjectRef
                : kCuChiNorthGuerillaRoleInfoObjectRef;
  if (rpc.roleInfoClass.isDynamic ||
      rpc.roleInfoClass.index != expectedObjectRef) {
    result.error = GroundingError::UnsupportedRoleInfoObject;
    return result;
  }
  if (rpc.tankSelectionPresent) {
    result.error = GroundingError::UnsupportedTankSelection;
    return result;
  }
  if (!HasCapturedDefaultWeaponSelection(rpc.weaponSelection)) {
    result.error = GroundingError::UnsupportedWeaponSelection;
    return result;
  }
  if (rpc.allowTeamTank || rpc.desiredContext) {
    result.error = GroundingError::UnsupportedSelectionFlags;
    return result;
  }

  result.role.roleInfoObjectRef = expectedObjectRef;
  result.role.classIndex = kCuChiInfantryClassIndex;
  result.role.primaryWeaponIndex = rpc.weaponSelection.primaryWeaponIndex;
  result.role.secondaryWeaponIndex = rpc.weaponSelection.secondaryWeaponIndex;
  // Keep ChangedRole, SquadIndex, and RoleIndex at their fail-closed defaults.
  result.error = GroundingError::None;
  return result;
}

GroundingResult ResolveGroundedCompoundRole(
    const SelectRoleByClass &rpc, std::string_view mapUrl,
    std::string_view modeName, std::string_view artifactVariant,
    uint32_t roGameObjectBase, uint32_t serverTeamId) {
  GroundingResult result;
  if (!EqualsInsensitive(mapUrl, "VNSK-Compound")) {
    result.error = GroundingError::UnsupportedMap;
    return result;
  }
  if (!EqualsInsensitive(modeName, "Skirmish")) {
    result.error = GroundingError::UnsupportedMode;
    return result;
  }
  if (artifactVariant != kInstalledArtifactVariant) {
    result.error = GroundingError::UnsupportedArtifactVariant;
    return result;
  }
  if (roGameObjectBase != kInstalledRoGameObjectBase) {
    result.error = GroundingError::UnsupportedPackageMapBase;
    return result;
  }
  if (serverTeamId != kCompoundUsServerTeam &&
      serverTeamId != kCompoundNlfServerTeam) {
    result.error = GroundingError::UnsupportedTeam;
    return result;
  }

  const bool southTeam = serverTeamId == kCompoundUsServerTeam;
  if (rpc.southDesired != southTeam) {
    result.error = GroundingError::TeamIntentMismatch;
    return result;
  }
  if (rpc.roleInfoClass.isDynamic || rpc.roleInfoClass.index == 0u) {
    result.error = GroundingError::UnsupportedRoleInfoObject;
    return result;
  }
  const CompoundRoleEntry *role =
      FindCompoundRole(southTeam, rpc.roleInfoClass.index);
  if (!role) {
    result.error = GroundingError::UnsupportedRoleInfoObject;
    return result;
  }
  if (rpc.tankSelectionPresent) {
    result.error = GroundingError::UnsupportedTankSelection;
    return result;
  }
  if (!HasGroundedCompoundWeaponSelection(rpc.weaponSelection, *role)) {
    result.error = GroundingError::UnsupportedWeaponSelection;
    return result;
  }
  if (rpc.allowTeamTank || rpc.desiredContext) {
    result.error = GroundingError::UnsupportedSelectionFlags;
    return result;
  }

  result.role.roleInfoObjectRef = role->classRef;
  result.role.classIndex = role->classIndex;
  result.role.roleLimit = role->roleLimit;
  result.role.primaryWeaponIndex = rpc.weaponSelection.primaryWeaponIndex;
  result.role.secondaryWeaponIndex = rpc.weaponSelection.secondaryWeaponIndex;
  // ChangedRole, SquadIndex, and RoleIndex remain at fail-closed defaults:
  // those values depend on runtime occupancy, not cooked role metadata.
  result.error = GroundingError::None;
  return result;
}

void WriteOwnerPriClassIndex(BitWriter &writer, uint8_t classIndex) {
  ActorRepl::WritePropByte(writer, kPriClassIndexHandle,
                           kRoPlayerReplicationInfoMaxHandle, classIndex);
}

void WriteOwnerPriRoleAssignment(BitWriter &writer, uint8_t squadIndex,
                                 uint8_t roleIndex) {
  // Capture f2537 order is h81 SquadIndex followed by h80 RoleIndex.  Do not
  // sort these handles: ReplicatedEvent sequencing is part of the evidence.
  ActorRepl::WritePropByte(writer, kPriSquadIndexHandle,
                           kRoPlayerReplicationInfoMaxHandle, squadIndex);
  ActorRepl::WritePropByte(writer, kPriRoleIndexHandle,
                           kRoPlayerReplicationInfoMaxHandle, roleIndex);
}

std::vector<uint8_t> EncodeChangedRole(uint8_t squadIndex, uint8_t classIndex,
                                       bool showLobby, bool showSpawnSelect,
                                       uint32_t &payloadBits) {
  return EncodeChangedRoleTransition(
      ChangedRoleEvidence{squadIndex, classIndex, showLobby,
                          showSpawnSelect, std::nullopt},
      payloadBits);
}

std::vector<uint8_t>
EncodeChangedSquad(const ChangedSquadEvidence &evidence,
                   uint32_t &payloadBits) {
  BitWriter writer;
  WriteChangedSquad(writer, evidence);
  payloadBits = static_cast<uint32_t>(writer.NumBits());
  return writer.GetBytes();
}

std::vector<uint8_t>
EncodeChangedRoleTransition(const ChangedRoleEvidence &evidence,
                            uint32_t &payloadBits) {
  BitWriter writer;
  writer.SerializeInt(kChangedRoleHandle, kRoPlayerControllerMaxHandle);
  writer.WriteBit(evidence.squadIndex != 0u);
  if (evidence.squadIndex != 0u)
    writer.WriteByte(evidence.squadIndex);
  writer.WriteBit(evidence.classIndex != 0u);
  if (evidence.classIndex != 0u)
    writer.WriteByte(evidence.classIndex);
  writer.WriteBit(evidence.showLobby);
  writer.WriteBit(evidence.showSpawnSelect);

  if (evidence.followingChangedSquad.has_value()) {
    WriteChangedSquad(writer, *evidence.followingChangedSquad);
  }

  payloadBits = static_cast<uint32_t>(writer.NumBits());
  return writer.GetBytes();
}

} // namespace RoleSelectionRepl
