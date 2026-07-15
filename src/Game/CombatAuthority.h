// Pure, deterministic combat authority for retail RPC integration.
//
// Positions cross this API in Unreal units (UU).  Distances, weapon ranges,
// velocities, gravity and explosion radii are explicit SI values and are
// converted through AuthorityConfig::ueUnitsPerMeter.  This module owns no
// sockets, actor channels, Player objects or retail projectile replication.

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "Game/ParticipantRoster.h"
#include "Math/Vector3.h"

namespace CombatAuthority {

using ParticipantId = uint32_t;
using ProjectileId = uint64_t;
constexpr uint32_t kNoTeam = std::numeric_limits<uint32_t>::max();
// Keeps malformed or accidentally huge projectile specs from turning a single
// server tick into an unbounded collision loop. Retail M61 uses five impacts.
constexpr uint32_t kMaxProjectileGroundImpacts = 64;
// Bounds the configured worst-case fixed-step work for one projectile in one
// authority Advance(). Ground-contact retries are separately bounded by
// kMaxProjectileGroundImpacts.
constexpr size_t kMaxProjectileSubstepsPerAdvance = 4096;
// This is a hard allocation/work ceiling. AuthorityConfig may select a smaller
// per-session cap, but never a larger one.
constexpr size_t kHardMaxActiveProjectiles = 4096;
// A retail server currently caps bots well below this value. Keep the pure
// authority boundary bounded anyway so an accidental or hostile adapter cannot
// turn one h56 report into an unbounded capsule sweep.
constexpr size_t kMaxExternalHitscanTargets = 128;

enum class HitZone : uint8_t {
    Head,
    UpperTorso,
    LowerTorso,
    Legs,
};

enum class EventKind : uint8_t {
    WeaponFired,
    ReloadStarted,
    ReloadCompleted,
    DamageApplied,
    FriendlyFireBlocked,
    ParticipantDied,
    ProjectileSpawned,
    ProjectileDetonated,
    ProjectileRemoved,
};

enum class RejectReason : uint8_t {
    None,
    InvalidConfiguration,
    InvalidArgument,
    UnknownParticipant,
    ParticipantDead,
    NoWeapon,
    Reloading,
    EmptyMagazine,
    Cadence,
    OriginMismatch,
    ZeroLengthSegment,
    OutOfRange,
    UnknownTarget,
    TargetDead,
    TargetMismatch,
    NoValidatedHit,
    ReportedImpactMismatch,
    FriendlyFireBlocked,
    ResourceExhausted,
};

enum class ActionOutcome : uint8_t {
    Rejected,
    Accepted,
    AcceptedMiss,
    AcceptedHit,
};

struct CombatEvent {
    EventKind kind = EventKind::WeaponFired;
    ParticipantId sourceId = 0;
    ParticipantId targetId = 0;
    // Captured when the action/projectile is created so delayed damage keeps
    // its original relationship even if the shooter disconnects or changes
    // team before impact.
    uint32_t sourceTeam = kNoTeam;
    ProjectileId projectileId = 0;
    std::string weaponId;
    Vector3 positionUu{};
    HitZone hitZone = HitZone::UpperTorso;
    float damage = 0.0f;
    float healthAfter = 0.0f;
    uint32_t magazineAmmo = 0;
    uint32_t reserveAmmo = 0;
    // Populated on ProjectileDetonated. Consumers that model participants
    // outside this authority (for example headless bots) can apply the same
    // server-owned radial falloff without weapon-name magic constants.
    float explosionRadiusMeters = 0.0f;
    float maxExplosionDamage = 0.0f;
    bool friendlyFire = false;
};

struct ActionResult {
    ActionOutcome outcome = ActionOutcome::Rejected;
    RejectReason reason = RejectReason::None;
    std::vector<CombatEvent> events;

    bool accepted() const { return outcome != ActionOutcome::Rejected; }
    bool confirmedHit() const { return outcome == ActionOutcome::AcceptedHit; }
};

struct AdvanceResult {
    bool advanced = false;
    RejectReason reason = RejectReason::None;
    std::vector<CombatEvent> events;
};

struct AuthorityConfig {
    float ueUnitsPerMeter = 50.0f;
    float originToleranceMeters = 2.0f;
    float reportedImpactToleranceMeters = 0.75f;
    float projectileMaxStepSeconds = 0.02f;
    float maxAdvanceSeconds = 1.0f;
    size_t maxActiveProjectiles = 1024;

    bool friendlyFireEnabled = false;
    float friendlyFireDamageScale = 0.5f;

