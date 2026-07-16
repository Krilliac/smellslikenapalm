// Capture/source-bounded C2S actor RPC walkers for the fixed owning actor graph.
// Unknown handles stop a walk without guessing their parameter layout.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "Network/ActorReplication.h"
#include "Network/MantleReplication.h"
#include "Network/MovementReplication.h"

namespace GameplayRpc {

constexpr size_t kMaxRpcsPerBunch = 64;
constexpr size_t kMaxRpcPayloadBits = 16384;

inline bool IsValidPayloadExtent(const uint8_t* payload, size_t payloadBytes,
                                 size_t payloadBits) noexcept {
    return payload && payloadBits > 0 &&
           payloadBytes <= std::numeric_limits<size_t>::max() / 8u &&
           payloadBits <= payloadBytes * 8u &&
           payloadBits <= kMaxRpcPayloadBits;
}

enum class PcKind : uint8_t {
    Movement,
    Use,
    UseRelease,
    ShortTimeout,
    SetSpectatorLocation,
    UpdateLevelVisibility,
    AttemptMantle,
    DoSpecialMove,
    EndSpecialMove,
    ResetTeamSwapDelay,
};

constexpr uint32_t kMaxNetworkedHardcodedName = 1250;
constexpr size_t kMaxPcPackageNameCharacters = 1023;

struct SpectatorLocation {
    bool hasLocation = false;
    Vector3 location{};
};

struct NetworkName {
    bool hardcoded = false;
    uint32_t hardcodedIndex = 0;
    std::string text;
    int32_t number = 0;
};

struct LevelVisibility {
    bool hasPackageName = false;
    NetworkName packageName{};
    bool visible = false;
};

struct SpecialMove {
    uint8_t move = 0;
    float joyUp = 0.0f;
    float joyRight = 0.0f;
    int32_t rotYaw = 0;
};

struct PcEvent {
    PcKind kind = PcKind::Movement;
    uint32_t handle = 0;
    MovementRepl::Rpc movement{};
    MantleRepl::Attempt mantle{};
    SpecialMove specialMove{};
    SpectatorLocation spectatorLocation{};
    LevelVisibility levelVisibility{};
};

template <typename Event>
struct WalkResult {
    bool valid = false;       // no truncation/overflow in decoded events
    bool complete = false;    // all payload bits matched evidenced schemas
    bool stoppedOnUnknown = false;
    uint32_t unknownHandle = 0;
    size_t consumedBits = 0;
    std::vector<Event> events;
};

inline WalkResult<PcEvent> DecodePlayerController(
    const uint8_t* payload, size_t payloadBytes, size_t payloadBits) {
    WalkResult<PcEvent> out;
    if (!IsValidPayloadExtent(payload, payloadBytes, payloadBits)) return out;
    BitReader r(payload, payloadBytes, payloadBits);
    out.valid = true;
    while (r.BitsLeft() > 0) {
        if (out.events.size() >= kMaxRpcsPerBunch) {
            out.valid = false;
            break;
        }
        BitReader probe = r;
        const uint32_t handle = probe.SerializeInt(MovementRepl::kRoPcMaxHandle);
        if (probe.IsOverflowed()) { out.valid = false; break; }

        PcEvent event;
        event.handle = handle;
        if (MovementRepl::IsKnownHandle(handle)) {
            if (!MovementRepl::DecodeOne(r, event.movement)) {
                out.valid = false;
                break;
            }
            if (handle == MovementRepl::kResetTeamSwapDelay) {
                event.kind = PcKind::ResetTeamSwapDelay;
            } else {
                event.kind = PcKind::Movement;
            }
        } else if (handle == 37 || handle == 79 ||
                   handle == 278 || handle == 307) {
            r = probe; // these four schemas have no parameters
            event.kind = handle == 37 ? PcKind::ShortTimeout :
                         handle == 79 ? PcKind::Use :
                         handle == 278 ? PcKind::UseRelease : PcKind::EndSpecialMove;
        } else if (handle == 89) {
            // PlayerController.ServerSetSpectatorLocation(Vector). Non-bool
            // parameters carry a presence bit; omission is the zero default.
            BitReader trial = probe;
            event.kind = PcKind::SetSpectatorLocation;
            event.spectatorLocation.hasLocation = trial.ReadBit();
            if (event.spectatorLocation.hasLocation) {
                ActorRepl::ReadCompressedVector(
                    trial,
                    event.spectatorLocation.location.x,
                    event.spectatorLocation.location.y,
                    event.spectatorLocation.location.z);
            }
            const Vector3& location = event.spectatorLocation.location;
            if (trial.IsOverflowed() ||
                !std::isfinite(location.x) ||
                !std::isfinite(location.y) ||
                !std::isfinite(location.z)) {
                out.valid = false;
                break;
            }
            r = trial;
        } else if (handle == 104) {
            // PlayerController.ServerUpdateLevelVisibility(FName,bool).
            // PackageName is a normal non-bool parameter (presence bit), while
            // bIsVisible is serialized directly as one bare bit.
            BitReader trial = probe;
            event.kind = PcKind::UpdateLevelVisibility;
            event.levelVisibility.hasPackageName = trial.ReadBit();
            if (event.levelVisibility.hasPackageName) {
                NetworkName& name = event.levelVisibility.packageName;
                name.hardcoded = trial.ReadBit();
                if (name.hardcoded) {
                    name.hardcodedIndex = trial.SerializeInt(
                        kMaxNetworkedHardcodedName + 1u);
                } else {
                    name.text = trial.ReadString();
                    name.number = trial.ReadInt32();
                }
            }
            event.levelVisibility.visible = trial.ReadBit();
            const NetworkName& name = event.levelVisibility.packageName;
            if (trial.IsOverflowed() ||
                (event.levelVisibility.hasPackageName && !name.hardcoded &&
                 (name.text.size() > kMaxPcPackageNameCharacters ||
                  name.number < 0))) {
                out.valid = false;
                break;
            }
            r = trial;
        } else if (handle == MantleRepl::kServerAttemptMantle) {
            event.mantle = MantleRepl::DecodeServerAttempt(r);
            if (!event.mantle.valid) { out.valid = false; break; }
            event.kind = PcKind::AttemptMantle;
        } else if (handle == 306) {
            // ESpecialMove, PlayerJoyUp, PlayerJoyRight, RotYaw. Non-bool RPC
            // parameters use UE3's default/presence bit before their value.
            r = probe;
            event.kind = PcKind::DoSpecialMove;
            if (r.ReadBit()) {
                event.specialMove.move =
                    static_cast<uint8_t>(r.SerializeInt(MantleRepl::kSpecialMoveMax));
                // SM_MAX is the enum sentinel, never a playable special move.
                if (event.specialMove.move == MantleRepl::kSpecialMoveMax - 1) {
                    out.valid = false;
                    break;
                }
            }
            if (r.ReadBit()) event.specialMove.joyUp = r.ReadFloat();
            if (r.ReadBit()) event.specialMove.joyRight = r.ReadFloat();
            if (r.ReadBit()) event.specialMove.rotYaw = r.ReadInt32();
            if (r.IsOverflowed() ||
                !std::isfinite(event.specialMove.joyUp) ||
                !std::isfinite(event.specialMove.joyRight)) {
                out.valid = false;
                break;
            }
        } else {
            out.stoppedOnUnknown = true;
            out.unknownHandle = handle;
            break;
        }
        out.events.push_back(event);
    }
    out.consumedBits = r.BitPos();
    out.complete = out.valid && !out.stoppedOnUnknown && r.BitsLeft() == 0;
    return out;
}

enum class PawnKind : uint8_t { ForceCrouch, MantleStarted };
struct PawnEvent { PawnKind kind = PawnKind::MantleStarted; uint32_t handle = 0; };

inline WalkResult<PawnEvent> DecodePawn(
    const uint8_t* payload, size_t payloadBytes, size_t payloadBits) {
    constexpr uint32_t kRoPawnMaxHandle = 168; // table ends at h167
    WalkResult<PawnEvent> out;
    if (!IsValidPayloadExtent(payload, payloadBytes, payloadBits)) return out;
    BitReader r(payload, payloadBytes, payloadBits);
    out.valid = true;
    while (r.BitsLeft() > 0) {
        if (out.events.size() >= kMaxRpcsPerBunch) { out.valid = false; break; }
        BitReader probe = r;
        const uint32_t handle = probe.SerializeInt(kRoPawnMaxHandle);
        if (probe.IsOverflowed()) { out.valid = false; break; }
        if (handle != 77 && handle != 85) {
            out.stoppedOnUnknown = true;
            out.unknownHandle = handle;
            break;
        }
        r = probe;
        out.events.push_back({handle == 77 ? PawnKind::ForceCrouch : PawnKind::MantleStarted,
                              handle});
    }
    out.consumedBits = r.BitPos();
    out.complete = out.valid && !out.stoppedOnUnknown && r.BitsLeft() == 0;
    return out;
}

enum class WeaponKind : uint8_t { StartFire, StopFire, RequestReload };
struct WeaponEvent {
    WeaponKind kind = WeaponKind::StartFire;
    uint32_t handle = 0;
    uint8_t fireMode = 0;
};

// ROWeapon ends at h98. The capture-verified M61 subclass adds two fields and
// ends at h100, so its function handles must be serialized against 101. A
// caller selects the bound from the owned actor-channel metadata; never infer
// it from payload bits.
constexpr uint32_t kRoWeaponMaxHandle = 99;
constexpr uint32_t kM61WeaponMaxHandle = 101;
constexpr uint32_t kServerStartFire = 29;
constexpr uint32_t kServerStopFire = 30;
constexpr uint32_t kServerRequestReload = 38;

inline WalkResult<WeaponEvent> DecodeWeapon(
    const uint8_t* payload, size_t payloadBytes, size_t payloadBits,
    uint32_t maxHandle = kRoWeaponMaxHandle) {
    WalkResult<WeaponEvent> out;
    // maxHandle comes from a small, static per-channel table. Retain a hard
    // bound here so a future caller cannot turn malformed input into an
    // unbounded SerializeInt walk.
    if (!IsValidPayloadExtent(payload, payloadBytes, payloadBits) ||
        maxHandle <= kServerRequestReload || maxHandle > 4096u) {
        return out;
    }
    BitReader r(payload, payloadBytes, payloadBits);
    out.valid = true;
    while (r.BitsLeft() > 0) {
        if (out.events.size() >= kMaxRpcsPerBunch) { out.valid = false; break; }
        BitReader probe = r;
        const uint32_t handle = probe.SerializeInt(maxHandle);
        if (probe.IsOverflowed()) { out.valid = false; break; }
        WeaponEvent event;
        event.handle = handle;
        if (handle == kServerStartFire || handle == kServerStopFire) {
            r = probe;
            event.kind = handle == kServerStartFire
                ? WeaponKind::StartFire
                : WeaponKind::StopFire;
            if (r.ReadBit()) event.fireMode = r.ReadByte();
        } else if (handle == kServerRequestReload) {
            r = probe;
            event.kind = WeaponKind::RequestReload;
        } else {
            out.stoppedOnUnknown = true;
            out.unknownHandle = handle;
            break;
        }
        if (r.IsOverflowed()) { out.valid = false; break; }
        out.events.push_back(event);
    }
    out.consumedBits = r.BitPos();
    out.complete = out.valid && !out.stoppedOnUnknown && r.BitsLeft() == 0;
    return out;
}

struct AimDirection {
    bool valid = false;
    Vector3 value{};
};

inline AimDirection NormalizeAimDirection(const Vector3& input) {
    AimDirection result;
    if (!std::isfinite(input.x) || !std::isfinite(input.y) ||
        !std::isfinite(input.z)) {
        return result;
    }
    const float lengthSquared =
        input.x * input.x + input.y * input.y + input.z * input.z;
    if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-8f) {
        return result;
    }
    const float inverseLength = 1.0f / std::sqrt(lengthSquared);
    result.value = input * inverseLength;
    result.valid = std::isfinite(result.value.x) &&
        std::isfinite(result.value.y) && std::isfinite(result.value.z);
    return result;
}

