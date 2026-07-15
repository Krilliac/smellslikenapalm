// Capture-bounded ROPlayerController.SelectRoleByClass replication.
//
// The wire decoder is deliberately syntactic: SelectRoleByClass carries a
// nonzero static UClass reference, and map/team/role authorization happens in
// the grounding helpers below. Unknown maps, modes, artifact variants,
// PackageMap bases, teams, role classes, weapon choices and vehicle selections
// fail closed rather than being inferred from numeric proximity.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "Network/ActorReplication.h"
#include "Network/BitReader.h"

class BitWriter;

namespace RoleSelectionRepl {

constexpr uint32_t kRoPlayerControllerMaxHandle = 531;
constexpr uint32_t kSelectRoleByClassHandle = 175;
constexpr uint32_t kServerSetReadyToSpawnHandle = 434;
constexpr uint32_t kServerResetSpectateModeHandle = 182;
constexpr uint32_t kServerAutoSelectSquadHandle = 451;
constexpr uint32_t kServerSetSpectatorLocationHandle = 89;
constexpr uint32_t kChangedRoleHandle = 210;
constexpr uint32_t kChangedSquadHandle = 211;
constexpr uint32_t kMaximumRoleSelectionBunchBits = 256;

// Legacy Resort role-registry references recovered from captures grounded with
// the historical 39479 token. Keep these numeric values stable until that
// registry is independently migrated to an actual PackageMap layout.
constexpr uint32_t kResortSouthGruntRoleInfoObjectRef = 87492;
constexpr uint32_t kResortNorthRiflemanRoleInfoObjectRef = 87396;

// Legacy Cu Chi Territories first-round class-0 references under the same
// historical grounding token. They are retained for compatibility rather than
// reinterpreted against the installed package layout.
constexpr uint32_t kCuChiSouthGruntRoleInfoObjectRef = 87490;
constexpr uint32_t kCuChiNorthGuerillaRoleInfoObjectRef = 87398;

// Installed ROGame.u, actual ObjectBase 39478. VNSK-Compound's Skirmish role
// registry carries UClass references in the h175 RoleInfoClass parameter.
constexpr uint32_t kInstalledRoGameObjectBase = 39478;
constexpr std::string_view kInstalledArtifactVariant = "installed";
constexpr uint8_t kCompoundUsServerTeam = 1;
constexpr uint8_t kCompoundNlfServerTeam = 2;

constexpr uint32_t kCompoundNorthGuerillaRoleClassRef = 87401;
constexpr uint32_t kCompoundNorthScoutRoleClassRef = 87439;
constexpr uint32_t kCompoundNorthMachineGunnerRoleClassRef = 87409;
constexpr uint32_t kCompoundNorthSniperRoleClassRef = 87449;
constexpr uint32_t kCompoundNorthSapperRoleClassRef = 87431;
constexpr uint32_t kCompoundSouthGruntRoleClassRef = 87493;
constexpr uint32_t kCompoundSouthPointmanRoleClassRef = 87541;
constexpr uint32_t kCompoundSouthMachineGunnerRoleClassRef = 87499;
constexpr uint32_t kCompoundSouthMarksmanRoleClassRef = 87523;
constexpr uint32_t kCompoundSouthEngineerRoleClassRef = 87465;

constexpr uint8_t kCompoundRiflemanClassIndex = 0;
constexpr uint8_t kCompoundPointmanClassIndex = 1;
constexpr uint8_t kCompoundMachineGunnerClassIndex = 2;
constexpr uint8_t kCompoundMarksmanClassIndex = 3;
constexpr uint8_t kCompoundEngineerClassIndex = 4;
constexpr uint8_t kCompoundRiflemanLimit = 255;
constexpr uint8_t kCompoundPointmanLimit = 2;
constexpr uint8_t kCompoundMachineGunnerLimit = 2;
constexpr uint8_t kCompoundMarksmanLimit = 1;
constexpr uint8_t kCompoundEngineerLimit = 2;

constexpr uint32_t kRoPlayerReplicationInfoMaxHandle = 98;
constexpr uint32_t kPriSpawnSelectionHandle = 77;
constexpr uint32_t kPriClassIndexHandle = 79;
constexpr uint32_t kPriRoleIndexHandle = 80;
constexpr uint32_t kPriSquadIndexHandle = 81;

constexpr uint8_t kResortUsServerTeam = 1;
constexpr uint8_t kResortNvaServerTeam = 2;
constexpr uint8_t kResortSouthGruntClassIndex = 0;
constexpr uint8_t kResortSouthGruntChangedRoleSquadIndex = 255;
constexpr uint8_t kResortSouthGruntSquadIndex = 8;
constexpr uint8_t kResortSouthGruntRoleIndex = 5;
constexpr uint8_t kResortNvaRiflemanClassIndex = 0;
constexpr uint8_t kResortNvaRiflemanChangedRoleSquadIndex = 255;
constexpr uint8_t kResortNvaRiflemanSquadIndex = 2;
constexpr uint8_t kResortNvaRiflemanRoleIndex = 3;
constexpr uint8_t kCuChiUsServerTeam = 1;
constexpr uint8_t kCuChiNlfServerTeam = 2;
constexpr uint8_t kCuChiInfantryClassIndex = 0;

struct WeaponSelectionInfo {
  bool presentOnWire = false;
  uint8_t primaryWeaponIndex = 0;
  uint8_t primaryWeaponLevel = 0;
  uint8_t primaryWeaponAmmo = 0;
  uint8_t secondaryWeaponIndex = 0;
  uint8_t secondaryWeaponLevel = 0;
};

struct SelectRoleByClass {
  bool southDesired = false;
  // A decoded value is the static UClass reference carried by the UE script
  // RoleInfoClass parameter. Semantic authorization is map/layout-specific.
  ActorRepl::NetGUIDRef roleInfoClass{};
  WeaponSelectionInfo weaponSelection{};
  bool tankSelectionPresent = false;
  ActorRepl::NetGUIDRef tankSelection{};
  bool allowTeamTank = false;
  bool desiredContext = false;
  bool closeMenu = false;
  size_t consumedBits = 0;
};

enum class FollowingRpcPattern : uint8_t {
  None,
  InterimReadyOnly,
  InterimReadyAndResetSpectate,
  FinalAutoSelectSquad,
  FinalAutoSelectSquadAndDefaultSpectatorLocation,
};

enum class DecodeError : uint8_t {
  None,
  InvalidBuffer,
  Oversized,
  Truncated,
  UnsupportedHandle,
  MissingRoleInfoClass,
  DynamicRoleInfoClass,
  ZeroRoleInfoClass,
  // Retained for source compatibility. Nonzero static refs are now decoded
  // syntactically and rejected, when necessary, by a grounding helper.
  UnknownRoleInfoObject,
  UnsupportedTankSelection,
  InvalidReadyStatus,
  UnsupportedFollowingRpc,
  TrailingBits,
};

struct DecodeResult {
  DecodeError error = DecodeError::InvalidBuffer;
  SelectRoleByClass rpc{};
  FollowingRpcPattern following = FollowingRpcPattern::None;