    // Zone is derived from normalized height in the target capsule.
    float headMinNormalizedHeight = 0.65f;
    float upperTorsoMinNormalizedHeight = 0.0f;
    float lowerTorsoMinNormalizedHeight = -0.45f;
    float headDamageMultiplier = 2.0f;
    float upperTorsoDamageMultiplier = 1.0f;
    float lowerTorsoDamageMultiplier = 0.9f;
    float legsDamageMultiplier = 0.7f;
};

struct HitVolume {
    // Capsule is vertical and centered on ParticipantSpec::positionUu.
    float radiusMeters = 0.4f;
    float halfHeightMeters = 0.9f;
};

struct ParticipantSpec {
    ParticipantId id = 0;
    uint32_t team = kNoTeam;
    Vector3 positionUu{};
    float maxHealth = 100.0f;
    float health = 100.0f;
    HitVolume hitVolume{};
};

struct WeaponProfile {
    std::string id;
    uint32_t magazineCapacity = 1;
    uint32_t initialMagazine = 1;
    uint32_t initialReserve = 0;
    float roundsPerMinute = 60.0f;
    float reloadSeconds = 1.0f;
    float maxRangeMeters = 100.0f;
    float baseDamage = 25.0f;
    float minDamageFractionAtMaxRange = 1.0f;
};

struct WeaponSnapshot {
    std::string id;
    uint32_t magazineAmmo = 0;
    uint32_t reserveAmmo = 0;
    bool reloading = false;
    double reloadCompletesAtSeconds = 0.0;
};

struct ParticipantSnapshot {
    ParticipantId id = 0;
    uint32_t team = kNoTeam;
    Vector3 positionUu{};
    float maxHealth = 0.0f;
    float health = 0.0f;
    bool alive = false;
    uint32_t kills = 0;
    uint32_t deaths = 0;
    HitVolume hitVolume{};
    // Backwards-compatible view of the active weapon.
    std::optional<WeaponSnapshot> weapon;
    std::string activeWeaponId;
    // Copy-only inventory view; mutating a snapshot cannot affect authority.
    std::map<std::string, WeaponSnapshot> weapons;
};

struct HitscanRequest {
    ParticipantId shooterId = 0;
    std::optional<ParticipantId> claimedTargetId;
    Vector3 startTraceUu{};
    Vector3 endTraceUu{};
    Vector3 reportedImpactUu{};
    // Empty selects the participant's active weapon.
    std::string weaponId;
};

// Value-only bridge for participants whose health/lifecycle is owned by a
// different authoritative subsystem. The tagged ID is deliberate: Bot(7) must
// never enter the uint32 human namespace as participant 7.
struct ExternalParticipantSpec {
    ::ParticipantId id{};
    uint32_t team = kNoTeam;
    Vector3 positionUu{};
    HitVolume hitVolume{};
    bool alive = false;
};

enum class ExternalHitRelationship : uint8_t {
    Hostile,
    FriendlyFireBlocked,
    FriendlyFireAuthorized,
};

struct ExternalHitscanRequest {
    ParticipantId shooterId = 0;
    ::ParticipantId claimedTargetId{};
    Vector3 startTraceUu{};
    Vector3 endTraceUu{};
    Vector3 reportedImpactUu{};
    // Empty selects the participant's active weapon.
    std::string weaponId;
    // Must contain every external bot that can occlude this trace, including
    // dead bots so a claimed dead target is distinguished from an unknown ID.
    std::vector<ExternalParticipantSpec> targets;
};

struct ExternalHitResult {
    ::ParticipantId participantId{};
    uint32_t targetTeam = kNoTeam;
    Vector3 positionUu{};
    HitZone hitZone = HitZone::UpperTorso;
    // Final authoritative damage after range, zone, and (when enabled)
    // friendly-fire scaling. Zero for a friendly-fire-blocked hit.
    float damage = 0.0f;
    ExternalHitRelationship relationship = ExternalHitRelationship::Hostile;
};

struct ExternalHitscanResult {
    ActionResult action;
    // CombatAuthority never mutates external health. The owning subsystem may
    // apply this result exactly once after mapping the explicit relationship.
    std::optional<ExternalHitResult> hit;
};

// Typed inverse bridge for damage produced by a server-owned headless bot.
// This API is intentionally not a weapon/client RPC: BotManager owns bot
// perception and cadence, while CombatAuthority remains the sole owner of the
// connected human's health/death transition. The tagged source prevents
// Bot(7) from being credited or validated as Human(7).
struct ExternalDamageRequest {
    ::ParticipantId sourceId{};
    uint32_t sourceTeam = kNoTeam;
    ParticipantId targetId = 0;
    std::string weaponId;
    Vector3 originUu{};
    Vector3 impactUu{};
    float maxRangeUu = 0.0f;
    float damage = 0.0f;
    HitZone hitZone = HitZone::UpperTorso;
};

struct ExternalDamageResult {
    bool applied = false;
    RejectReason reason = RejectReason::None;
    ::ParticipantId sourceId{};
    ParticipantId targetId = 0;
    uint32_t sourceTeam = kNoTeam;
    float damage = 0.0f;
    float healthAfter = 0.0f;
    bool killed = false;
    bool friendlyFire = false;
};

struct ProjectileSpec {
    float launchSpeedMetersPerSecond = 1.0f;
    Vector3 gravityMetersPerSecondSquared{0.0f, 0.0f, -9.81f};
    float fuseSeconds = 0.0f;          // zero disables the fuse
    float maxLifetimeSeconds = 10.0f;
    float collisionRadiusMeters = 0.05f;
    float directDamage = 0.0f;
    float explosionRadiusMeters = 0.0f;
    float maxExplosionDamage = 0.0f;