// UE3 ServerMove packs Pitch into the high word and Yaw into the low word of
// View. Each word is a full-turn 16-bit Unreal rotator value.
inline AimDirection DirectionFromPackedView(int32_t packedView) {
    constexpr float kTwoPi = 6.28318530717958647692f;
    constexpr float kRotatorUnits = 65536.0f;
    const uint32_t raw = static_cast<uint32_t>(packedView);
    const uint32_t rawPitch = (raw >> 16u) & 0xFFFFu;
    const uint32_t rawYaw = raw & 0xFFFFu;
    const float signedPitchUnits = rawPitch <= 0x7FFFu
        ? static_cast<float>(rawPitch)
        : static_cast<float>(static_cast<int32_t>(rawPitch) - 65536);
    const float pitch = signedPitchUnits * (kTwoPi / kRotatorUnits);
    const float yaw = static_cast<float>(rawYaw) *
        (kTwoPi / kRotatorUnits);
    const float cosPitch = std::cos(pitch);
    return NormalizeAimDirection({
        cosPitch * std::cos(yaw),
        cosPitch * std::sin(yaw),
        std::sin(pitch),
    });
}

constexpr uint8_t kM61OverhandFireMode = 0;
constexpr uint8_t kM61TossFireMode = 1;
constexpr float kM61UeUnitsPerMeter = 50.0f;
constexpr float kM61FuseSeconds = 4.2f;
constexpr float kType67FuseSeconds = 4.5f;
constexpr float kM61MinimumFuseSeconds = 0.05f;
constexpr float kM61MaximumCookSeconds =
    kM61FuseSeconds - kM61MinimumFuseSeconds;
