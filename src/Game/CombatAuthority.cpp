#include "Game/CombatAuthority.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

namespace CombatAuthority {
namespace {

constexpr float kEpsilon = 1.0e-5f;
constexpr double kGroundTimeEpsilon = 1.0e-7;
constexpr double kGroundHeightToleranceMeters = 1.0e-6;

bool Finite(float value) {
    return std::isfinite(value);
}

void SaturatingIncrement(uint32_t& value) {
    if (value != std::numeric_limits<uint32_t>::max()) ++value;
}

float Clamp(float value, float low, float high) {
    return std::max(low, std::min(value, high));
}

bool SegmentSphereIntersection(const Vector3& start, const Vector3& delta,
                               const Vector3& center, float radius,
                               float& bestFraction) {
    const Vector3 offset = start - center;
    const float a = delta.Dot(delta);
    const float b = 2.0f * offset.Dot(delta);
    const float c = offset.Dot(offset) - radius * radius;
    if (a <= kEpsilon) return false;
    const float discriminant = b * b - 4.0f * a * c;
    if (discriminant < 0.0f) return false;
    const float root = std::sqrt(std::max(0.0f, discriminant));
    const float inv = 0.5f / a;
    const float first = (-b - root) * inv;
    const float second = (-b + root) * inv;
    bool found = false;
    if (first >= 0.0f && first <= 1.0f && first < bestFraction) {
        bestFraction = first;
        found = true;
    }
    if (second >= 0.0f && second <= 1.0f && second < bestFraction) {
        bestFraction = second;
        found = true;
    }
    return found;
}

// Solve the swept vertical ballistic path against a horizontal contact plane.
// A chord-only test can miss a contact when an arc rises and falls within one
// bounded step. The positive-root rule also lets a projectile rebound from the
// plane without immediately re-hitting the t=0 root forever.
bool FindGroundImpactTime(float positionUu, float velocityMetersPerSecond,
                          float gravityMetersPerSecondSquared,
                          float groundContactUu, float maxTimeSeconds,
                          float ueUnitsPerMeter, float& impactTimeSeconds) {
    const double heightMeters =
        (static_cast<double>(positionUu) -
         static_cast<double>(groundContactUu)) /
        static_cast<double>(ueUnitsPerMeter);
    const double velocity = static_cast<double>(velocityMetersPerSecond);
    const double gravity =
        static_cast<double>(gravityMetersPerSecondSquared);
    const double maxTime = static_cast<double>(maxTimeSeconds);

    if (heightMeters < -kGroundHeightToleranceMeters) {
        impactTimeSeconds = 0.0f;
        return true;
    }

    const bool atGround =
        heightMeters <= kGroundHeightToleranceMeters;
    // Contact while stationary/downward is immediate. Upward motion must ignore
    // the t=0 root and may hit again only at a later positive root.
    if (atGround &&
        (velocity < -kGroundTimeEpsilon ||
         (velocity <= kGroundTimeEpsilon &&
          gravity <= kGroundTimeEpsilon))) {
        impactTimeSeconds = 0.0f;
        return true;
    }

    double best = std::numeric_limits<double>::infinity();
    const double minimumRoot = atGround ? kGroundTimeEpsilon : 0.0;
    const auto consider = [&](double root) {
        if (std::isfinite(root) && root >= minimumRoot &&
            root <= maxTime + kGroundTimeEpsilon) {
            best = std::min(best, std::max(0.0, root));
        }
    };

    const double c = atGround ? 0.0 : heightMeters;
    const double a = 0.5 * gravity;
    if (std::abs(a) <= kGroundTimeEpsilon) {
        if (std::abs(velocity) > kGroundTimeEpsilon) {
            consider(-c / velocity);
        }
    } else {
        const double discriminant = velocity * velocity - 4.0 * a * c;
        if (discriminant >= 0.0 && std::isfinite(discriminant)) {
            const double root = std::sqrt(std::max(0.0, discriminant));
            const double denominator = 2.0 * a;
            consider((-velocity - root) / denominator);
            consider((-velocity + root) / denominator);
        }
    }

    if (!std::isfinite(best)) return false;
    impactTimeSeconds = static_cast<float>(std::min(best, maxTime));
    return true;
}

} // namespace

Authority::Authority(AuthorityConfig config) : m_config(std::move(config)) {
    const bool finiteScalars =
        Finite(m_config.ueUnitsPerMeter) &&
        Finite(m_config.originToleranceMeters) &&
        Finite(m_config.reportedImpactToleranceMeters) &&
        Finite(m_config.projectileMaxStepSeconds) &&
        Finite(m_config.maxAdvanceSeconds) &&
        Finite(m_config.friendlyFireDamageScale) &&
        Finite(m_config.headMinNormalizedHeight) &&
        Finite(m_config.upperTorsoMinNormalizedHeight) &&
        Finite(m_config.lowerTorsoMinNormalizedHeight) &&
        Finite(m_config.headDamageMultiplier) &&
        Finite(m_config.upperTorsoDamageMultiplier) &&
        Finite(m_config.lowerTorsoDamageMultiplier) &&
        Finite(m_config.legsDamageMultiplier);
    m_configValid = finiteScalars &&
        m_config.ueUnitsPerMeter > 0.0f &&
        m_config.originToleranceMeters >= 0.0f &&
        m_config.reportedImpactToleranceMeters >= 0.0f &&
        m_config.projectileMaxStepSeconds > kEpsilon &&
        m_config.maxAdvanceSeconds > kEpsilon &&
        static_cast<double>(m_config.maxAdvanceSeconds) /
                static_cast<double>(m_config.projectileMaxStepSeconds) <=
            static_cast<double>(kMaxProjectileSubstepsPerAdvance) &&
        m_config.maxActiveProjectiles > 0 &&
        m_config.maxActiveProjectiles <= kHardMaxActiveProjectiles &&
        m_config.friendlyFireDamageScale >= 0.0f &&
        m_config.headMinNormalizedHeight <= 1.0f &&
        m_config.headMinNormalizedHeight > m_config.upperTorsoMinNormalizedHeight &&
        m_config.upperTorsoMinNormalizedHeight >
            m_config.lowerTorsoMinNormalizedHeight &&
        m_config.lowerTorsoMinNormalizedHeight >= -1.0f &&
        m_config.headDamageMultiplier >= 0.0f &&
        m_config.upperTorsoDamageMultiplier >= 0.0f &&
        m_config.lowerTorsoDamageMultiplier >= 0.0f &&
        m_config.legsDamageMultiplier >= 0.0f;
}

float Authority::MetersToUu(float meters) const {
    return meters * m_config.ueUnitsPerMeter;
}

float Authority::UuToMeters(float unrealUnits) const {
    return unrealUnits / m_config.ueUnitsPerMeter;
}

bool Authority::IsFinite(const Vector3& value) {
    return Finite(value.x) && Finite(value.y) && Finite(value.z);
}

bool Authority::MetersFitUu(float meters) const {
    return Finite(meters) && Finite(MetersToUu(meters));
}

bool Authority::ValidateParticipantSpec(const ParticipantSpec& participant) const {
    return m_configValid && participant.id != 0 &&
        IsFinite(participant.positionUu) &&
        Finite(participant.maxHealth) && Finite(participant.health) &&
        Finite(participant.hitVolume.radiusMeters) &&
        Finite(participant.hitVolume.halfHeightMeters) &&
        participant.maxHealth > 0.0f && participant.health >= 0.0f &&
        participant.health <= participant.maxHealth &&
        participant.hitVolume.radiusMeters > 0.0f &&
        participant.hitVolume.halfHeightMeters >=
            participant.hitVolume.radiusMeters &&
        MetersFitUu(participant.hitVolume.radiusMeters) &&
        MetersFitUu(participant.hitVolume.halfHeightMeters);
}

bool Authority::ValidateWeaponProfile(const WeaponProfile& weapon) const {
    // A profile must not be able to consume ammo and only then overflow while
    // applying an otherwise-authorized hit.
    const double largestZoneScale = std::max({
        static_cast<double>(m_config.headDamageMultiplier),
        static_cast<double>(m_config.upperTorsoDamageMultiplier),
        static_cast<double>(m_config.lowerTorsoDamageMultiplier),
        static_cast<double>(m_config.legsDamageMultiplier)});
    const double largestRelationshipScale = m_config.friendlyFireEnabled
        ? std::max(1.0, static_cast<double>(m_config.friendlyFireDamageScale))
        : 1.0;
    const double largestAppliedDamage =
        static_cast<double>(weapon.baseDamage) * largestZoneScale *
        largestRelationshipScale;

    return m_configValid && !weapon.id.empty() &&
        weapon.magazineCapacity > 0 &&
        weapon.initialMagazine <= weapon.magazineCapacity &&
        Finite(weapon.roundsPerMinute) && weapon.roundsPerMinute > 0.0f &&
        Finite(weapon.reloadSeconds) && weapon.reloadSeconds >= 0.0f &&
        Finite(weapon.maxRangeMeters) && weapon.maxRangeMeters > 0.0f &&
        Finite(weapon.baseDamage) && weapon.baseDamage >= 0.0f &&
        Finite(weapon.minDamageFractionAtMaxRange) &&
        weapon.minDamageFractionAtMaxRange >= 0.0f &&
        weapon.minDamageFractionAtMaxRange <= 1.0f &&
        std::isfinite(largestAppliedDamage) &&
        largestAppliedDamage <=
            static_cast<double>(std::numeric_limits<float>::max());
}

bool Authority::ValidateProjectileSpec(const ProjectileSpec& projectile) const {
    const auto fitsFloat = [](double value) {
        return std::isfinite(value) &&
            std::fabs(value) <=
                static_cast<double>(std::numeric_limits<float>::max());
    };
    const double units = static_cast<double>(m_config.ueUnitsPerMeter);
    const double step =
        static_cast<double>(m_config.projectileMaxStepSeconds);
    const double collisionUu =
        static_cast<double>(projectile.collisionRadiusMeters) * units;
    const double largestZoneScale = std::max({
        static_cast<double>(m_config.headDamageMultiplier),
        static_cast<double>(m_config.upperTorsoDamageMultiplier),
        static_cast<double>(m_config.lowerTorsoDamageMultiplier),
        static_cast<double>(m_config.legsDamageMultiplier)});
    const double largestRelationshipScale = m_config.friendlyFireEnabled
        ? std::max(1.0, static_cast<double>(m_config.friendlyFireDamageScale))
        : 1.0;
    const double largestDirectDamage =
        static_cast<double>(projectile.directDamage) * largestZoneScale *
        largestRelationshipScale;
    const double largestExplosionDamage =
        static_cast<double>(projectile.maxExplosionDamage) * largestZoneScale *
        largestRelationshipScale;
    const bool finiteStepMotion =
        fitsFloat(static_cast<double>(projectile.launchSpeedMetersPerSecond) *
                  units * step) &&
        fitsFloat(static_cast<double>(projectile.gravityMetersPerSecondSquared.x) *
                  step) &&
        fitsFloat(static_cast<double>(projectile.gravityMetersPerSecondSquared.y) *
                  step) &&
        fitsFloat(static_cast<double>(projectile.gravityMetersPerSecondSquared.z) *
                  step) &&
        fitsFloat(0.5 *
                  static_cast<double>(projectile.gravityMetersPerSecondSquared.x) *
                  step * step * units) &&
        fitsFloat(0.5 *
                  static_cast<double>(projectile.gravityMetersPerSecondSquared.y) *
                  step * step * units) &&
        fitsFloat(0.5 *
                  static_cast<double>(projectile.gravityMetersPerSecondSquared.z) *
                  step * step * units);

    return Finite(projectile.launchSpeedMetersPerSecond) &&
        projectile.launchSpeedMetersPerSecond > 0.0f &&
        IsFinite(projectile.gravityMetersPerSecondSquared) &&
        Finite(projectile.fuseSeconds) && projectile.fuseSeconds >= 0.0f &&
        Finite(projectile.maxLifetimeSeconds) &&
        projectile.maxLifetimeSeconds > 0.0f &&
        Finite(projectile.collisionRadiusMeters) &&
        projectile.collisionRadiusMeters >= 0.0f &&
        Finite(projectile.directDamage) && projectile.directDamage >= 0.0f &&
        Finite(projectile.explosionRadiusMeters) &&
        projectile.explosionRadiusMeters >= 0.0f &&
        Finite(projectile.maxExplosionDamage) &&
        projectile.maxExplosionDamage >= 0.0f &&
        Finite(projectile.groundHeightUu) &&
        Finite(projectile.ownerCollisionGraceSeconds) &&
        projectile.ownerCollisionGraceSeconds >= 0.0f &&
        projectile.maxGroundImpacts <= kMaxProjectileGroundImpacts &&
        Finite(projectile.groundBounceDamping) &&
        projectile.groundBounceDamping >= 0.0f &&
        projectile.groundBounceDamping <= 1.0f &&
        Finite(projectile.groundBounceVerticalDamping) &&
        projectile.groundBounceVerticalDamping >= 0.0f &&
        projectile.groundBounceVerticalDamping <= 1.0f &&
        MetersFitUu(projectile.launchSpeedMetersPerSecond) &&
        MetersFitUu(projectile.collisionRadiusMeters) &&
        MetersFitUu(projectile.explosionRadiusMeters) &&
        fitsFloat(static_cast<double>(projectile.groundHeightUu) +
                  collisionUu) &&
        fitsFloat(largestDirectDamage) &&
        fitsFloat(largestExplosionDamage) &&
        finiteStepMotion;
}

ProjectileId Authority::NextProjectileId(ProjectileId id) {
    return id == std::numeric_limits<ProjectileId>::max() ? 1 : id + 1;
}

std::optional<ProjectileId> Authority::FindAvailableProjectileId() const {
    if (m_projectiles.size() >= m_config.maxActiveProjectiles) {
        return std::nullopt;
    }

    ProjectileId candidate = m_nextProjectileId == 0 ? 1 : m_nextProjectileId;
    // With N active keys, at least one of any N+1 distinct candidates is free.
    // The configured active cap makes this collision recovery bounded.
    for (size_t attempt = 0; attempt <= m_projectiles.size(); ++attempt) {
        if (!m_projectiles.contains(candidate)) return candidate;
        candidate = NextProjectileId(candidate);
    }
    return std::nullopt;
}

bool Authority::AddParticipant(const ParticipantSpec& participant) {
    if (!ValidateParticipantSpec(participant) ||
        m_participants.contains(participant.id)) {
        return false;
    }
    ParticipantState state;
    state.spec = participant;
    state.health = participant.health;
    state.alive = participant.health > 0.0f;
    state.deathEventEmitted = !state.alive;
    m_participants.emplace(participant.id, std::move(state));
    return true;
}

bool Authority::RemoveParticipant(ParticipantId id) {
    return m_participants.erase(id) != 0;
}

bool Authority::SetParticipantPosition(ParticipantId id,
                                       const Vector3& positionUu) {
    auto it = m_participants.find(id);
    if (it == m_participants.end() || !IsFinite(positionUu)) return false;
    it->second.spec.positionUu = positionUu;
    return true;
}

bool Authority::RespawnParticipant(ParticipantId id,
                                   const Vector3& positionUu, float health) {
    auto it = m_participants.find(id);
    if (it == m_participants.end() || !IsFinite(positionUu) || !Finite(health) ||
        health <= 0.0f || health > it->second.spec.maxHealth) {
        return false;
    }
    ParticipantState& state = it->second;
    state.spec.positionUu = positionUu;
    state.health = health;
    state.alive = true;
    state.deathEventEmitted = false;
    for (auto& [weaponId, weapon] : state.weapons) {
        (void)weaponId;
        weapon.reloading = false;
        weapon.reloadCompletesAtSeconds = 0.0;
    }
    return true;
}

ParticipantSnapshot Authority::MakeParticipantSnapshot(
    const ParticipantState& state) {
    ParticipantSnapshot snapshot;
    snapshot.id = state.spec.id;
    snapshot.team = state.spec.team;
    snapshot.positionUu = state.spec.positionUu;
    snapshot.maxHealth = state.spec.maxHealth;
    snapshot.health = state.health;
    snapshot.alive = state.alive;
    snapshot.kills = state.kills;
    snapshot.deaths = state.deaths;
    snapshot.hitVolume = state.spec.hitVolume;
    for (const auto& [weaponId, stateWeapon] : state.weapons) {
        WeaponSnapshot weapon;
        weapon.id = stateWeapon.profile.id;
        weapon.magazineAmmo = stateWeapon.magazineAmmo;
        weapon.reserveAmmo = stateWeapon.reserveAmmo;
        weapon.reloading = stateWeapon.reloading;
        weapon.reloadCompletesAtSeconds = stateWeapon.reloadCompletesAtSeconds;
        snapshot.weapons.emplace(weaponId, std::move(weapon));
    }
    const auto active = snapshot.weapons.find(state.activeWeaponId);
    if (active != snapshot.weapons.end()) {
        snapshot.activeWeaponId = state.activeWeaponId;
        snapshot.weapon = active->second;
    }
    return snapshot;
}

std::optional<ParticipantSnapshot> Authority::GetParticipant(
    ParticipantId id) const {
    const auto it = m_participants.find(id);
    if (it == m_participants.end()) return std::nullopt;
    return MakeParticipantSnapshot(it->second);
}

bool Authority::AddWeapon(ParticipantId id, const WeaponProfile& weapon,
                          bool makeActive) {
    auto it = m_participants.find(id);
    if (it == m_participants.end() || !ValidateWeaponProfile(weapon)) return false;
    WeaponState state;
    state.profile = weapon;
    state.magazineAmmo = weapon.initialMagazine;
    state.reserveAmmo = weapon.initialReserve;
    it->second.weapons.insert_or_assign(weapon.id, std::move(state));
    if (makeActive) it->second.activeWeaponId = weapon.id;
    return true;
}

bool Authority::SetActiveWeapon(ParticipantId id,
                                const std::string& weaponId) {
    auto participant = m_participants.find(id);
    if (participant == m_participants.end() || weaponId.empty() ||
        !participant->second.weapons.contains(weaponId)) {
        return false;
    }
    participant->second.activeWeaponId = weaponId;
    return true;
}

bool Authority::EquipWeapon(ParticipantId id, const WeaponProfile& weapon) {
    return AddWeapon(id, weapon, true);
}

Authority::WeaponState* Authority::ResolveWeapon(
    ParticipantState& participant, const std::string& weaponId) {
    const std::string& selectedId =
        weaponId.empty() ? participant.activeWeaponId : weaponId;
    if (selectedId.empty()) return nullptr;
    const auto selected = participant.weapons.find(selectedId);
    return selected == participant.weapons.end() ? nullptr : &selected->second;
}

bool Authority::CompleteReloadIfDue(const ParticipantState& participant,
                                    WeaponState& weapon,
                                    std::vector<CombatEvent>& events) {
    if (!weapon.reloading ||
        m_timeSeconds + static_cast<double>(kEpsilon) <
            weapon.reloadCompletesAtSeconds) {
        return false;
    }
    const uint32_t missing = weapon.profile.magazineCapacity - weapon.magazineAmmo;
    const uint32_t moved = std::min(missing, weapon.reserveAmmo);
    weapon.magazineAmmo += moved;
    weapon.reserveAmmo -= moved;
    weapon.reloading = false;

    CombatEvent event;
    event.kind = EventKind::ReloadCompleted;
    event.sourceId = participant.spec.id;
    event.weaponId = weapon.profile.id;
    event.magazineAmmo = weapon.magazineAmmo;
    event.reserveAmmo = weapon.reserveAmmo;
    events.push_back(std::move(event));
    return true;
}

ActionResult Authority::RequestReload(ParticipantId id) {
    return RequestReload(id, {});
}

ActionResult Authority::RequestReload(ParticipantId id,
                                      const std::string& weaponId) {
    ActionResult result;
    if (!m_configValid) {
        result.reason = RejectReason::InvalidConfiguration;
        return result;
    }
    auto it = m_participants.find(id);
    if (it == m_participants.end()) {
        result.reason = RejectReason::UnknownParticipant;
        return result;
    }
    ParticipantState& participant = it->second;
    if (!participant.alive) {
        result.reason = RejectReason::ParticipantDead;
        return result;
    }
    WeaponState* weapon = ResolveWeapon(participant, weaponId);
    if (weapon == nullptr) {
        result.reason = RejectReason::NoWeapon;
        return result;
    }
    CompleteReloadIfDue(participant, *weapon, result.events);
    if (weapon->reloading) {
        result.reason = RejectReason::Reloading;
        return result;
    }
    if (weapon->magazineAmmo >= weapon->profile.magazineCapacity ||
        weapon->reserveAmmo == 0) {
        result.reason = RejectReason::InvalidArgument;
        return result;
    }

    weapon->reloading = true;
    weapon->reloadCompletesAtSeconds =
        m_timeSeconds + static_cast<double>(weapon->profile.reloadSeconds);
    CombatEvent started;
    started.kind = EventKind::ReloadStarted;
    started.sourceId = id;
    started.weaponId = weapon->profile.id;
    started.magazineAmmo = weapon->magazineAmmo;
    started.reserveAmmo = weapon->reserveAmmo;
    result.events.push_back(std::move(started));
    CompleteReloadIfDue(participant, *weapon, result.events);
    result.outcome = ActionOutcome::Accepted;
    return result;
}

RejectReason Authority::AuthorizeWeaponShot(
    const ParticipantState& shooter, WeaponState& weapon,
    const Vector3& originUu,
    std::vector<CombatEvent>& events) {
    if (!shooter.alive) return RejectReason::ParticipantDead;
    if (!IsFinite(originUu) ||
        UuToMeters(originUu.Distance(shooter.spec.positionUu)) >
            m_config.originToleranceMeters) {
        return RejectReason::OriginMismatch;
    }
    CompleteReloadIfDue(shooter, weapon, events);
    if (weapon.reloading) return RejectReason::Reloading;
    if (weapon.magazineAmmo == 0) return RejectReason::EmptyMagazine;
    const double shotInterval = 60.0 / static_cast<double>(weapon.profile.roundsPerMinute);
    if (m_timeSeconds + static_cast<double>(kEpsilon) <
        weapon.lastShotAtSeconds + shotInterval) {
        return RejectReason::Cadence;
    }
    --weapon.magazineAmmo;
    weapon.lastShotAtSeconds = m_timeSeconds;
    return RejectReason::None;
}

void Authority::EmitWeaponFired(const ParticipantState& shooter,
                                const WeaponState& weapon,
                                const Vector3& positionUu,
                                ProjectileId projectileId,
                                std::vector<CombatEvent>& events) {
    CombatEvent event;
    event.kind = EventKind::WeaponFired;
    event.sourceId = shooter.spec.id;
    event.sourceTeam = shooter.spec.team;
    event.projectileId = projectileId;
    event.weaponId = weapon.profile.id;
    event.positionUu = positionUu;
    event.magazineAmmo = weapon.magazineAmmo;
    event.reserveAmmo = weapon.reserveAmmo;
    events.push_back(std::move(event));
}

bool Authority::IntersectSegmentCapsule(const Vector3& startUu,
                                        const Vector3& endUu,
                                        const ParticipantState& participant,
                                        float& fraction,
                                        Vector3& pointUu,
                                        float expansionMeters) const {
    return IntersectSegmentCapsule(
        startUu, endUu, participant.spec.positionUu,
        participant.spec.hitVolume, fraction, pointUu, expansionMeters);
}

bool Authority::IntersectSegmentCapsule(const Vector3& startUu,
                                        const Vector3& endUu,
                                        const Vector3& centerUu,
                                        const HitVolume& hitVolume,
                                        float& fraction,
                                        Vector3& pointUu,
                                        float expansionMeters) const {
    const double radiusUu =
        (static_cast<double>(hitVolume.radiusMeters) +
         static_cast<double>(expansionMeters)) *
        static_cast<double>(m_config.ueUnitsPerMeter);
    const double halfHeightUu =
        (static_cast<double>(hitVolume.halfHeightMeters) +
         static_cast<double>(expansionMeters)) *
        static_cast<double>(m_config.ueUnitsPerMeter);
    if (!std::isfinite(radiusUu) || !std::isfinite(halfHeightUu) ||
        radiusUu < 0.0 || halfHeightUu < radiusUu ||
        radiusUu > static_cast<double>(std::numeric_limits<float>::max()) ||
        halfHeightUu >
            static_cast<double>(std::numeric_limits<float>::max())) {
        return false;
    }
    const float radius = static_cast<float>(radiusUu);
    const float halfHeight = static_cast<float>(halfHeightUu);
    const float spineHalf = std::max(0.0f, halfHeight - radius);
    const Vector3 center = centerUu;
    const float closestZ = Clamp(startUu.z, center.z - spineHalf,
                                 center.z + spineHalf);
    const Vector3 closest(center.x, center.y, closestZ);
    if (startUu.DistanceSquared(closest) <= radius * radius) {
        fraction = 0.0f;
        pointUu = startUu;
        return true;
    }

    const Vector3 delta = endUu - startUu;
    if (delta.LengthSquared() <= kEpsilon * kEpsilon) return false;
    float best = std::numeric_limits<float>::infinity();

    // Infinite vertical cylinder, restricted to the capsule's spine extent.
    const float ox = startUu.x - center.x;
    const float oy = startUu.y - center.y;
    const float a = delta.x * delta.x + delta.y * delta.y;
    if (a > kEpsilon) {
        const float b = 2.0f * (ox * delta.x + oy * delta.y);
        const float c = ox * ox + oy * oy - radius * radius;
        const float discriminant = b * b - 4.0f * a * c;
        if (discriminant >= 0.0f) {
            const float root = std::sqrt(std::max(0.0f, discriminant));
            const float inv = 0.5f / a;
            const float candidates[2] = {(-b - root) * inv, (-b + root) * inv};
            for (float candidate : candidates) {
                if (candidate < 0.0f || candidate > 1.0f || candidate >= best) {
                    continue;
                }
                const float z = startUu.z + delta.z * candidate;
                if (z >= center.z - spineHalf && z <= center.z + spineHalf) {
                    best = candidate;
                }
            }
        }
    }

    SegmentSphereIntersection(startUu, delta,
                              Vector3(center.x, center.y, center.z - spineHalf),
                              radius, best);
    SegmentSphereIntersection(startUu, delta,
                              Vector3(center.x, center.y, center.z + spineHalf),
                              radius, best);
    if (!Finite(best)) return false;
    fraction = best;
    pointUu = startUu + delta * best;
    return true;
}

Authority::SegmentHit Authority::FindFirstParticipantHit(
    const Vector3& startUu, const Vector3& endUu, ParticipantId ignoredId,
    std::optional<ParticipantId> graceId, float expansionMeters) const {
    SegmentHit nearest;
    float best = std::numeric_limits<float>::infinity();
    for (const auto& [id, participant] : m_participants) {
        if (!participant.alive || id == ignoredId ||
            (graceId && id == *graceId)) {
            continue;
        }
        float fraction = 0.0f;
        Vector3 point{};
        if (IntersectSegmentCapsule(startUu, endUu, participant, fraction, point,
                                    expansionMeters) &&
            fraction < best) {
            best = fraction;
            nearest.hit = true;
            nearest.participantId = id;
            nearest.fraction = fraction;
            nearest.positionUu = point;
        }
    }
    return nearest;
}

HitZone Authority::DetermineHitZone(const ParticipantState& target,
                                    const Vector3& hitPositionUu) const {
    return DetermineHitZone(target.spec.positionUu, target.spec.hitVolume,
                            hitPositionUu);
}

HitZone Authority::DetermineHitZone(const Vector3& centerUu,
                                    const HitVolume& hitVolume,
                                    const Vector3& hitPositionUu) const {
    const float halfHeight =
        MetersToUu(hitVolume.halfHeightMeters);
    const float normalized = Clamp(
        (hitPositionUu.z - centerUu.z) / halfHeight, -1.0f, 1.0f);
    if (normalized >= m_config.headMinNormalizedHeight) return HitZone::Head;
    if (normalized >= m_config.upperTorsoMinNormalizedHeight) {
        return HitZone::UpperTorso;
    }
    if (normalized >= m_config.lowerTorsoMinNormalizedHeight) {
        return HitZone::LowerTorso;
    }
    return HitZone::Legs;
}

float Authority::ZoneMultiplier(HitZone zone) const {
    switch (zone) {
        case HitZone::Head: return m_config.headDamageMultiplier;
        case HitZone::UpperTorso: return m_config.upperTorsoDamageMultiplier;
        case HitZone::LowerTorso: return m_config.lowerTorsoDamageMultiplier;
        case HitZone::Legs: return m_config.legsDamageMultiplier;
    }
    return 1.0f;
}

float Authority::WeaponDamageAtDistance(const WeaponProfile& weapon,
                                        float distanceMeters) {
    const float alpha = Clamp(distanceMeters / weapon.maxRangeMeters, 0.0f, 1.0f);
    const float fraction = 1.0f -
        alpha * (1.0f - weapon.minDamageFractionAtMaxRange);
    return weapon.baseDamage * fraction;
}

bool Authority::IsFriendlyFire(ParticipantId sourceId, ParticipantId targetId,
                               uint32_t capturedSourceTeam) const {
    if (sourceId == targetId) return false;
    const auto target = m_participants.find(targetId);
    if (target == m_participants.end()) return false;

    uint32_t sourceTeam = capturedSourceTeam;
    if (sourceTeam == kNoTeam) {
        const auto source = m_participants.find(sourceId);
        if (source == m_participants.end()) return false;
        sourceTeam = source->second.spec.team;
    }
    return sourceTeam != kNoTeam && sourceTeam == target->second.spec.team;
}

bool Authority::ApplyDamage(ParticipantId sourceId, ParticipantId targetId,
                            const std::string& weaponId, float damage,
                            HitZone zone, const Vector3& positionUu,
                            ProjectileId projectileId,
                            std::vector<CombatEvent>& events,
                            uint32_t capturedSourceTeam) {
    auto targetIt = m_participants.find(targetId);
    if (targetIt == m_participants.end() || !targetIt->second.alive ||
        !Finite(damage) || damage <= 0.0f) {
        return false;
    }
    ParticipantState& target = targetIt->second;
    uint32_t sourceTeam = capturedSourceTeam;
    if (sourceTeam == kNoTeam) {
        const auto sourceIt = m_participants.find(sourceId);
        if (sourceIt != m_participants.end()) {
            sourceTeam = sourceIt->second.spec.team;
        }
    }
    const bool friendly = IsFriendlyFire(sourceId, targetId, sourceTeam);
    if (friendly && !m_config.friendlyFireEnabled) {
        CombatEvent blocked;
        blocked.kind = EventKind::FriendlyFireBlocked;
        blocked.sourceId = sourceId;
        blocked.targetId = targetId;
        blocked.sourceTeam = sourceTeam;
        blocked.projectileId = projectileId;
        blocked.weaponId = weaponId;
        blocked.positionUu = positionUu;
        blocked.hitZone = zone;
        blocked.healthAfter = target.health;
        blocked.friendlyFire = true;
        events.push_back(std::move(blocked));
        return false;
    }

    if (friendly) damage *= m_config.friendlyFireDamageScale;
    if (!Finite(damage) || damage <= 0.0f) return false;
    target.health = std::max(0.0f, target.health - damage);

    CombatEvent applied;
    applied.kind = EventKind::DamageApplied;
    applied.sourceId = sourceId;
    applied.targetId = targetId;
    applied.sourceTeam = sourceTeam;
    applied.projectileId = projectileId;
    applied.weaponId = weaponId;
    applied.positionUu = positionUu;
    applied.hitZone = zone;
    applied.damage = damage;
    applied.healthAfter = target.health;
    applied.friendlyFire = friendly;
    events.push_back(std::move(applied));

    if (target.health <= 0.0f && target.alive) {
        target.alive = false;
        for (auto& [targetWeaponId, targetWeapon] : target.weapons) {
            (void)targetWeaponId;
            targetWeapon.reloading = false;
            targetWeapon.reloadCompletesAtSeconds = 0.0;
        }
        if (!target.deathEventEmitted) {
            target.deathEventEmitted = true;
            SaturatingIncrement(target.deaths);
            auto sourceIt = m_participants.find(sourceId);
            if (sourceIt != m_participants.end() && sourceId != targetId) {
                SaturatingIncrement(sourceIt->second.kills);
            }
            CombatEvent died;
            died.kind = EventKind::ParticipantDied;
            died.sourceId = sourceId;
            died.targetId = targetId;
            died.sourceTeam = sourceTeam;
            died.projectileId = projectileId;
            died.weaponId = weaponId;
            died.positionUu = positionUu;
            died.hitZone = zone;
            died.healthAfter = 0.0f;
            died.friendlyFire = friendly;
            events.push_back(std::move(died));
        }
    }
    return true;
}

ActionResult Authority::FireHitscan(const HitscanRequest& request) {
    ActionResult result;
    if (!m_configValid) {
        result.reason = RejectReason::InvalidConfiguration;
        return result;
    }
    if (!IsFinite(request.startTraceUu) || !IsFinite(request.endTraceUu) ||
        !IsFinite(request.reportedImpactUu)) {
        result.reason = RejectReason::InvalidArgument;
        return result;
    }
    auto shooterIt = m_participants.find(request.shooterId);
    if (shooterIt == m_participants.end()) {
        result.reason = RejectReason::UnknownParticipant;
        return result;
    }
    ParticipantState& shooter = shooterIt->second;
    if (!shooter.alive) {
        result.reason = RejectReason::ParticipantDead;
        return result;
    }
    WeaponState* weapon = ResolveWeapon(shooter, request.weaponId);
    if (weapon == nullptr) {
        result.reason = RejectReason::NoWeapon;
        return result;
    }
    if (request.claimedTargetId) {
        const auto target = m_participants.find(*request.claimedTargetId);
        if (target == m_participants.end()) {
            result.reason = RejectReason::UnknownTarget;
            return result;
        }
        if (!target->second.alive) {
            result.reason = RejectReason::TargetDead;
            return result;
        }
    }

    const Vector3 segment = request.endTraceUu - request.startTraceUu;
    const float segmentLengthUu = segment.Length();
    if (!Finite(segmentLengthUu) || segmentLengthUu <= kEpsilon) {
        result.reason = RejectReason::ZeroLengthSegment;
        return result;
    }
    if (UuToMeters(segmentLengthUu) > weapon->profile.maxRangeMeters) {
        result.reason = RejectReason::OutOfRange;
        return result;
    }

    const RejectReason authorization =
        AuthorizeWeaponShot(shooter, *weapon, request.startTraceUu,
                            result.events);
    if (authorization != RejectReason::None) {
        result.reason = authorization;
        return result;
    }
    EmitWeaponFired(shooter, *weapon, request.startTraceUu, 0, result.events);
    result.outcome = ActionOutcome::AcceptedMiss;
    if (!request.claimedTargetId) {
        result.reason = RejectReason::NoValidatedHit;
        return result;
    }

    const SegmentHit hit = FindFirstParticipantHit(
        request.startTraceUu, request.endTraceUu, request.shooterId);
    if (!hit.hit) {
        result.reason = RejectReason::NoValidatedHit;
        return result;
    }
    if (hit.participantId != *request.claimedTargetId) {
        result.reason = RejectReason::TargetMismatch;
        return result;
    }
    if (UuToMeters(hit.positionUu.Distance(request.reportedImpactUu)) >
        m_config.reportedImpactToleranceMeters) {
        result.reason = RejectReason::ReportedImpactMismatch;
        return result;
    }

    const ParticipantState& target = m_participants.at(hit.participantId);
    const HitZone zone = DetermineHitZone(target, hit.positionUu);
    const float distanceMeters =
        UuToMeters(request.startTraceUu.Distance(hit.positionUu));
    const float damage =
        WeaponDamageAtDistance(weapon->profile, distanceMeters) *
        ZoneMultiplier(zone);
    if (!ApplyDamage(request.shooterId, hit.participantId,
                     weapon->profile.id, damage, zone, hit.positionUu, 0,
                     result.events)) {
        result.reason = IsFriendlyFire(request.shooterId, hit.participantId)
            ? RejectReason::FriendlyFireBlocked
            : RejectReason::NoValidatedHit;
        return result;
    }
    result.outcome = ActionOutcome::AcceptedHit;
    result.reason = RejectReason::None;
    return result;
}

ExternalHitscanResult Authority::FireHitscanExternal(
    const ExternalHitscanRequest& request) {
    ExternalHitscanResult result;
    ActionResult& action = result.action;
    if (!m_configValid) {
        action.reason = RejectReason::InvalidConfiguration;
        return result;
    }
    if (!IsFinite(request.startTraceUu) || !IsFinite(request.endTraceUu) ||
        !IsFinite(request.reportedImpactUu) ||
        !request.claimedTargetId.IsBot() ||
        request.targets.size() > kMaxExternalHitscanTargets) {
        action.reason = RejectReason::InvalidArgument;
        return result;
    }

    auto shooterIt = m_participants.find(request.shooterId);
    if (shooterIt == m_participants.end()) {
        action.reason = RejectReason::UnknownParticipant;
        return result;
    }
    ParticipantState& shooter = shooterIt->second;
    if (!shooter.alive) {
        action.reason = RejectReason::ParticipantDead;
        return result;
    }
    WeaponState* weapon = ResolveWeapon(shooter, request.weaponId);
    if (weapon == nullptr) {
        action.reason = RejectReason::NoWeapon;
        return result;
    }

    const ExternalParticipantSpec* claimedTarget = nullptr;
    std::set<::ParticipantId> uniqueTargets;
    for (const ExternalParticipantSpec& target : request.targets) {
        const bool validTeam = target.team == 1u || target.team == 2u;
        const bool validVolume =
            Finite(target.hitVolume.radiusMeters) &&
            Finite(target.hitVolume.halfHeightMeters) &&
            target.hitVolume.radiusMeters > 0.0f &&
            target.hitVolume.halfHeightMeters >=
                target.hitVolume.radiusMeters &&
            MetersFitUu(target.hitVolume.radiusMeters) &&
            MetersFitUu(target.hitVolume.halfHeightMeters);
        if (!target.id.IsBot() || !validTeam ||
            !IsFinite(target.positionUu) || !validVolume ||
            !uniqueTargets.insert(target.id).second) {
            action.reason = RejectReason::InvalidArgument;
            return result;
        }
        if (target.id == request.claimedTargetId) {
            claimedTarget = &target;
        }
    }
    if (claimedTarget == nullptr) {
        action.reason = RejectReason::UnknownTarget;
        return result;
    }
    if (!claimedTarget->alive) {
        action.reason = RejectReason::TargetDead;
        return result;
    }

    const Vector3 segment = request.endTraceUu - request.startTraceUu;
    const float segmentLengthUu = segment.Length();
    if (!Finite(segmentLengthUu) || segmentLengthUu <= kEpsilon) {
        action.reason = RejectReason::ZeroLengthSegment;
        return result;
    }
    if (UuToMeters(segmentLengthUu) > weapon->profile.maxRangeMeters) {
        action.reason = RejectReason::OutOfRange;
        return result;
    }

    const RejectReason authorization = AuthorizeWeaponShot(
        shooter, *weapon, request.startTraceUu, action.events);
    if (authorization != RejectReason::None) {
        action.reason = authorization;
        return result;
    }
    EmitWeaponFired(shooter, *weapon, request.startTraceUu, 0, action.events);
    action.outcome = ActionOutcome::AcceptedMiss;

    bool found = false;
    float nearestFraction = std::numeric_limits<float>::infinity();
    ::ParticipantId nearestId{};
    Vector3 nearestPosition{};
    auto considerHit = [&](const ::ParticipantId& id, float fraction,
                           const Vector3& positionUu) {
        // Stable tie-breaking makes the result independent of adapter vector
        // order. Human sorts before Bot, so overlapping tagged actors cannot be
        // used to shoot through an internal participant into Bot(rawSameId).
        const bool strictlyNearer = fraction + kEpsilon < nearestFraction;
        const bool tied = found &&
            std::fabs(fraction - nearestFraction) <= kEpsilon;
        if (!found || strictlyNearer || (tied && id < nearestId)) {
            found = true;
            nearestFraction = fraction;
            nearestId = id;
            nearestPosition = positionUu;
        }
    };

    for (const auto& [id, participant] : m_participants) {
        if (!participant.alive || id == request.shooterId) continue;
        float fraction = 0.0f;
        Vector3 position{};
        if (IntersectSegmentCapsule(
                request.startTraceUu, request.endTraceUu, participant,
                fraction, position)) {
            considerHit(::ParticipantId::Human(id), fraction, position);
        }
    }
    for (const ExternalParticipantSpec& target : request.targets) {
        if (!target.alive) continue;
        float fraction = 0.0f;
        Vector3 position{};
        if (IntersectSegmentCapsule(
                request.startTraceUu, request.endTraceUu, target.positionUu,
                target.hitVolume, fraction, position)) {
            considerHit(target.id, fraction, position);
        }
    }

    if (!found) {
        action.reason = RejectReason::NoValidatedHit;
        return result;
    }
    if (nearestId != request.claimedTargetId) {
        action.reason = RejectReason::TargetMismatch;
        return result;
    }
    if (UuToMeters(nearestPosition.Distance(request.reportedImpactUu)) >
        m_config.reportedImpactToleranceMeters) {
        action.reason = RejectReason::ReportedImpactMismatch;
        return result;
    }

    ExternalHitResult hit;
    hit.participantId = claimedTarget->id;
    hit.targetTeam = claimedTarget->team;
    hit.positionUu = nearestPosition;
    hit.hitZone = DetermineHitZone(
        claimedTarget->positionUu, claimedTarget->hitVolume, nearestPosition);
    const float distanceMeters =
        UuToMeters(request.startTraceUu.Distance(nearestPosition));
    hit.damage = WeaponDamageAtDistance(weapon->profile, distanceMeters) *
        ZoneMultiplier(hit.hitZone);

    const bool friendly = shooter.spec.team != kNoTeam &&
        shooter.spec.team == claimedTarget->team;
    if (friendly && !m_config.friendlyFireEnabled) {
        hit.damage = 0.0f;
        hit.relationship = ExternalHitRelationship::FriendlyFireBlocked;
        result.hit = hit;
        action.reason = RejectReason::FriendlyFireBlocked;
        return result;
    }
    if (friendly) {
        hit.damage *= m_config.friendlyFireDamageScale;
        hit.relationship = ExternalHitRelationship::FriendlyFireAuthorized;
    } else {
        hit.relationship = ExternalHitRelationship::Hostile;
    }
    if (!Finite(hit.damage) || hit.damage <= 0.0f) {
        action.reason = friendly
            ? RejectReason::FriendlyFireBlocked
            : RejectReason::NoValidatedHit;
        return result;
    }

    result.hit = hit;
    action.outcome = ActionOutcome::AcceptedHit;
    action.reason = RejectReason::None;
    return result;
}

ExternalDamageResult Authority::ApplyExternalDamage(
    const ExternalDamageRequest& request) {
    ExternalDamageResult result;
    result.sourceId = request.sourceId;
    result.sourceTeam = request.sourceTeam;
    result.targetId = request.targetId;

    if (!m_configValid) {
        result.reason = RejectReason::InvalidConfiguration;
        return result;
    }
    const bool validHitZone = request.hitZone == HitZone::Head ||
        request.hitZone == HitZone::UpperTorso ||
        request.hitZone == HitZone::LowerTorso ||
        request.hitZone == HitZone::Legs;
    if (!request.sourceId.IsBot() ||
        (request.sourceTeam != 1u && request.sourceTeam != 2u) ||
        request.targetId == 0 || request.weaponId.empty() ||
        request.weaponId.size() > 128u || !IsFinite(request.originUu) ||
        !IsFinite(request.impactUu) || !Finite(request.maxRangeUu) ||
        request.maxRangeUu <= 0.0f || !Finite(request.damage) ||
        request.damage <= 0.0f || !validHitZone) {
        result.reason = RejectReason::InvalidArgument;
        return result;
    }

    auto targetIt = m_participants.find(request.targetId);
    if (targetIt == m_participants.end()) {
        result.reason = RejectReason::UnknownTarget;
        return result;
    }
    ParticipantState& target = targetIt->second;
    if (!target.alive) {
        result.reason = RejectReason::TargetDead;
        result.healthAfter = target.health;
        return result;
    }
    if (target.spec.team != 1u && target.spec.team != 2u) {
        result.reason = RejectReason::InvalidArgument;
        return result;
    }

    const float traceDistanceUu =
        request.originUu.Distance(request.impactUu);
    if (!Finite(traceDistanceUu) ||
        traceDistanceUu > request.maxRangeUu + kEpsilon) {
        result.reason = RejectReason::OutOfRange;
        return result;
    }

    // The server adapter reports the deterministic point it aimed at. Require
    // that point to remain inside the authority's current target capsule so a
    // stale/mismatched target snapshot cannot mutate health.
    const double localX = static_cast<double>(request.impactUu.x) -
        static_cast<double>(target.spec.positionUu.x);
    const double localY = static_cast<double>(request.impactUu.y) -
        static_cast<double>(target.spec.positionUu.y);
    const double localZ = static_cast<double>(request.impactUu.z) -
        static_cast<double>(target.spec.positionUu.z);
    const double radiusUu =
        static_cast<double>(target.spec.hitVolume.radiusMeters) *
        static_cast<double>(m_config.ueUnitsPerMeter);
    const double halfHeightUu =
        static_cast<double>(target.spec.hitVolume.halfHeightMeters) *
        static_cast<double>(m_config.ueUnitsPerMeter);
    const double spineHalf = std::max(0.0, halfHeightUu - radiusUu);
    const double closestSpineZ =
        std::max(-spineHalf, std::min(localZ, spineHalf));
    const double deltaZ = localZ - closestSpineZ;
    const double capsuleDistanceSquared =
        localX * localX + localY * localY + deltaZ * deltaZ;
    const double radiusSquared = radiusUu * radiusUu;
    if (!std::isfinite(capsuleDistanceSquared) ||
        !std::isfinite(radiusSquared) ||
        capsuleDistanceSquared >
            radiusSquared + static_cast<double>(kEpsilon)) {
        result.reason = RejectReason::TargetMismatch;
        return result;
    }

    result.friendlyFire = request.sourceTeam == target.spec.team;
    float damage = request.damage;
    if (result.friendlyFire) {
        if (!m_config.friendlyFireEnabled) {
            result.reason = RejectReason::FriendlyFireBlocked;
            result.healthAfter = target.health;
            return result;
        }
        damage *= m_config.friendlyFireDamageScale;
    }
    if (!Finite(damage) || damage <= 0.0f) {
        result.reason = RejectReason::InvalidArgument;
        return result;
    }

    target.health = std::max(0.0f, target.health - damage);
    result.applied = true;
    result.reason = RejectReason::None;
    result.damage = damage;
    result.healthAfter = target.health;

    if (target.health <= 0.0f && target.alive) {
        target.alive = false;
        for (auto& [weaponId, weapon] : target.weapons) {
            (void)weaponId;
            weapon.reloading = false;
            weapon.reloadCompletesAtSeconds = 0.0;
        }
        if (!target.deathEventEmitted) {
            target.deathEventEmitted = true;
            SaturatingIncrement(target.deaths);
            result.killed = true;
        }
    }
    return result;
}

SpawnResult Authority::SpawnProjectile(const ProjectileRequest& request) {
    SpawnResult result;
    ActionResult& action = result.action;
    if (!m_configValid) {
        action.reason = RejectReason::InvalidConfiguration;
        return result;
    }
    const float directionLength = request.direction.Length();
    if (!IsFinite(request.originUu) || !IsFinite(request.direction) ||
        !Finite(directionLength) || directionLength <= kEpsilon ||
        !ValidateProjectileSpec(request.projectile)) {
        action.reason = RejectReason::InvalidArgument;
        return result;
    }
    auto shooterIt = m_participants.find(request.shooterId);
    if (shooterIt == m_participants.end()) {
        action.reason = RejectReason::UnknownParticipant;
        return result;
    }
    ParticipantState& shooter = shooterIt->second;
    if (!shooter.alive) {
        action.reason = RejectReason::ParticipantDead;
        return result;
    }
    WeaponState* weapon = ResolveWeapon(shooter, request.weaponId);
    if (weapon == nullptr) {
        action.reason = RejectReason::NoWeapon;
        return result;
    }
    const std::optional<ProjectileId> projectileId =
        FindAvailableProjectileId();
    if (!projectileId) {
        action.reason = RejectReason::ResourceExhausted;
        return result;
    }

    // Preserve the complete weapon transaction in the impossible-but-checked
    // event that the preflighted identifier cannot be inserted. In particular,
    // capacity/collision failures must not consume ammo or cadence.
    const WeaponState weaponBefore = *weapon;
    const size_t eventsBefore = action.events.size();
    const RejectReason authorization =
        AuthorizeWeaponShot(shooter, *weapon, request.originUu, action.events);
    if (authorization != RejectReason::None) {
        action.reason = authorization;
        return result;
    }

    ProjectileState projectile;
    projectile.id = *projectileId;
    projectile.shooterId = request.shooterId;
    projectile.shooterTeam = shooter.spec.team;
    projectile.weaponId = weapon->profile.id;
    projectile.positionUu = request.originUu;
    projectile.velocityMetersPerSecond =
        request.direction.Normalized() *
        request.projectile.launchSpeedMetersPerSecond;
    projectile.spec = request.projectile;
    const auto inserted = m_projectiles.emplace(projectile.id, projectile);
    if (!inserted.second) {
        *weapon = weaponBefore;
        action.events.resize(eventsBefore);
        action.reason = RejectReason::ResourceExhausted;
        return result;
    }
    m_nextProjectileId = NextProjectileId(*projectileId);
    result.projectileId = *projectileId;

    EmitWeaponFired(shooter, *weapon, request.originUu, *projectileId,
                    action.events);
    CombatEvent spawned;
    spawned.kind = EventKind::ProjectileSpawned;
    spawned.sourceId = request.shooterId;
    spawned.sourceTeam = projectile.shooterTeam;
    spawned.projectileId = *projectileId;
    spawned.weaponId = inserted.first->second.weaponId;
    spawned.positionUu = inserted.first->second.positionUu;
    action.events.push_back(std::move(spawned));
    action.outcome = ActionOutcome::Accepted;
    return result;
}

std::optional<ProjectileSnapshot> Authority::GetProjectile(
    ProjectileId id) const {
    const auto it = m_projectiles.find(id);
    if (it == m_projectiles.end()) return std::nullopt;
    ProjectileSnapshot snapshot;
    snapshot.id = it->second.id;
    snapshot.shooterId = it->second.shooterId;
    snapshot.shooterTeam = it->second.shooterTeam;
    snapshot.weaponId = it->second.weaponId;
    snapshot.positionUu = it->second.positionUu;
    snapshot.velocityMetersPerSecond = it->second.velocityMetersPerSecond;
    snapshot.ageSeconds = it->second.ageSeconds;
    snapshot.spec = it->second.spec;
    snapshot.groundImpacts = it->second.groundImpacts;
    snapshot.settledOnGround = it->second.settledOnGround;
    return snapshot;
}

std::vector<CombatEvent> Authority::RemoveAllProjectiles() {
    std::vector<CombatEvent> events;
    events.reserve(m_projectiles.size());
    for (const auto& [id, projectile] : m_projectiles) {
        (void)id;
        CombatEvent removed;
        removed.kind = EventKind::ProjectileRemoved;
        removed.sourceId = projectile.shooterId;
        removed.sourceTeam = projectile.shooterTeam;
        removed.projectileId = projectile.id;
        removed.weaponId = projectile.weaponId;
        removed.positionUu = projectile.positionUu;
        events.push_back(std::move(removed));
    }
    m_projectiles.clear();
    return events;
}

float Authority::DistanceFromCapsuleMeters(
    const Vector3& pointUu, const ParticipantState& participant) const {
    const float radiusUu = MetersToUu(participant.spec.hitVolume.radiusMeters);
    const float halfHeightUu =
        MetersToUu(participant.spec.hitVolume.halfHeightMeters);
    const float spineHalf = std::max(0.0f, halfHeightUu - radiusUu);
    const Vector3& center = participant.spec.positionUu;
    const Vector3 nearest(
        center.x, center.y,
        Clamp(pointUu.z, center.z - spineHalf, center.z + spineHalf));
    return UuToMeters(std::max(0.0f, pointUu.Distance(nearest) - radiusUu));
}

void Authority::DetonateProjectile(const ProjectileState& projectile,
                                   const Vector3& positionUu,
                                   std::vector<CombatEvent>& events) {
    CombatEvent detonated;
    detonated.kind = EventKind::ProjectileDetonated;
    detonated.sourceId = projectile.shooterId;
    detonated.sourceTeam = projectile.shooterTeam;
    detonated.projectileId = projectile.id;
    detonated.weaponId = projectile.weaponId;
    detonated.positionUu = positionUu;
    detonated.explosionRadiusMeters =
        projectile.spec.explosionRadiusMeters;
    detonated.maxExplosionDamage = projectile.spec.maxExplosionDamage;
    events.push_back(std::move(detonated));

    if (projectile.spec.explosionRadiusMeters <= 0.0f ||
        projectile.spec.maxExplosionDamage <= 0.0f) {
        return;
    }
    for (auto& [id, participant] : m_participants) {
        if (!participant.alive) continue;
        const float distance = DistanceFromCapsuleMeters(positionUu, participant);
        if (distance > projectile.spec.explosionRadiusMeters) continue;
        const float alpha = Clamp(
            distance / projectile.spec.explosionRadiusMeters, 0.0f, 1.0f);
        const float damage = projectile.spec.maxExplosionDamage * (1.0f - alpha);
        ApplyDamage(projectile.shooterId, id, projectile.weaponId, damage,
                    HitZone::LowerTorso, positionUu, projectile.id, events,
                    projectile.shooterTeam);
    }
}

AdvanceResult Authority::Advance(float deltaSeconds) {
    AdvanceResult result;
    if (!m_configValid) {
        result.reason = RejectReason::InvalidConfiguration;
        return result;
    }
    // The projectile loop intentionally treats kEpsilon as no remaining work.
    // Reject a positive delta in that interval before the global clock moves,
    // otherwise reload/cadence time advances while every projectile clock is
    // left behind.
    if (!Finite(deltaSeconds) || deltaSeconds < 0.0f ||
        (deltaSeconds > 0.0f && deltaSeconds <= kEpsilon) ||
        deltaSeconds > m_config.maxAdvanceSeconds) {
        result.reason = RejectReason::InvalidArgument;
        return result;
    }
    // A zero-duration authority tick is an observation, not an opportunity to
    // complete timers or synthesize t=0 collision contacts.
    if (deltaSeconds == 0.0f) {
        result.advanced = true;
        return result;
    }
    m_timeSeconds += static_cast<double>(deltaSeconds);
    for (auto& [id, participant] : m_participants) {
        (void)id;
        for (auto& [weaponId, weapon] : participant.weapons) {
            (void)weaponId;
            CompleteReloadIfDue(participant, weapon, result.events);
        }
    }

    std::vector<ProjectileId> remove;
    for (auto& [id, projectile] : m_projectiles) {
        bool removed = false;
        float remaining = deltaSeconds;
        while (!removed && remaining > kEpsilon) {
            float step = std::min(remaining, m_config.projectileMaxStepSeconds);
            if (projectile.spec.fuseSeconds > 0.0f) {
                step = std::min(step, std::max(
                    0.0f, projectile.spec.fuseSeconds - projectile.ageSeconds));
            }
            step = std::min(step, std::max(
                0.0f, projectile.spec.maxLifetimeSeconds - projectile.ageSeconds));

            if (step <= kEpsilon) {
                if (projectile.spec.fuseSeconds > 0.0f &&
                    projectile.ageSeconds + kEpsilon >=
                        projectile.spec.fuseSeconds) {
                    DetonateProjectile(projectile, projectile.positionUu,
                                       result.events);
                } else {
                    CombatEvent expired;
                    expired.kind = EventKind::ProjectileRemoved;
                    expired.sourceId = projectile.shooterId;
                    expired.sourceTeam = projectile.shooterTeam;
                    expired.projectileId = projectile.id;
                    expired.weaponId = projectile.weaponId;
                    expired.positionUu = projectile.positionUu;
                    result.events.push_back(std::move(expired));
                }
                removed = true;
                break;
            }

            if (projectile.settledOnGround) {
                projectile.ageSeconds += step;
                remaining -= step;
            } else {
                const Vector3 previous = projectile.positionUu;
                const Vector3 gravity =
                    projectile.spec.gravityMetersPerSecondSquared;
                const Vector3 next = previous +
                    projectile.velocityMetersPerSecond *
                        (step * m_config.ueUnitsPerMeter) +
                    gravity * (0.5f * step * step * m_config.ueUnitsPerMeter);
                const Vector3 nextVelocity =
                    projectile.velocityMetersPerSecond + gravity * step;
                if (!IsFinite(next) || !IsFinite(nextVelocity)) {
                    CombatEvent invalid;
                    invalid.kind = EventKind::ProjectileRemoved;
                    invalid.sourceId = projectile.shooterId;
                    invalid.sourceTeam = projectile.shooterTeam;
                    invalid.projectileId = projectile.id;
                    invalid.weaponId = projectile.weaponId;
                    invalid.positionUu = projectile.positionUu;
                    result.events.push_back(std::move(invalid));
                    removed = true;
                    break;
                }

                const float groundZ = projectile.spec.groundHeightUu +
                    MetersToUu(projectile.spec.collisionRadiusMeters);
                float groundTime = 0.0f;
                const bool hitGround = FindGroundImpactTime(
                    previous.z, projectile.velocityMetersPerSecond.z,
                    gravity.z, groundZ, step, m_config.ueUnitsPerMeter,
                    groundTime);
                const float groundFraction = hitGround
                    ? Clamp(groundTime / step, 0.0f, 1.0f)
                    : std::numeric_limits<float>::infinity();

                SegmentHit participantHit;
                if (projectile.spec.detonateOnParticipant ||
                    projectile.spec.directDamage > 0.0f) {
                    const ParticipantId ignored =
                        projectile.ageSeconds <
                            projectile.spec.ownerCollisionGraceSeconds
                        ? projectile.shooterId
                        : kNoTeam;
                    participantHit =
                        FindFirstParticipantHit(
                            previous, next, ignored, std::nullopt,
                            projectile.spec.collisionRadiusMeters);
                }

                const bool hitParticipant = participantHit.hit &&
                    participantHit.fraction <= groundFraction;
                if (hitParticipant) {
                    const float used = step * participantHit.fraction;
                    projectile.positionUu = participantHit.positionUu;
                    projectile.velocityMetersPerSecond =
                        projectile.velocityMetersPerSecond + gravity * used;
                    projectile.ageSeconds += used;
                    remaining -= used;
                    if (projectile.spec.detonateOnParticipant &&
                        projectile.spec.explosionRadiusMeters > 0.0f) {
                        DetonateProjectile(projectile, projectile.positionUu,
                                           result.events);
                    } else if (projectile.spec.directDamage > 0.0f) {
                        const ParticipantState& target =
                            m_participants.at(participantHit.participantId);
                        const HitZone zone =
                            DetermineHitZone(target, participantHit.positionUu);
                        ApplyDamage(projectile.shooterId,
                                    participantHit.participantId,
                                    projectile.weaponId,
                                    projectile.spec.directDamage *
                                        ZoneMultiplier(zone), zone,
                                    participantHit.positionUu, projectile.id,
                                    result.events, projectile.shooterTeam);
                        CombatEvent impact;
                        impact.kind = EventKind::ProjectileRemoved;
                        impact.sourceId = projectile.shooterId;
                        impact.sourceTeam = projectile.shooterTeam;
                        impact.targetId = participantHit.participantId;
                        impact.projectileId = projectile.id;
                        impact.weaponId = projectile.weaponId;
                        impact.positionUu = participantHit.positionUu;
                        result.events.push_back(std::move(impact));
                    } else {
                        CombatEvent impact;
                        impact.kind = EventKind::ProjectileRemoved;
                        impact.sourceId = projectile.shooterId;
                        impact.sourceTeam = projectile.shooterTeam;
                        impact.targetId = participantHit.participantId;
                        impact.projectileId = projectile.id;
                        impact.weaponId = projectile.weaponId;
                        impact.positionUu = participantHit.positionUu;
                        result.events.push_back(std::move(impact));
                    }
                    removed = true;
                } else if (hitGround) {
                    const float used = groundTime;
                    projectile.positionUu = previous +
                        projectile.velocityMetersPerSecond *
                            (used * m_config.ueUnitsPerMeter) +
                        gravity * (0.5f * used * used *
                                   m_config.ueUnitsPerMeter);
                    projectile.positionUu.z = groundZ;
                    projectile.velocityMetersPerSecond =
                        projectile.velocityMetersPerSecond + gravity * used;
                    projectile.ageSeconds += used;
                    remaining -= used;
                    if (projectile.spec.detonateOnGround) {
                        DetonateProjectile(projectile, projectile.positionUu,
                                           result.events);
                        removed = true;
                    } else {
                        ++projectile.groundImpacts;
                        if (projectile.spec.maxGroundImpacts == 0 ||
                            projectile.groundImpacts >=
                                projectile.spec.maxGroundImpacts) {
                            projectile.velocityMetersPerSecond = {};
                            projectile.settledOnGround = true;
                        } else {
                            // Horizontal ground has normal (0,0,1). Reflect the
                            // whole vector, damp it uniformly, then apply the
                            // source's extra vertical damping exactly once.
                            projectile.velocityMetersPerSecond.x *=
                                projectile.spec.groundBounceDamping;
                            projectile.velocityMetersPerSecond.y *=
                                projectile.spec.groundBounceDamping;
                            projectile.velocityMetersPerSecond.z *=
                                -projectile.spec.groundBounceDamping;
                            projectile.velocityMetersPerSecond.z *=
                                projectile.spec.groundBounceVerticalDamping;
                            if (!IsFinite(
                                    projectile.velocityMetersPerSecond)) {
                                CombatEvent invalid;
                                invalid.kind = EventKind::ProjectileRemoved;
                                invalid.sourceId = projectile.shooterId;
                                invalid.sourceTeam = projectile.shooterTeam;
                                invalid.projectileId = projectile.id;
                                invalid.weaponId = projectile.weaponId;
                                invalid.positionUu = projectile.positionUu;
                                result.events.push_back(std::move(invalid));
                                removed = true;
                            }
                        }
                    }
                } else {
                    projectile.positionUu = next;
                    projectile.velocityMetersPerSecond = nextVelocity;
                    projectile.ageSeconds += step;
                    remaining -= step;
                }
            }

            if (!removed && projectile.spec.fuseSeconds > 0.0f &&
                projectile.ageSeconds + kEpsilon >= projectile.spec.fuseSeconds) {
                DetonateProjectile(projectile, projectile.positionUu,
                                   result.events);
                removed = true;
            }
            if (!removed && projectile.ageSeconds + kEpsilon >=
                projectile.spec.maxLifetimeSeconds) {
                CombatEvent expired;
                expired.kind = EventKind::ProjectileRemoved;
                expired.sourceId = projectile.shooterId;
                expired.sourceTeam = projectile.shooterTeam;
                expired.projectileId = projectile.id;
                expired.weaponId = projectile.weaponId;
                expired.positionUu = projectile.positionUu;
                result.events.push_back(std::move(expired));
                removed = true;
            }
        }
        if (removed) remove.push_back(id);
    }
    for (ProjectileId id : remove) m_projectiles.erase(id);
    result.advanced = true;
    return result;
}

} // namespace CombatAuthority