    // No implicit world plane: every spawn must supply a finite map/trace-derived
    // ground height, even for projectiles that do not detonate on ground.
    float groundHeightUu = std::numeric_limits<float>::quiet_NaN();
    bool detonateOnGround = true;
    bool detonateOnParticipant = true;
    float ownerCollisionGraceSeconds = 0.15f;

    // A value of zero preserves the legacy non-impact projectile behavior:
    // the first ground contact settles it. For a positive budget, each contact
    // consumes one impact before deciding whether to rebound. This mirrors
    // ROStickGrenadeProjectile, where M61 Bounces=5 yields four reflected
    // rebounds and settles on the fifth impact.
    uint32_t maxGroundImpacts = 0;
    float groundBounceDamping = 0.0f;
    // Applied to Z after the whole reflected velocity is multiplied by
    // groundBounceDamping (M61 source applies an additional 0.85 to Z).
    float groundBounceVerticalDamping = 1.0f;
};

struct ProjectileRequest {
    ParticipantId shooterId = 0;
    Vector3 originUu{};
    Vector3 direction{};
    ProjectileSpec projectile{};
    // Empty selects the participant's active weapon.
    std::string weaponId;
};

struct ProjectileSnapshot {
    ProjectileId id = 0;
    ParticipantId shooterId = 0;
    uint32_t shooterTeam = kNoTeam;
    std::string weaponId;
    Vector3 positionUu{};
    Vector3 velocityMetersPerSecond{};
    float ageSeconds = 0.0f;
    ProjectileSpec spec{};
    uint32_t groundImpacts = 0;
    bool settledOnGround = false;
};

struct SpawnResult {
    ActionResult action;
    ProjectileId projectileId = 0;
};

// Narrow friend seam used only by deterministic overflow tests. Production
// code has no setter for authority-owned identifiers or score counters.
struct AuthorityTestAccess;

class Authority {
public:
    explicit Authority(AuthorityConfig config = {});

    bool IsConfigurationValid() const { return m_configValid; }
    const AuthorityConfig& GetConfig() const { return m_config; }
    double GetTimeSeconds() const { return m_timeSeconds; }

    float MetersToUu(float meters) const;
    float UuToMeters(float unrealUnits) const;

    bool AddParticipant(const ParticipantSpec& participant);
    bool RemoveParticipant(ParticipantId id);
    bool SetParticipantPosition(ParticipantId id, const Vector3& positionUu);
    bool RespawnParticipant(ParticipantId id, const Vector3& positionUu,
                            float health);
    std::optional<ParticipantSnapshot> GetParticipant(ParticipantId id) const;

    bool AddWeapon(ParticipantId id, const WeaponProfile& weapon,
                   bool makeActive = false);
    bool SetActiveWeapon(ParticipantId id, const std::string& weaponId);
    // Compatibility wrapper: add/replace this weapon and make it active.
    bool EquipWeapon(ParticipantId id, const WeaponProfile& weapon);
    ActionResult RequestReload(ParticipantId id);
    ActionResult RequestReload(ParticipantId id, const std::string& weaponId);
    ActionResult FireHitscan(const HitscanRequest& request);
    ExternalHitscanResult FireHitscanExternal(
        const ExternalHitscanRequest& request);
    ExternalDamageResult ApplyExternalDamage(
        const ExternalDamageRequest& request);

    SpawnResult SpawnProjectile(const ProjectileRequest& request);
    std::optional<ProjectileSnapshot> GetProjectile(ProjectileId id) const;
    size_t GetProjectileCount() const { return m_projectiles.size(); }
    // Map/session transitions remove live projectiles without detonating them.
    // Returned removal events let retail actor channels close in-order before
    // the new world begins.
    std::vector<CombatEvent> RemoveAllProjectiles();

    // Advances reloads and ballistics. Invalid, non-finite, negative,
    // sub-epsilon positive, or overly large deltas are rejected without
    // changing authoritative state.
    AdvanceResult Advance(float deltaSeconds);

private:
    friend struct AuthorityTestAccess;