constexpr float kType67MaximumCookSeconds =
    kType67FuseSeconds - kM61MinimumFuseSeconds;
constexpr float kM61FullStrengthCookSeconds = 1.0f;
constexpr float kM61OverhandMinSpeedUu = 650.0f;
constexpr float kM61OverhandMaxSpeedUu = 1200.0f;
constexpr float kM61TossMinSpeedUu = 400.0f;
constexpr float kM61TossMaxSpeedUu = 600.0f;
constexpr float kM61TossZUu = 150.0f;

struct M61ThrowParameters {
    bool valid = false;
    uint8_t fireMode = 0;
    float cookSeconds = 0.0f;
    float fuseSeconds = kM61FuseSeconds;
    float baseSpeedUuPerSecond = 0.0f;
    float tossZUuPerSecond = 0.0f;
    Vector3 launchDirection{};
    float launchSpeedUuPerSecond = 0.0f;
    float launchSpeedMetersPerSecond = 0.0f;
};

// h29 begins the server-timed cook and h30 releases it. No separate client
// strength parameter has been verified: interpolate the source-bounded speed
// range over the first second of authoritative hold time, while the full hold
// continues to consume the 4.2-second fuse. Toss mode adds the source TossZ
// before normalizing for CombatAuthority's direction/speed API.
inline M61ThrowParameters BuildGrenadeThrowParameters(
    const Vector3& aimDirection, uint8_t fireMode, double elapsedCookSeconds,
    float fullFuseSeconds) {
    M61ThrowParameters result;
    const AimDirection aim = NormalizeAimDirection(aimDirection);
    if (!aim.valid || !std::isfinite(elapsedCookSeconds) ||
        !std::isfinite(fullFuseSeconds) ||
        fullFuseSeconds <= kM61MinimumFuseSeconds ||
        (fireMode != kM61OverhandFireMode &&
         fireMode != kM61TossFireMode)) {
        return result;
    }

    const float maximumCookSeconds =
        fullFuseSeconds - kM61MinimumFuseSeconds;
    result.fireMode = fireMode;
    result.cookSeconds = std::clamp(
        static_cast<float>(elapsedCookSeconds), 0.0f,
        maximumCookSeconds);
    result.fuseSeconds = std::max(
        kM61MinimumFuseSeconds, fullFuseSeconds - result.cookSeconds);
    const float strength = std::clamp(
        result.cookSeconds / kM61FullStrengthCookSeconds, 0.0f, 1.0f);
    if (fireMode == kM61OverhandFireMode) {
        result.baseSpeedUuPerSecond = kM61OverhandMinSpeedUu +
            (kM61OverhandMaxSpeedUu - kM61OverhandMinSpeedUu) * strength;
    } else {
        result.baseSpeedUuPerSecond = kM61TossMinSpeedUu +
            (kM61TossMaxSpeedUu - kM61TossMinSpeedUu) * strength;
        result.tossZUuPerSecond = kM61TossZUu;
    }

    Vector3 velocity = aim.value * result.baseSpeedUuPerSecond;
    velocity.z += result.tossZUuPerSecond;
    const AimDirection normalizedVelocity = NormalizeAimDirection(velocity);
    if (!normalizedVelocity.valid) return result;

    result.launchSpeedUuPerSecond = velocity.Length();
    if (!std::isfinite(result.launchSpeedUuPerSecond) ||
        result.launchSpeedUuPerSecond <= 0.0f) {
        return result;
    }
    result.launchDirection = normalizedVelocity.value;
    result.launchSpeedMetersPerSecond =
        result.launchSpeedUuPerSecond / kM61UeUnitsPerMeter;
    result.valid = std::isfinite(result.launchSpeedMetersPerSecond) &&
        result.launchSpeedMetersPerSecond > 0.0f;
    return result;
}