  bool valid() const noexcept { return error == DecodeError::None; }
};

// Decode exactly one h175 from the reader's current position.  Failure leaves
// both reader and output unchanged, including on a truncated object reference.
bool DecodeOne(BitReader &reader, SelectRoleByClass &output,
               DecodeError &error);

// Decode a complete reliable ch2 payload. In addition to a lone h175, the
// structurally complete observed tails are h434(status=2),
// h434(status=2)+h182, h451, and h451+h89(default/absent Vector).
DecodeResult DecodeRoleSelectionBunch(const uint8_t *payload,
                                      size_t payloadBytes, size_t payloadBits);

enum class GroundingError : uint8_t {
  None,
  UnsupportedMap,
  UnsupportedTeam,
  TeamIntentMismatch,
  UnsupportedRoleInfoObject,
  UnsupportedTankSelection,
  UnsupportedWeaponSelection,
  UnsupportedSelectionFlags,
  UnsupportedMode,
  UnsupportedArtifactVariant,
  UnsupportedPackageMapBase,
};

struct ChangedSquadEvidence {
  uint8_t squadIndex = 255;
  uint8_t roleIndex = 255;
};

// A post-selection h210 tuple is usable only when all four fields were observed
// together. Resort North frame 61989 additionally carries h211 in the same
// reliable ch2 bunch; keeping that tail attached to the evidence prevents a
// caller from accidentally emitting only half of the captured transition.
struct ChangedRoleEvidence {
  uint8_t squadIndex = 255;
  uint8_t classIndex = 255;
  bool showLobby = false;
  bool showSpawnSelect = false;
  std::optional<ChangedSquadEvidence> followingChangedSquad;
};

struct GroundedRoleSelection {
  // Compatibility name used by existing callers; h175 supplies a UClass ref.
  uint32_t roleInfoObjectRef = 0;
  uint8_t classIndex = 255;
  uint8_t roleLimit = 0;
  std::optional<ChangedRoleEvidence> changedRole;
  uint8_t squadIndex = 255;
  uint8_t roleIndex = 255;
  uint8_t primaryWeaponIndex = 0;
  uint8_t secondaryWeaponIndex = 0;
};

struct GroundingResult {
  GroundingError error = GroundingError::UnsupportedMap;
  GroundedRoleSelection role{};