    struct WeaponState {
        WeaponProfile profile{};
        uint32_t magazineAmmo = 0;
        uint32_t reserveAmmo = 0;
        bool reloading = false;
        double reloadCompletesAtSeconds = 0.0;
        double lastShotAtSeconds = -std::numeric_limits<double>::infinity();
    };

    struct ParticipantState {
        ParticipantSpec spec{};
        float health = 0.0f;
        bool alive = false;
        bool deathEventEmitted = false;
        uint32_t kills = 0;
        uint32_t deaths = 0;
        std::string activeWeaponId;
        std::map<std::string, WeaponState> weapons;
    };

    struct ProjectileState {
        ProjectileId id = 0;
        ParticipantId shooterId = 0;
        uint32_t shooterTeam = kNoTeam;
        std::string weaponId;
        Vector3 positionUu{};
        Vector3 velocityMetersPerSecond{};
        float ageSeconds = 0.0f;
        ProjectileSpec spec{};
        uint32_t groundImpacts = 0;
        bool settledOnGround = false;
    };

    struct SegmentHit {
        bool hit = false;
        ParticipantId participantId = 0;
        float fraction = 0.0f;
        Vector3 positionUu{};
    };

    AuthorityConfig m_config;
    bool m_configValid = false;
    double m_timeSeconds = 0.0;
    ProjectileId m_nextProjectileId = 1;
    std::map<ParticipantId, ParticipantState> m_participants;
    std::map<ProjectileId, ProjectileState> m_projectiles;

    static bool IsFinite(const Vector3& value);
    bool MetersFitUu(float meters) const;
    bool IsFriendlyFire(ParticipantId sourceId, ParticipantId targetId,
                        uint32_t capturedSourceTeam = kNoTeam) const;
    bool ValidateParticipantSpec(const ParticipantSpec& participant) const;
    bool ValidateWeaponProfile(const WeaponProfile& weapon) const;
    bool ValidateProjectileSpec(const ProjectileSpec& projectile) const;
    static ProjectileId NextProjectileId(ProjectileId id);
    std::optional<ProjectileId> FindAvailableProjectileId() const;

    static ParticipantSnapshot MakeParticipantSnapshot(
        const ParticipantState& state);
    static WeaponState* ResolveWeapon(ParticipantState& participant,
                                      const std::string& weaponId);
    bool CompleteReloadIfDue(const ParticipantState& participant,
                             WeaponState& weapon,
                             std::vector<CombatEvent>& events);
    RejectReason AuthorizeWeaponShot(const ParticipantState& shooter,
                                     WeaponState& weapon,
                                     const Vector3& originUu,
                                     std::vector<CombatEvent>& events);
    static void EmitWeaponFired(const ParticipantState& shooter,
                                const WeaponState& weapon,
                                const Vector3& positionUu,
                                ProjectileId projectileId,
                                std::vector<CombatEvent>& events);

    bool IntersectSegmentCapsule(const Vector3& startUu, const Vector3& endUu,
                                 const ParticipantState& participant,
                                 float& fraction, Vector3& pointUu,
                                 float expansionMeters = 0.0f) const;
    bool IntersectSegmentCapsule(const Vector3& startUu, const Vector3& endUu,
                                 const Vector3& centerUu,
                                 const HitVolume& hitVolume,
                                 float& fraction, Vector3& pointUu,
                                 float expansionMeters = 0.0f) const;
    SegmentHit FindFirstParticipantHit(const Vector3& startUu,
                                       const Vector3& endUu,
                                       ParticipantId ignoredId,
                                       std::optional<ParticipantId> graceId = std::nullopt,
                                       float expansionMeters = 0.0f) const;
    HitZone DetermineHitZone(const ParticipantState& target,
                             const Vector3& hitPositionUu) const;
    HitZone DetermineHitZone(const Vector3& centerUu,
                             const HitVolume& hitVolume,
                             const Vector3& hitPositionUu) const;
    float ZoneMultiplier(HitZone zone) const;
    static float WeaponDamageAtDistance(const WeaponProfile& weapon,
                                        float distanceMeters);

    bool ApplyDamage(ParticipantId sourceId, ParticipantId targetId,
                     const std::string& weaponId, float damage, HitZone zone,
                     const Vector3& positionUu, ProjectileId projectileId,
                     std::vector<CombatEvent>& events,
                     uint32_t capturedSourceTeam = kNoTeam);
    void DetonateProjectile(const ProjectileState& projectile,
                            const Vector3& positionUu,
                            std::vector<CombatEvent>& events);
    float DistanceFromCapsuleMeters(const Vector3& pointUu,
                                    const ParticipantState& participant) const;
};

} // namespace CombatAuthority
