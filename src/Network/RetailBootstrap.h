// Map/mode-aware retail bootstrap parsing and reconstruction.
//
// The captured stream contains opaque, session-specific control bytes around a
// structured UE3 PackageMap and NMT_Welcome.  This API deliberately changes only
// the package GUID/name and the two Welcome FStrings; every other captured byte
// is retained verbatim.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace RetailBootstrap {

using GuidBytes = std::array<uint8_t, 16>;

// Legacy role-registry grounding token recovered from the captured Resort
// stream.  This is intentionally NOT the ROGame package's actual ObjectBase:
// the canonical package base is 39478, while the role decoder was grounded
// against this historical +1 token.  Do not substitute a package-layout base
// here without independently migrating and re-grounding the role registry.
constexpr uint32_t kCapturedRoGameObjectBase = 39479;

struct Profile {
    std::string mapUrl;
    std::string modeName;
    std::string gameClassPath;
    uint32_t gameClassIndex = 0;
    GuidBytes mapPackageGuid{}; // raw UE3 FGuid bytes: four little-endian uint32s
    // Zero means the ROGame PackageMap base has not been grounded for this
    // profile.  Role-object authorization must reject a zero/unknown base.
    uint32_t roGameObjectBase = 0;
    bool experimental = false;
    bool usedFallback = false;
};

// One complete control-channel payload per entry. The on-disk encoding is a
// repeated [uint32 little-endian payload length][payload bytes] stream.
struct Document {
    std::vector<std::vector<uint8_t>> records;
};

// Source-grounded ROGame static-reference layout paired with one PackageMap
// artifact.  These values are package-layout data, not interchangeable role
// authorization tokens.  The actor-open entries below are Default__ actor/CDO
// (archetype) refs; the GameInfo entries are UClass refs.  The installed package
// has grounded actor and GameInfo refs, but its role CDO registry and the
// complete 139-record captured world have not been migrated and therefore
// remain fail-closed.
struct RoGameLayout {
    uint32_t actualObjectBase = 0;
    uint32_t playerControllerClassRef = 0;
    uint32_t playerReplicationInfoClassRef = 0;
    uint32_t gameReplicationInfoClassRef = 0;
    uint32_t teamInfoClassRef = 0;
    uint32_t territoriesGameClassRef = 0;
    uint32_t skirmishGameClassRef = 0;
    uint32_t supremacyGameClassRef = 0;
    bool roleRegistryGrounded = false;
    bool capturedWorldReplayGrounded = false;
};

// Static-reference metadata needed by the capture-backed owning-pawn graph.
// Its weapon/pawn actor opens reference ROGameContent exports, whose installed
// layout is shifted from the canonical capture. ROInventoryManager belongs to
// a different package, so its actor-open class reference is pinned explicitly
// instead of applying the ROGameContent offset to it.
struct OwningPawnPackageMapLayout {
    uint32_t roGameContentObjectRefOffset = 0;
    uint32_t inventoryManagerArchetypeRef = 0;
};

// Process-start artifact policy for the captured replication cohort. The
// canonical PackageMap and actor captures remain the default; the installed
// PackageMap plus source-grounded actor-reference rebase are reachable only
// through one exact opt-in value. String views always refer to static literals.
struct ArtifactSelection {
    std::string_view variant;
    std::string_view path;
    RoGameLayout roGame;
    OwningPawnPackageMapLayout owningPawn;
    // Authored map-actor refs are canonical. Installed PackageMaps can insert
    // exports before the map package, so wire refs are rebased per client.
    uint32_t mapObjectRefOffset = 0;
};

constexpr std::string_view kArtifactVariantEnvironment =
    "RS2V_REPLICATION_BOOTSTRAP_VARIANT";

// Empty means the environment variable is absent or explicitly empty. Every
// other unsupported value fails closed instead of silently replaying canonical
// package identities after an operator typo.
std::optional<ArtifactSelection> ResolveArtifactSelection(
    std::string_view requestedVariant, std::string& error);

// Pin the complete on-disk PackageMap/control artifact to the resolved variant.
// This prevents a valid-but-wrong canonical stream from being paired with the
// installed ROGame object layout (or vice versa). Unsupported variants, size
// drift, unavailable SHA-256 support, and identity mismatches fail closed.
bool ValidateReplicationArtifact(const std::vector<uint8_t>& bytes,
                                 std::string_view variant,
                                 std::string& error);