  bool valid() const noexcept { return error == GroundingError::None; }
};

// Bind the decoded legacy role-class reference to the capture-evidenced Resort
// team, controller transition and PRI state. `serverTeamId` uses the emulator's
// 1=US, 2=NVA convention.
GroundingResult ResolveGroundedResortInfantry(const SelectRoleByClass &rpc,
                                              std::string_view mapUrl,
                                              uint32_t serverTeamId);

// Bind only Cu Chi's source-grounded first-round Territories class-0 role refs.
// This intentionally does not manufacture ChangedRole or PRI squad/role values:
// those are live occupancy results, not cooked-map metadata.  The caller must
// keep the selection fail-closed until it can allocate them from exact runtime
// squad state.
GroundingResult ResolveGroundedCuChiInfantry(
    const SelectRoleByClass &rpc, std::string_view mapUrl,
    std::string_view modeName, uint32_t roGameObjectBase,
    uint32_t serverTeamId);

// Bind the installed VNSK-Compound Skirmish h175 request to its exact
// source-grounded team registry. Class index and role limit are deterministic
// cooked-map metadata; squad/role occupancy and ChangedRole are intentionally
// left unmanufactured.
GroundingResult ResolveGroundedCompoundRole(
    const SelectRoleByClass &rpc, std::string_view mapUrl,
    std::string_view modeName, std::string_view artifactVariant,
    uint32_t roGameObjectBase, uint32_t serverTeamId);

// Pure wire writers used by ConnectionManager and unit tests.  They append
// only the requested property/RPC payload; packet/channel ownership remains
// with the caller.
void WriteOwnerPriClassIndex(BitWriter &writer, uint8_t classIndex);
void WriteOwnerPriRoleAssignment(BitWriter &writer, uint8_t squadIndex,
                                 uint8_t roleIndex);
std::vector<uint8_t> EncodeChangedRole(uint8_t squadIndex, uint8_t classIndex,
                                       bool showLobby, bool showSpawnSelect,
                                       uint32_t &payloadBits);
std::vector<uint8_t>
EncodeChangedSquad(const ChangedSquadEvidence &evidence,
                   uint32_t &payloadBits);
std::vector<uint8_t>
EncodeChangedRoleTransition(const ChangedRoleEvidence &evidence,
                            uint32_t &payloadBits);

} // namespace RoleSelectionRepl