inline M61ThrowParameters BuildM61ThrowParameters(
    const Vector3& aimDirection, uint8_t fireMode, double elapsedCookSeconds) {
    return BuildGrenadeThrowParameters(
        aimDirection, fireMode, elapsedCookSeconds, kM61FuseSeconds);
}

inline M61ThrowParameters BuildType67ThrowParameters(
    const Vector3& aimDirection, uint8_t fireMode, double elapsedCookSeconds) {
    return BuildGrenadeThrowParameters(
        aimDirection, fireMode, elapsedCookSeconds, kType67FuseSeconds);
}

struct InventoryEvent {
    uint32_t handle = 0;
    bool hasDesiredWeapon = false;
    ActorRepl::NetGUIDRef desiredWeapon{};
};

inline WalkResult<InventoryEvent> DecodeInventoryManager(
    const uint8_t* payload, size_t payloadBytes, size_t payloadBits) {
    // Cooked ROInventoryManager chain: 34 fields, h25 inherited
    // InventoryManager.ServerSetCurrentWeapon(Weapon DesiredWeapon).
    constexpr uint32_t kInventoryManagerMaxHandle = 34;
    constexpr uint32_t kServerSetCurrentWeapon = 25;
    WalkResult<InventoryEvent> out;
    if (!IsValidPayloadExtent(payload, payloadBytes, payloadBits)) return out;
    BitReader r(payload, payloadBytes, payloadBits);
    out.valid = true;
    while (r.BitsLeft() > 0) {
        if (out.events.size() >= kMaxRpcsPerBunch) { out.valid = false; break; }
        BitReader probe = r;
        const uint32_t handle = probe.SerializeInt(kInventoryManagerMaxHandle);
        if (probe.IsOverflowed()) { out.valid = false; break; }
        if (handle != kServerSetCurrentWeapon) {
            out.stoppedOnUnknown = true;
            out.unknownHandle = handle;
            break;
        }
        r = probe;
        InventoryEvent event;
        event.handle = handle;
        event.hasDesiredWeapon = r.ReadBit();
        if (event.hasDesiredWeapon) {
            event.desiredWeapon = ActorRepl::ReadNetGUID(r);
        }
        if (r.IsOverflowed()) { out.valid = false; break; }
        out.events.push_back(event);
    }
    out.consumedBits = r.BitPos();
    out.complete = out.valid && !out.stoppedOnUnknown && r.BitsLeft() == 0;
    return out;
}

} // namespace GameplayRpc