// Resolve a GameInfo class through the selected package layout.  Unknown paths
// fail closed rather than falling back to a canonical numeric reference.
std::optional<uint32_t> ResolveGameClassRef(
    const ArtifactSelection& selection, std::string_view gameClassPath);

// Convert a nonzero canonical ROGameContent static reference to the exact wire
// reference carried by the selected PackageMap. None, values outside UE3's
// static-object range, and range overflow fail closed. ROInventoryManager is
// intentionally not resolved through this
// helper because it belongs to a separately shifted package; use
// selection.owningPawn.inventoryManagerArchetypeRef for that actor open.
std::optional<uint32_t> ResolveRoGameContentWireRef(
    const ArtifactSelection& selection, uint32_t canonicalRef) noexcept;

// Deterministically derive the actor-open stream paired with a resolved
// replication artifact. "canonical" is byte-identical. "installed" applies
// only the source-grounded static-reference bit changes for the currently
// installed ROGame package and verifies both the source and result SHA-256.
// Any other variant, wrong source capture, or unavailable SHA-256 support
// fails closed and leaves out empty.
bool BuildActorArtifactVariant(const std::vector<uint8_t>& canonical,
                               std::string_view variant,
                               std::vector<uint8_t>& out,
                               std::string& error);

// Validate the complete actor-artifact descriptor stream:
// [u16 channel][u8 type][u8 flags][u16 sequence][u32 payload bits][payload].
// A truncated payload, partial trailing header, or empty stream fails closed.
bool ValidateActorArtifactFraming(const std::vector<uint8_t>& bytes,
                                  std::string& error);

struct PackageRecord {
    size_t outerRecord = 0;
    size_t begin = 0;
    size_t end = 0;
    size_t nameFieldBegin = 0;
    size_t nameFieldEnd = 0;
    GuidBytes guid{};
    std::string name;
    std::string extension;
    uint32_t packageFlags = 0;
    int32_t generation = 0;
};

struct WelcomeRecord {
    size_t outerRecord = 0;
    size_t begin = 0;
    size_t mapFieldBegin = 0;
    size_t mapFieldEnd = 0;
    size_t gameFieldBegin = 0;
    size_t gameFieldEnd = 0;
    std::string mapUrl;
    std::string gameClassPath;
};

// Resort is the byte-for-byte canonical capture and remains the fallback for
// maps for which no bounded profile has been verified.
Profile CanonicalProfile();

// Resolve the package identity from mapUrl and the GameInfo identity from the
// effective mode. Unknown maps return CanonicalProfile() with usedFallback=true.
Profile ResolveProfile(std::string_view mapUrl, std::string_view effectiveMode);

// Resolve only source-grounded bootstrap profiles. Runtime map travel/rotation
// must use this API so an unsupported map can never inherit Resort's package
// identity. ResolveProfile() intentionally retains that fallback for legacy
// initial connections.
std::optional<Profile> ResolveExactProfile(std::string_view mapUrl,
                                           std::string_view effectiveMode);
bool HasExactProfile(std::string_view mapUrl);

// Resolve only a cooked map package identity. Unlike ResolveProfile(), this
// never substitutes Resort for an unknown map: callers such as ClientTravel
// must send the optional all-zero Guid rather than claim an unrelated package.
std::optional<GuidBytes> ResolveMapPackageGuid(std::string_view mapUrl);

bool Parse(const std::vector<uint8_t>& bytes, Document& out, std::string& error);
std::vector<uint8_t> Serialize(const Document& document);

bool FindPackage(const Document& document,
                 std::string_view packageName,
                 PackageRecord& out,
                 std::string& error);

bool FindWelcome(const Document& document,
                 std::string_view mapUrl,
                 std::string_view gameClassPath,
                 WelcomeRecord& out,
                 std::string& error);

// Rebuild a variant from the canonical Resort capture. No absolute offsets are
// used: the source package and Welcome are structurally decoded first, then the
// two edits are applied from the end of each affected payload.
bool BuildVariant(const Document& canonical,
                  const Profile& profile,
                  Document& out,
                  std::string& error);

} // namespace RetailBootstrap
