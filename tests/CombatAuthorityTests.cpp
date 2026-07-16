#include "TestFramework.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

#include "Game/BotManager.h"
#include "Game/CombatAuthority.h"

namespace CombatAuthority {

struct AuthorityTestAccess {
    static void SetNextProjectileId(Authority& authority, ProjectileId id) {
        authority.m_nextProjectileId = id;
    }

    static bool SetCounters(Authority& authority, ParticipantId id,
                            uint32_t kills, uint32_t deaths) {
        const auto found = authority.m_participants.find(id);
        if (found == authority.m_participants.end()) return false;
        found->second.kills = kills;
        found->second.deaths = deaths;
        return true;
    }
};

} // namespace CombatAuthority

namespace {

using namespace CombatAuthority;

ParticipantSpec Soldier(CombatAuthority::ParticipantId id, uint32_t team,
                         const Vector3& positionUu, float health = 100.0f) {
    ParticipantSpec participant;
    participant.id = id;
    participant.team = team;
    participant.positionUu = positionUu;
    participant.maxHealth = health;
    participant.health = health;
    participant.hitVolume = {0.4f, 0.9f};
    return participant;
}

WeaponProfile Rifle(float damage = 30.0f, uint32_t magazine = 10,
                    uint32_t reserve = 20) {
    WeaponProfile weapon;
    weapon.id = "M16A1";
    weapon.magazineCapacity = magazine;
    weapon.initialMagazine = magazine;
    weapon.initialReserve = reserve;
    weapon.roundsPerMinute = 600.0f;
    weapon.reloadSeconds = 0.5f;
    weapon.maxRangeMeters = 100.0f;
    weapon.baseDamage = damage;
    weapon.minDamageFractionAtMaxRange = 1.0f;
    return weapon;
}

WeaponProfile Grenade(uint32_t magazine = 1, uint32_t reserve = 2) {
    WeaponProfile weapon;
    weapon.id = "M61";
    weapon.magazineCapacity = magazine;
    weapon.initialMagazine = magazine;
    weapon.initialReserve = reserve;
    weapon.roundsPerMinute = 30.0f;
    weapon.reloadSeconds = 1.0f;
    weapon.maxRangeMeters = 50.0f;
    weapon.baseDamage = 0.0f;
    weapon.minDamageFractionAtMaxRange = 1.0f;
    return weapon;
}

ProjectileRequest PersistentGrenadeRequest() {
    ProjectileRequest request;
    request.shooterId = 1;
    request.originUu = {};
    request.direction = {1.0f, 0.0f, 0.0f};
    request.weaponId = "M61";
    request.projectile.launchSpeedMetersPerSecond = 1.0f;
    request.projectile.gravityMetersPerSecondSquared = {};
    request.projectile.fuseSeconds = 0.0f;
    request.projectile.maxLifetimeSeconds = 10.0f;
    request.projectile.collisionRadiusMeters = 0.05f;
    request.projectile.groundHeightUu = -1000.0f;
    request.projectile.detonateOnGround = false;
    request.projectile.detonateOnParticipant = false;
    return request;
}

HitscanRequest TorsoShot(CombatAuthority::ParticipantId target = 2,
                          float targetX = 500.0f) {
    HitscanRequest request;
    request.shooterId = 1;
    request.claimedTargetId = target;
    request.startTraceUu = {0.0f, 0.0f, 0.0f};
    request.endTraceUu = {targetX, 0.0f, 0.0f};
    request.reportedImpactUu = {targetX - 20.0f, 0.0f, 0.0f};
    return request;
}

ExternalParticipantSpec ExternalBot(
    uint32_t rawId, uint32_t team, const Vector3& positionUu,
    bool alive = true) {
    ExternalParticipantSpec target;
    target.id = ::ParticipantId::Bot(rawId);
    target.team = team;
    target.positionUu = positionUu;
    target.hitVolume = {0.4f, 0.9f};
    target.alive = alive;
    return target;
}

ExternalHitscanRequest ExternalBotShot(
    uint32_t rawTargetId = 7, float targetX = 500.0f) {
    ExternalHitscanRequest request;
    request.shooterId = 1;
    request.claimedTargetId = ::ParticipantId::Bot(rawTargetId);
    request.startTraceUu = {0.0f, 0.0f, 0.0f};
    request.endTraceUu = {targetX, 0.0f, 0.0f};
    request.reportedImpactUu = {targetX - 20.0f, 0.0f, 0.0f};
    request.targets.push_back(
        ExternalBot(rawTargetId, 2, {targetX, 0.0f, 0.0f}));
    return request;
}

size_t CountEvents(const std::vector<CombatEvent>& events, EventKind kind) {
    return static_cast<size_t>(std::count_if(
        events.begin(), events.end(),
        [kind](const CombatEvent& event) { return event.kind == kind; }));
}

Authority TwoPlayerWorld(uint32_t shooterTeam = 1, uint32_t targetTeam = 2,
                          const AuthorityConfig& config = {}) {
    Authority authority(config);
    authority.AddParticipant(Soldier(1, shooterTeam, {0.0f, 0.0f, 0.0f}));
    authority.AddParticipant(Soldier(2, targetTeam, {500.0f, 0.0f, 0.0f}));
    authority.EquipWeapon(1, Rifle());
    return authority;
}

Authority ExternalWorld(uint32_t shooterTeam = 1,
                        const AuthorityConfig& config = {}) {
    Authority authority(config);
    authority.AddParticipant(Soldier(1, shooterTeam, {}));
    authority.EquipWeapon(1, Rifle());
    return authority;
}

} // namespace

TEST(CombatAuthority, ExplicitUnitBoundaryAndConfigurationValidation) {
    Authority authority;
    ASSERT_TRUE(authority.IsConfigurationValid());
    EXPECT_FLOAT_EQ(authority.MetersToUu(10.0f), 500.0f);
    EXPECT_FLOAT_EQ(authority.UuToMeters(500.0f), 10.0f);

    AuthorityConfig invalid;
    invalid.ueUnitsPerMeter = 0.0f;
    Authority rejected(invalid);
    EXPECT_FALSE(rejected.IsConfigurationValid());
    EXPECT_FALSE(rejected.AddParticipant(Soldier(1, 1, {})));

    AuthorityConfig boundedWork;
    boundedWork.maxAdvanceSeconds = 1.0f;
    boundedWork.projectileMaxStepSeconds = 1.0f / 4096.0f;
    EXPECT_TRUE(Authority(boundedWork).IsConfigurationValid());
    boundedWork.projectileMaxStepSeconds = 1.0f / 8192.0f;
    EXPECT_FALSE(Authority(boundedWork).IsConfigurationValid());
    boundedWork.maxAdvanceSeconds = 1.0e-6f;
    boundedWork.projectileMaxStepSeconds = 1.0e-6f;
    EXPECT_FALSE(Authority(boundedWork).IsConfigurationValid());

    AuthorityConfig invalidCapacity;
    invalidCapacity.maxActiveProjectiles = 0;
    EXPECT_FALSE(Authority(invalidCapacity).IsConfigurationValid());
    invalidCapacity.maxActiveProjectiles = kHardMaxActiveProjectiles + 1u;
    EXPECT_FALSE(Authority(invalidCapacity).IsConfigurationValid());
}

TEST(CombatAuthority, RejectsSentinelParticipantAndOverflowingGeometry) {
    Authority authority;
    EXPECT_FALSE(authority.AddParticipant(Soldier(0, 1, {})));

    ParticipantSpec enormous = Soldier(1, 1, {});
    enormous.hitVolume.radiusMeters = std::numeric_limits<float>::max();
    enormous.hitVolume.halfHeightMeters =
        std::numeric_limits<float>::max();
    EXPECT_FALSE(authority.AddParticipant(enormous));

    ASSERT_TRUE(authority.AddParticipant(Soldier(1, 1, {})));
    WeaponProfile grenade = Grenade(2, 0);
    grenade.roundsPerMinute = 600.0f;
    ASSERT_TRUE(authority.EquipWeapon(1, grenade));
    ProjectileRequest request = PersistentGrenadeRequest();
    request.projectile.collisionRadiusMeters =
        std::numeric_limits<float>::max();
    const SpawnResult rejected = authority.SpawnProjectile(request);
    EXPECT_FALSE(rejected.action.accepted());
    EXPECT_EQ(rejected.action.reason, RejectReason::InvalidArgument);
    EXPECT_EQ(authority.GetProjectileCount(), static_cast<size_t>(0));
    const auto participant = authority.GetParticipant(1);
    ASSERT_TRUE(participant.has_value());
    ASSERT_TRUE(participant->weapon.has_value());
    EXPECT_EQ(participant->weapon->magazineAmmo, 2u);
}

TEST(CombatAuthority, RejectsFiniteDamageProductsThatOverflowFloat) {
    Authority authority;
    ASSERT_TRUE(authority.AddParticipant(Soldier(1, 1, {})));

    WeaponProfile overflowingRifle =
        Rifle(std::numeric_limits<float>::max());
    EXPECT_FALSE(authority.EquipWeapon(1, overflowingRifle));
    const auto withoutWeapon = authority.GetParticipant(1);
    ASSERT_TRUE(withoutWeapon.has_value());
    EXPECT_TRUE(withoutWeapon->weapons.empty());

    WeaponProfile grenade = Grenade(2, 0);
    grenade.roundsPerMinute = 600.0f;
    ASSERT_TRUE(authority.EquipWeapon(1, grenade));
    ProjectileRequest request = PersistentGrenadeRequest();
    request.projectile.directDamage =
        std::numeric_limits<float>::max();

    const SpawnResult rejected = authority.SpawnProjectile(request);
    EXPECT_FALSE(rejected.action.accepted());
    EXPECT_EQ(rejected.action.reason, RejectReason::InvalidArgument);
    EXPECT_EQ(authority.GetProjectileCount(), static_cast<size_t>(0));
    const auto after = authority.GetParticipant(1);
    ASSERT_TRUE(after.has_value());
    ASSERT_TRUE(after->weapon.has_value());
    EXPECT_EQ(after->weapon->magazineAmmo, 2u);

    request.projectile.directDamage = 0.0f;
    request.projectile.maxExplosionDamage =
        std::numeric_limits<float>::max();
    const SpawnResult rejectedExplosion = authority.SpawnProjectile(request);
    EXPECT_FALSE(rejectedExplosion.action.accepted());
    EXPECT_EQ(rejectedExplosion.action.reason, RejectReason::InvalidArgument);
    EXPECT_EQ(authority.GetProjectileCount(), static_cast<size_t>(0));
    const auto afterExplosion = authority.GetParticipant(1);
    ASSERT_TRUE(afterExplosion.has_value());
    ASSERT_TRUE(afterExplosion->weapon.has_value());
    EXPECT_EQ(afterExplosion->weapon->magazineAmmo, 2u);
}

TEST(CombatAuthority, RejectsSubEpsilonAdvanceWithoutAdvancingAnyClock) {
    Authority authority;
    ASSERT_TRUE(authority.AddParticipant(Soldier(1, 1, {})));
    WeaponProfile grenade = Grenade(1, 0);
    grenade.roundsPerMinute = 600.0f;
    ASSERT_TRUE(authority.EquipWeapon(1, grenade));
    const SpawnResult spawned =
        authority.SpawnProjectile(PersistentGrenadeRequest());
    ASSERT_TRUE(spawned.action.accepted());

    const AdvanceResult rejected = authority.Advance(1.0e-6f);
    EXPECT_FALSE(rejected.advanced);
    EXPECT_EQ(rejected.reason, RejectReason::InvalidArgument);
    EXPECT_EQ(authority.GetTimeSeconds(), 0.0);
    const auto projectile = authority.GetProjectile(spawned.projectileId);
    ASSERT_TRUE(projectile.has_value());
    EXPECT_FLOAT_EQ(projectile->ageSeconds, 0.0f);
}

TEST(CombatAuthority, ProjectileCapacityFailureDoesNotConsumeWeaponState) {
    AuthorityConfig config;
    config.maxActiveProjectiles = 1;
    Authority authority(config);
    ASSERT_TRUE(authority.AddParticipant(Soldier(1, 1, {})));
    WeaponProfile grenade = Grenade(2, 0);
    grenade.roundsPerMinute = 600.0f;
    ASSERT_TRUE(authority.EquipWeapon(1, grenade));
    const ProjectileRequest request = PersistentGrenadeRequest();

    const SpawnResult first = authority.SpawnProjectile(request);
    ASSERT_TRUE(first.action.accepted());
    ASSERT_TRUE(authority.Advance(0.1f).advanced);
    const auto before = authority.GetParticipant(1);
    ASSERT_TRUE(before.has_value());
    ASSERT_TRUE(before->weapon.has_value());

    const SpawnResult rejected = authority.SpawnProjectile(request);
    EXPECT_FALSE(rejected.action.accepted());
    EXPECT_EQ(rejected.action.reason, RejectReason::ResourceExhausted);
    EXPECT_EQ(rejected.projectileId, 0u);
    EXPECT_TRUE(rejected.action.events.empty());
    EXPECT_EQ(authority.GetProjectileCount(), static_cast<size_t>(1));
    const auto after = authority.GetParticipant(1);
    ASSERT_TRUE(after.has_value());
    ASSERT_TRUE(after->weapon.has_value());
    EXPECT_EQ(after->weapon->magazineAmmo, before->weapon->magazineAmmo);
    EXPECT_FLOAT_EQ(static_cast<float>(after->weapon->reloadCompletesAtSeconds),
                    static_cast<float>(before->weapon->reloadCompletesAtSeconds));

    EXPECT_EQ(authority.RemoveAllProjectiles().size(), static_cast<size_t>(1));
    const SpawnResult reused = authority.SpawnProjectile(request);
    EXPECT_TRUE(reused.action.accepted());
    EXPECT_NE(reused.projectileId, 0u);
}

TEST(CombatAuthority, ProjectileIdentifiersWrapWithoutZeroOrCollision) {
    AuthorityConfig config;
    config.maxActiveProjectiles = 4;
    Authority authority(config);
    ASSERT_TRUE(authority.AddParticipant(Soldier(1, 1, {})));
    WeaponProfile grenade = Grenade(3, 0);
    grenade.roundsPerMinute = 600.0f;
    ASSERT_TRUE(authority.EquipWeapon(1, grenade));
    AuthorityTestAccess::SetNextProjectileId(
        authority, std::numeric_limits<ProjectileId>::max());
    const ProjectileRequest request = PersistentGrenadeRequest();

    const SpawnResult last = authority.SpawnProjectile(request);
    ASSERT_TRUE(last.action.accepted());
    EXPECT_EQ(last.projectileId, std::numeric_limits<ProjectileId>::max());
    ASSERT_TRUE(authority.Advance(0.1f).advanced);
    const SpawnResult wrapped = authority.SpawnProjectile(request);
    ASSERT_TRUE(wrapped.action.accepted());
    EXPECT_EQ(wrapped.projectileId, 1u);
    EXPECT_NE(wrapped.projectileId, last.projectileId);
    AuthorityTestAccess::SetNextProjectileId(
        authority, std::numeric_limits<ProjectileId>::max());
    ASSERT_TRUE(authority.Advance(0.1f).advanced);
    const SpawnResult skippedCollision = authority.SpawnProjectile(request);
    ASSERT_TRUE(skippedCollision.action.accepted());
    EXPECT_EQ(skippedCollision.projectileId, 2u);
    EXPECT_EQ(authority.GetProjectileCount(), static_cast<size_t>(3));
}

TEST(CombatAuthority, CombatCountersSaturateAcrossLethalDamage) {
    Authority authority = TwoPlayerWorld();
    ASSERT_TRUE(authority.EquipWeapon(1, Rifle(200.0f)));
    ASSERT_TRUE(AuthorityTestAccess::SetCounters(
        authority, 1, std::numeric_limits<uint32_t>::max(), 0));
    ASSERT_TRUE(AuthorityTestAccess::SetCounters(
        authority, 2, 0, std::numeric_limits<uint32_t>::max()));

    const ActionResult result = authority.FireHitscan(TorsoShot());
    ASSERT_TRUE(result.confirmedHit());
    EXPECT_EQ(CountEvents(result.events, EventKind::ParticipantDied),
              static_cast<size_t>(1));
    const auto shooter = authority.GetParticipant(1);
    const auto target = authority.GetParticipant(2);
    ASSERT_TRUE(shooter.has_value());
    ASSERT_TRUE(target.has_value());
    EXPECT_EQ(shooter->kills, std::numeric_limits<uint32_t>::max());
    EXPECT_EQ(target->deaths, std::numeric_limits<uint32_t>::max());
}

TEST(CombatAuthority, HitscanConsumesAmmoAndEnforcesCadence) {
    Authority authority = TwoPlayerWorld();
    const ActionResult first = authority.FireHitscan(TorsoShot());
    ASSERT_TRUE(first.confirmedHit());
    EXPECT_EQ(CountEvents(first.events, EventKind::WeaponFired), 1u);
    EXPECT_EQ(CountEvents(first.events, EventKind::DamageApplied), 1u);

    const ActionResult tooSoon = authority.FireHitscan(TorsoShot());
    EXPECT_FALSE(tooSoon.accepted());
    EXPECT_EQ(tooSoon.reason, RejectReason::Cadence);
    auto shooter = authority.GetParticipant(1);
    ASSERT_TRUE(shooter && shooter->weapon);
    EXPECT_EQ(shooter->weapon->magazineAmmo, 9u);

    ASSERT_TRUE(authority.Advance(0.1f).advanced);
    EXPECT_TRUE(authority.FireHitscan(TorsoShot()).confirmedHit());
    shooter = authority.GetParticipant(1);
    ASSERT_TRUE(shooter && shooter->weapon);
    EXPECT_EQ(shooter->weapon->magazineAmmo, 8u);
}

TEST(CombatAuthority, ReloadTransfersOnlyAvailableReserveAtDeadline) {
    Authority authority = TwoPlayerWorld();
    WeaponProfile weapon = Rifle(10.0f, 1, 2);
    ASSERT_TRUE(authority.EquipWeapon(1, weapon));
    ASSERT_TRUE(authority.FireHitscan(TorsoShot()).accepted());

    const ActionResult reload = authority.RequestReload(1);
    ASSERT_TRUE(reload.accepted());
    EXPECT_EQ(CountEvents(reload.events, EventKind::ReloadStarted), 1u);
    EXPECT_TRUE(authority.Advance(0.49f).advanced);
    auto state = authority.GetParticipant(1);
    ASSERT_TRUE(state && state->weapon);
    EXPECT_TRUE(state->weapon->reloading);
    EXPECT_EQ(state->weapon->magazineAmmo, 0u);

    const AdvanceResult completed = authority.Advance(0.01f);
    EXPECT_EQ(CountEvents(completed.events, EventKind::ReloadCompleted), 1u);
    state = authority.GetParticipant(1);
    ASSERT_TRUE(state && state->weapon);
    EXPECT_FALSE(state->weapon->reloading);
    EXPECT_EQ(state->weapon->magazineAmmo, 1u);
    EXPECT_EQ(state->weapon->reserveAmmo, 1u);
}

TEST(CombatAuthority, NearestCapsuleMustMatchClaimedTarget) {
    Authority authority = TwoPlayerWorld();
    ASSERT_TRUE(authority.AddParticipant(
        Soldier(3, 2, {250.0f, 0.0f, 0.0f})));
    const ActionResult result = authority.FireHitscan(TorsoShot());
    EXPECT_TRUE(result.accepted());
    EXPECT_FALSE(result.confirmedHit());
    EXPECT_EQ(result.reason, RejectReason::TargetMismatch);
    EXPECT_FLOAT_EQ(authority.GetParticipant(2)->health, 100.0f);
    EXPECT_FLOAT_EQ(authority.GetParticipant(3)->health, 100.0f);
    EXPECT_EQ(authority.GetParticipant(1)->weapon->magazineAmmo, 9u);
}

TEST(CombatAuthority, RejectsForgedImpactAwayFromValidatedCapsuleEntry) {
    Authority authority = TwoPlayerWorld();
    HitscanRequest request = TorsoShot();
    request.reportedImpactUu = {100.0f, 500.0f, 500.0f};
    const ActionResult result = authority.FireHitscan(request);
    EXPECT_TRUE(result.accepted());
    EXPECT_FALSE(result.confirmedHit());
    EXPECT_EQ(result.reason, RejectReason::ReportedImpactMismatch);
    EXPECT_FLOAT_EQ(authority.GetParticipant(2)->health, 100.0f);
}

TEST(CombatAuthority, InvalidOriginDoesNotConsumeAmmo) {
    Authority authority = TwoPlayerWorld();
    HitscanRequest request = TorsoShot();
    request.startTraceUu = {1000.0f, 0.0f, 0.0f};
    request.endTraceUu = {500.0f, 0.0f, 0.0f};
    const ActionResult result = authority.FireHitscan(request);
    EXPECT_FALSE(result.accepted());
    EXPECT_EQ(result.reason, RejectReason::OriginMismatch);
    EXPECT_EQ(authority.GetParticipant(1)->weapon->magazineAmmo, 10u);
}

TEST(CombatAuthority, FriendlyFireCanBlockOrScaleDamage) {
    Authority blocked = TwoPlayerWorld(1, 1);
    const ActionResult noDamage = blocked.FireHitscan(TorsoShot());
    EXPECT_TRUE(noDamage.accepted());
    EXPECT_EQ(noDamage.reason, RejectReason::FriendlyFireBlocked);
    EXPECT_EQ(CountEvents(noDamage.events, EventKind::FriendlyFireBlocked), 1u);
    EXPECT_FLOAT_EQ(blocked.GetParticipant(2)->health, 100.0f);

    AuthorityConfig scaledConfig;
    scaledConfig.friendlyFireEnabled = true;
    scaledConfig.friendlyFireDamageScale = 0.5f;
    Authority scaled = TwoPlayerWorld(1, 1, scaledConfig);
    const ActionResult damage = scaled.FireHitscan(TorsoShot());
    ASSERT_TRUE(damage.confirmedHit());
    EXPECT_NEAR(scaled.GetParticipant(2)->health, 85.0f, 1.0e-5f);
}

TEST(CombatAuthority, ExternalHitscanKeepsHumanAndBotRawIdsDistinct) {
    Authority authority;
    ASSERT_TRUE(authority.AddParticipant(Soldier(7, 1, {})));
    ASSERT_TRUE(authority.EquipWeapon(7, Rifle()));

    ExternalHitscanRequest request = ExternalBotShot(7);
    request.shooterId = 7;
    const ExternalHitscanResult result =
        authority.FireHitscanExternal(request);

    ASSERT_TRUE(result.action.confirmedHit());
    ASSERT_TRUE(result.hit.has_value());
    EXPECT_EQ(result.hit->participantId, ::ParticipantId::Bot(7));
    EXPECT_NE(result.hit->participantId, ::ParticipantId::Human(7));
    EXPECT_EQ(result.hit->relationship, ExternalHitRelationship::Hostile);
    EXPECT_FLOAT_EQ(result.hit->damage, 30.0f);
    EXPECT_EQ(authority.GetParticipant(7)->weapon->magazineAmmo, 9u);
    EXPECT_EQ(CountEvents(result.action.events, EventKind::WeaponFired), 1u);
    EXPECT_EQ(CountEvents(result.action.events, EventKind::DamageApplied), 0u);
}

TEST(CombatAuthority, ExternalHitscanRequiresClaimedBotToBeFirstTaggedHit) {
    {
        Authority authority = ExternalWorld();
        ASSERT_TRUE(authority.AddParticipant(
            Soldier(7, 2, {250.0f, 0.0f, 0.0f})));
        const ExternalHitscanResult result =
            authority.FireHitscanExternal(ExternalBotShot(7));
        EXPECT_TRUE(result.action.accepted());
        EXPECT_FALSE(result.action.confirmedHit());
        EXPECT_EQ(result.action.reason, RejectReason::TargetMismatch);
        EXPECT_FALSE(result.hit.has_value());
        EXPECT_EQ(authority.GetParticipant(1)->weapon->magazineAmmo, 9u);
    }
    {
        Authority authority = ExternalWorld();
        ExternalHitscanRequest request = ExternalBotShot(7);
        request.targets.push_back(
            ExternalBot(8, 2, {250.0f, 0.0f, 0.0f}));
        const ExternalHitscanResult result =
            authority.FireHitscanExternal(request);
        EXPECT_TRUE(result.action.accepted());
        EXPECT_FALSE(result.action.confirmedHit());
        EXPECT_EQ(result.action.reason, RejectReason::TargetMismatch);
        EXPECT_FALSE(result.hit.has_value());
        EXPECT_EQ(authority.GetParticipant(1)->weapon->magazineAmmo, 9u);
    }
    {
        Authority authority = ExternalWorld();
        ASSERT_TRUE(authority.AddParticipant(
            Soldier(7, 2, {500.0f, 0.0f, 0.0f})));
        const ExternalHitscanResult result =
            authority.FireHitscanExternal(ExternalBotShot(7));
        EXPECT_EQ(result.action.reason, RejectReason::TargetMismatch);
        EXPECT_FALSE(result.hit.has_value());
    }
}

TEST(CombatAuthority, ExternalHitscanRejectsInvalidRosterBeforeAmmoMutation) {
    {
        Authority authority = ExternalWorld();
        ExternalHitscanRequest request = ExternalBotShot(7);
        request.claimedTargetId = ::ParticipantId::Bot(8);
        const auto result = authority.FireHitscanExternal(request);
        EXPECT_FALSE(result.action.accepted());
        EXPECT_EQ(result.action.reason, RejectReason::UnknownTarget);
        EXPECT_EQ(authority.GetParticipant(1)->weapon->magazineAmmo, 10u);
    }
    {
        Authority authority = ExternalWorld();
        ExternalHitscanRequest request = ExternalBotShot(7);
        request.targets.front().alive = false;
        const auto result = authority.FireHitscanExternal(request);
        EXPECT_FALSE(result.action.accepted());
        EXPECT_EQ(result.action.reason, RejectReason::TargetDead);
        EXPECT_EQ(authority.GetParticipant(1)->weapon->magazineAmmo, 10u);
    }
    {
        Authority authority = ExternalWorld();
        ExternalHitscanRequest request = ExternalBotShot(7);
        request.targets.push_back(request.targets.front());
        const auto result = authority.FireHitscanExternal(request);
        EXPECT_FALSE(result.action.accepted());
        EXPECT_EQ(result.action.reason, RejectReason::InvalidArgument);
        EXPECT_EQ(authority.GetParticipant(1)->weapon->magazineAmmo, 10u);
    }
    {
        Authority authority = ExternalWorld();
        ExternalHitscanRequest request = ExternalBotShot(7);
        ExternalParticipantSpec malformed =
            ExternalBot(8, 2, {250.0f, 0.0f, 0.0f});
        malformed.positionUu.x = std::numeric_limits<float>::quiet_NaN();
        request.targets.push_back(malformed);
        const auto result = authority.FireHitscanExternal(request);
        EXPECT_FALSE(result.action.accepted());
        EXPECT_EQ(result.action.reason, RejectReason::InvalidArgument);
        EXPECT_EQ(authority.GetParticipant(1)->weapon->magazineAmmo, 10u);
    }
    {
        Authority authority = ExternalWorld();
        ExternalHitscanRequest request = ExternalBotShot(7);
        request.targets.front().hitVolume.radiusMeters =
            std::numeric_limits<float>::max();
        request.targets.front().hitVolume.halfHeightMeters =
            std::numeric_limits<float>::max();
        const auto result = authority.FireHitscanExternal(request);
        EXPECT_FALSE(result.action.accepted());
        EXPECT_EQ(result.action.reason, RejectReason::InvalidArgument);
        EXPECT_EQ(authority.GetParticipant(1)->weapon->magazineAmmo, 10u);
    }
    {
        Authority authority = ExternalWorld();
        ExternalHitscanRequest request = ExternalBotShot(7);
        ExternalParticipantSpec malformed =
            ExternalBot(8, 2, {250.0f, 0.0f, 0.0f});
        malformed.id = ::ParticipantId::Human(8);
        request.targets.push_back(malformed);
        const auto result = authority.FireHitscanExternal(request);
        EXPECT_FALSE(result.action.accepted());
        EXPECT_EQ(result.action.reason, RejectReason::InvalidArgument);
        EXPECT_EQ(authority.GetParticipant(1)->weapon->magazineAmmo, 10u);
    }
    {
        Authority authority = ExternalWorld();
        ExternalHitscanRequest request = ExternalBotShot(7);
        request.targets.clear();
        for (size_t i = 0; i <= kMaxExternalHitscanTargets; ++i) {
            request.targets.push_back(ExternalBot(
                static_cast<uint32_t>(i + 1u), 2,
                {500.0f + static_cast<float>(i), 0.0f, 0.0f}));
        }
        const auto result = authority.FireHitscanExternal(request);
        EXPECT_FALSE(result.action.accepted());
        EXPECT_EQ(result.action.reason, RejectReason::InvalidArgument);
        EXPECT_EQ(authority.GetParticipant(1)->weapon->magazineAmmo, 10u);
    }
}

TEST(CombatAuthority, ExternalImpactMismatchConsumesOneAuthorizedShot) {
    Authority authority = ExternalWorld();
    ExternalHitscanRequest request = ExternalBotShot(7);
    request.reportedImpactUu = {100.0f, 500.0f, 500.0f};
    const ExternalHitscanResult result =
        authority.FireHitscanExternal(request);

    EXPECT_TRUE(result.action.accepted());
    EXPECT_FALSE(result.action.confirmedHit());
    EXPECT_EQ(result.action.reason, RejectReason::ReportedImpactMismatch);
    EXPECT_FALSE(result.hit.has_value());
    EXPECT_EQ(authority.GetParticipant(1)->weapon->magazineAmmo, 9u);
    EXPECT_EQ(CountEvents(result.action.events, EventKind::WeaponFired), 1u);

    const ExternalHitscanResult cadence =
        authority.FireHitscanExternal(ExternalBotShot(7));
    EXPECT_FALSE(cadence.action.accepted());
    EXPECT_EQ(cadence.action.reason, RejectReason::Cadence);
    EXPECT_EQ(authority.GetParticipant(1)->weapon->magazineAmmo, 9u);
    EXPECT_EQ(CountEvents(cadence.action.events, EventKind::WeaponFired), 0u);
}

TEST(CombatAuthority, ExternalHitscanEmptyMagazineDoesNotDoubleConsume) {
    Authority authority;
    ASSERT_TRUE(authority.AddParticipant(Soldier(1, 1, {})));
    ASSERT_TRUE(authority.EquipWeapon(1, Rifle(30.0f, 1, 0)));
    ASSERT_TRUE(authority.FireHitscanExternal(
        ExternalBotShot(7)).action.confirmedHit());
    ASSERT_TRUE(authority.Advance(0.1f).advanced);

    const ExternalHitscanResult empty =
        authority.FireHitscanExternal(ExternalBotShot(7));
    EXPECT_FALSE(empty.action.accepted());
    EXPECT_EQ(empty.action.reason, RejectReason::EmptyMagazine);
    EXPECT_EQ(authority.GetParticipant(1)->weapon->magazineAmmo, 0u);
    EXPECT_EQ(CountEvents(empty.action.events, EventKind::WeaponFired), 0u);
}

TEST(CombatAuthority, ExternalFriendlyFireIsExplicitAndScaledOnce) {
    Authority blocked = ExternalWorld(1);
    ExternalHitscanRequest friendly = ExternalBotShot(7);
    friendly.targets.front().team = 1;
    const ExternalHitscanResult noDamage =
        blocked.FireHitscanExternal(friendly);
    EXPECT_TRUE(noDamage.action.accepted());
    EXPECT_EQ(noDamage.action.reason, RejectReason::FriendlyFireBlocked);
    ASSERT_TRUE(noDamage.hit.has_value());
    EXPECT_EQ(noDamage.hit->relationship,
              ExternalHitRelationship::FriendlyFireBlocked);
    EXPECT_FLOAT_EQ(noDamage.hit->damage, 0.0f);
    EXPECT_EQ(blocked.GetParticipant(1)->weapon->magazineAmmo, 9u);

    AuthorityConfig config;
    config.friendlyFireEnabled = true;
    config.friendlyFireDamageScale = 0.5f;
    Authority scaled = ExternalWorld(1, config);
    const ExternalHitscanResult damage =
        scaled.FireHitscanExternal(friendly);
    ASSERT_TRUE(damage.action.confirmedHit());
    ASSERT_TRUE(damage.hit.has_value());
    EXPECT_EQ(damage.hit->relationship,
              ExternalHitRelationship::FriendlyFireAuthorized);
    EXPECT_FLOAT_EQ(damage.hit->damage, 15.0f);
    EXPECT_EQ(scaled.GetParticipant(1)->weapon->magazineAmmo, 9u);
}

TEST(CombatAuthority, ExternalHitscanUsesRangeFalloffAndHitZoneMultiplier) {
    Authority authority;
    ASSERT_TRUE(authority.AddParticipant(Soldier(1, 1, {})));
    WeaponProfile weapon = Rifle(40.0f);
    weapon.maxRangeMeters = 100.0f;
    weapon.minDamageFractionAtMaxRange = 0.5f;
    ASSERT_TRUE(authority.EquipWeapon(1, weapon));

    ExternalHitscanRequest request = ExternalBotShot(7, 2500.0f);
    request.startTraceUu.z = 35.0f;
    request.endTraceUu.z = 35.0f;
    request.reportedImpactUu = {2483.0f, 0.0f, 35.0f};
    request.targets.front().positionUu = {2500.0f, 0.0f, 0.0f};
    const ExternalHitscanResult result =
        authority.FireHitscanExternal(request);

    ASSERT_TRUE(result.action.confirmedHit());
    ASSERT_TRUE(result.hit.has_value());
    EXPECT_EQ(result.hit->hitZone, HitZone::Head);
    const float distanceMeters =
        authority.UuToMeters(request.startTraceUu.Distance(
            result.hit->positionUu));
    const float expected =
        40.0f * (1.0f - (distanceMeters / 100.0f) * 0.5f) * 2.0f;
    EXPECT_NEAR(result.hit->damage, expected, 1.0e-4f);
}

TEST(CombatAuthority, ExternalHitCanDriveOneBotDamageAndDeathTransition) {
    BotManagerConfig botConfig;
    botConfig.fillTargetPerTeam = 1;
    botConfig.maxBotsPerTeam = 1;
    botConfig.moveSpeed = 0.0f;
    botConfig.maxHealth = 40.0f;
    BotManager bots(botConfig);
    bots.SetEligibleSpawns({
        BotSpawnSnapshot{1, BotManager::kTeamOne,
                         Vector3(-500.0f, 0.0f, 0.0f)},
        BotSpawnSnapshot{2, BotManager::kTeamTwo,
                         Vector3(500.0f, 0.0f, 0.0f)},
    });
    (void)bots.ConsumeRespawnEvents();

    Authority authority;
    ASSERT_TRUE(authority.AddParticipant(Soldier(1, 1, {})));
    ASSERT_TRUE(authority.EquipWeapon(1, Rifle(60.0f)));
    ExternalHitscanRequest request = ExternalBotShot();
    request.targets.clear();
    ::ParticipantId victim{};
    for (const BotSnapshot& bot : bots.GetBots()) {
        request.targets.push_back(ExternalParticipantSpec{
            bot.id, static_cast<uint32_t>(bot.teamId), bot.position,
            HitVolume{}, bot.lifecycle == BotLifecycle::Alive});
        if (bot.teamId == BotManager::kTeamTwo) {
            victim = bot.id;
        }
    }
    ASSERT_TRUE(victim.IsBot());
    request.claimedTargetId = victim;

    const ExternalHitscanResult result =
        authority.FireHitscanExternal(request);
    ASSERT_TRUE(result.action.confirmedHit());
    ASSERT_TRUE(result.hit.has_value());
    ASSERT_TRUE(bots.ApplyExternalDamageToBot(
        result.hit->participantId, ::ParticipantId::Human(1),
        result.hit->damage, ExternalBotDamageAuthorization::Hostile));

    const BotSnapshot* snapshot = bots.FindBot(victim);
    ASSERT_TRUE(snapshot != nullptr);
    EXPECT_FLOAT_EQ(snapshot->health, 0.0f);
    EXPECT_EQ(snapshot->lifecycle, BotLifecycle::RespawnQueued);
    EXPECT_EQ(snapshot->deathSequence, 1u);
    const ::ParticipantSnapshot* roster = bots.Roster().Find(victim);
    ASSERT_TRUE(roster != nullptr);
    EXPECT_FALSE(roster->alive);

    const std::vector<BotCombatEvent> combat = bots.ConsumeCombatEvents();
    const std::vector<BotDeathEvent> deaths = bots.ConsumeDeathEvents();
    ASSERT_EQ(combat.size(), static_cast<size_t>(1));
    ASSERT_EQ(deaths.size(), static_cast<size_t>(1));
    EXPECT_EQ(combat.front().victimId, victim);
    EXPECT_EQ(deaths.front().victimId, victim);
    EXPECT_EQ(deaths.front().killerId, ::ParticipantId::Human(1));
    EXPECT_TRUE(bots.ConsumeCombatEvents().empty());
    EXPECT_TRUE(bots.ConsumeDeathEvents().empty());
}

TEST(CombatAuthority, TypedBotDamagePreservesRawIdCollisionAndMutatesHumanOnce) {
    Authority authority;
    ASSERT_TRUE(authority.AddParticipant(
        Soldier(1, 2, Vector3(500.0f, 0.0f, 0.0f))));

    ExternalDamageRequest request;
    request.sourceId = ::ParticipantId::Bot(1);
    request.sourceTeam = 1;
    request.targetId = 1;
    request.weaponId = "BotRifle";
    request.originUu = Vector3(0.0f, 0.0f, 0.0f);
    request.impactUu = Vector3(500.0f, 0.0f, 0.0f);
    request.maxRangeUu = 2500.0f;
    request.damage = 30.0f;
    request.hitZone = HitZone::UpperTorso;

    const ExternalDamageResult first = authority.ApplyExternalDamage(request);
    ASSERT_TRUE(first.applied);
    EXPECT_FALSE(first.killed);
    EXPECT_EQ(first.sourceId, ::ParticipantId::Bot(1));
    EXPECT_NE(first.sourceId, ::ParticipantId::Human(1));
    EXPECT_NEAR(first.healthAfter, 70.0f, 0.0001f);

    request.damage = 80.0f;
    const ExternalDamageResult lethal = authority.ApplyExternalDamage(request);
    ASSERT_TRUE(lethal.applied);
    EXPECT_TRUE(lethal.killed);
    EXPECT_NEAR(lethal.healthAfter, 0.0f, 0.0001f);
    const auto target = authority.GetParticipant(1);
    ASSERT_TRUE(target.has_value());
    EXPECT_FALSE(target->alive);
    EXPECT_EQ(target->deaths, static_cast<uint32_t>(1));

    const ExternalDamageResult duplicate =
        authority.ApplyExternalDamage(request);
    EXPECT_FALSE(duplicate.applied);
    EXPECT_EQ(duplicate.reason, RejectReason::TargetDead);
    const auto afterDuplicate = authority.GetParticipant(1);
    ASSERT_TRUE(afterDuplicate.has_value());
    EXPECT_EQ(afterDuplicate->deaths, static_cast<uint32_t>(1));
}

TEST(CombatAuthority, TypedBotDamageRejectsFriendlyUnknownStaleAndInvalidInputs) {
    Authority authority;
    ASSERT_TRUE(authority.AddParticipant(
        Soldier(9, 1, Vector3(500.0f, 0.0f, 0.0f))));

    ExternalDamageRequest request;
    request.sourceId = ::ParticipantId::Bot(7);
    request.sourceTeam = 1;
    request.targetId = 9;
    request.weaponId = "BotRifle";
    request.originUu = Vector3(0.0f, 0.0f, 0.0f);
    request.impactUu = Vector3(500.0f, 0.0f, 0.0f);
    request.maxRangeUu = 2500.0f;
    request.damage = 5.0f;

    EXPECT_EQ(authority.ApplyExternalDamage(request).reason,
              RejectReason::FriendlyFireBlocked);
    request.sourceTeam = 2;
    request.targetId = 999;
    EXPECT_EQ(authority.ApplyExternalDamage(request).reason,
              RejectReason::UnknownTarget);
    request.targetId = 9;
    request.impactUu = Vector3(800.0f, 0.0f, 0.0f);
    EXPECT_EQ(authority.ApplyExternalDamage(request).reason,
              RejectReason::TargetMismatch);
    request.impactUu = Vector3(500.0f, 0.0f, 0.0f);
    request.maxRangeUu = 100.0f;
    EXPECT_EQ(authority.ApplyExternalDamage(request).reason,
              RejectReason::OutOfRange);
    request.maxRangeUu = 2500.0f;
    request.damage = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(authority.ApplyExternalDamage(request).reason,
              RejectReason::InvalidArgument);
    request.damage = 5.0f;
    request.sourceId = ::ParticipantId::Human(7);
    EXPECT_EQ(authority.ApplyExternalDamage(request).reason,
              RejectReason::InvalidArgument);

    const auto target = authority.GetParticipant(9);
    ASSERT_TRUE(target.has_value());
    EXPECT_NEAR(target->health, 100.0f, 0.0001f);
    EXPECT_EQ(target->deaths, static_cast<uint32_t>(0));
}

TEST(CombatAuthority, TypedBotDamageRejectsPointOutsideRoundedCapsuleCap) {
    Authority authority;
    ASSERT_TRUE(authority.AddParticipant(
        Soldier(9, 2, Vector3(500.0f, 0.0f, 0.0f))));

    ExternalDamageRequest request;
    request.sourceId = ::ParticipantId::Bot(7);
    request.sourceTeam = 1;
    request.targetId = 9;
    request.weaponId = "BotRifle";
    request.originUu = Vector3(0.0f, 0.0f, 0.0f);
    // Radius=20 UU and spine top=25 UU. This point is inside the old
    // radius-by-half-height cylinder, but sqrt(19^2 + 19^2) lies outside the
    // capsule's rounded cap.
    request.impactUu = Vector3(519.0f, 0.0f, 44.0f);
    request.maxRangeUu = 2500.0f;
    request.damage = 25.0f;

    const ExternalDamageResult result = authority.ApplyExternalDamage(request);
    EXPECT_FALSE(result.applied);
    EXPECT_EQ(result.reason, RejectReason::TargetMismatch);
    const auto target = authority.GetParticipant(9);
    ASSERT_TRUE(target.has_value());
    EXPECT_FLOAT_EQ(target->health, 100.0f);
    EXPECT_TRUE(target->alive);
}

TEST(CombatAuthority, TypedBotFriendlyFireScalesExactlyOnceWhenEnabled) {
    AuthorityConfig config;
    config.friendlyFireEnabled = true;
    config.friendlyFireDamageScale = 0.5f;
    Authority authority(config);
    ASSERT_TRUE(authority.AddParticipant(
        Soldier(9, 1, Vector3(500.0f, 0.0f, 0.0f))));

    ExternalDamageRequest request;
    request.sourceId = ::ParticipantId::Bot(7);
    request.sourceTeam = 1;
    request.targetId = 9;
    request.weaponId = "BotRifle";
    request.originUu = Vector3(0.0f, 0.0f, 0.0f);
    request.impactUu = Vector3(500.0f, 0.0f, 0.0f);
    request.maxRangeUu = 2500.0f;
    request.damage = 20.0f;

    const ExternalDamageResult result = authority.ApplyExternalDamage(request);
    ASSERT_TRUE(result.applied);
    EXPECT_TRUE(result.friendlyFire);
    EXPECT_NEAR(result.damage, 10.0f, 0.0001f);
    EXPECT_NEAR(result.healthAfter, 90.0f, 0.0001f);
}

TEST(CombatAuthority, HeadMultiplierKillsAndDeathEventIsIdempotent) {
    Authority authority = TwoPlayerWorld();
    ASSERT_TRUE(authority.EquipWeapon(1, Rifle(60.0f)));
    HitscanRequest head = TorsoShot();
    head.startTraceUu.z = 35.0f;
    head.endTraceUu.z = 35.0f;
    head.reportedImpactUu = {483.0f, 0.0f, 35.0f};
    const ActionResult lethal = authority.FireHitscan(head);
    ASSERT_TRUE(lethal.confirmedHit());
    EXPECT_EQ(CountEvents(lethal.events, EventKind::ParticipantDied), 1u);
    EXPECT_EQ(CountEvents(lethal.events, EventKind::DamageApplied), 1u);
    EXPECT_EQ(authority.GetParticipant(2)->deaths, 1u);
    EXPECT_EQ(authority.GetParticipant(1)->kills, 1u);

    ASSERT_TRUE(authority.Advance(0.1f).advanced);
    const ActionResult alreadyDead = authority.FireHitscan(head);
    EXPECT_FALSE(alreadyDead.accepted());
    EXPECT_EQ(alreadyDead.reason, RejectReason::TargetDead);
    EXPECT_EQ(CountEvents(alreadyDead.events, EventKind::ParticipantDied), 0u);
    EXPECT_EQ(authority.GetParticipant(2)->deaths, 1u);
}

TEST(CombatAuthority, BallisticsUseMetersPerSecondAgainstUePositions) {
    Authority authority = TwoPlayerWorld();
    ProjectileRequest request;
    request.shooterId = 1;
    request.originUu = {0.0f, 0.0f, 100.0f};
    request.direction = {1.0f, 0.0f, 0.0f};
    request.projectile.launchSpeedMetersPerSecond = 10.0f;
    request.projectile.gravityMetersPerSecondSquared = {0.0f, 0.0f, -10.0f};
    request.projectile.groundHeightUu = -1000.0f;
    request.projectile.detonateOnGround = false;
    const SpawnResult spawn = authority.SpawnProjectile(request);
    ASSERT_TRUE(spawn.action.accepted());
    ASSERT_TRUE(authority.Advance(0.1f).advanced);
    const auto projectile = authority.GetProjectile(spawn.projectileId);
    ASSERT_TRUE(projectile.has_value());
    EXPECT_NEAR(projectile->positionUu.x, 50.0f, 1.0e-3f);
    EXPECT_NEAR(projectile->positionUu.z, 97.5f, 1.0e-3f);
    EXPECT_NEAR(projectile->velocityMetersPerSecond.z, -1.0f, 1.0e-4f);
}

TEST(CombatAuthority, M61GroundBounceMatchesRecoveredDampingOrder) {
    AuthorityConfig config;
    config.projectileMaxStepSeconds = 0.2f;
    Authority authority = TwoPlayerWorld(1, 2, config);

    ProjectileRequest request;
    request.shooterId = 1;
    request.originUu = {0.0f, 0.0f, 42.0f};
    request.direction = {0.6f, 0.0f, -0.8f};
    request.projectile.launchSpeedMetersPerSecond = 10.0f;
    request.projectile.gravityMetersPerSecondSquared = {};
    request.projectile.groundHeightUu = 0.0f;
    request.projectile.collisionRadiusMeters = 0.04f; // retail 2 UU
    request.projectile.detonateOnGround = false;
    request.projectile.detonateOnParticipant = false;
    request.projectile.maxGroundImpacts = 5;
    request.projectile.groundBounceDamping = 0.33f;
    request.projectile.groundBounceVerticalDamping = 0.85f;

    const SpawnResult spawn = authority.SpawnProjectile(request);
    ASSERT_TRUE(spawn.action.accepted());
    ASSERT_TRUE(authority.Advance(0.1f).advanced);
    const auto projectile = authority.GetProjectile(spawn.projectileId);
    ASSERT_TRUE(projectile.has_value());
    EXPECT_EQ(projectile->groundImpacts, 1u);
    EXPECT_FALSE(projectile->settledOnGround);
    EXPECT_NEAR(projectile->positionUu.x, 30.0f, 1.0e-3f);
    EXPECT_NEAR(projectile->positionUu.z, 2.0f, 1.0e-4f);
    // ROStickGrenadeProjectile reflects, multiplies the whole velocity by
    // 0.33, then applies the additional 0.85 only to Z.
    EXPECT_NEAR(projectile->velocityMetersPerSecond.x, 1.98f, 1.0e-4f);
    EXPECT_NEAR(projectile->velocityMetersPerSecond.z, 2.244f, 1.0e-4f);
}

TEST(CombatAuthority, M61FifthGroundImpactSettlesAfterFourRebounds) {
    AuthorityConfig config;
    config.projectileMaxStepSeconds = 1.0f;
    Authority authority = TwoPlayerWorld(1, 2, config);

    ProjectileRequest request;
    request.shooterId = 1;
    request.originUu = {0.0f, 0.0f, 2.0f};
    request.direction = {0.0f, 0.0f, -1.0f};
    request.projectile.launchSpeedMetersPerSecond = 10.0f;
    request.projectile.gravityMetersPerSecondSquared = {0.0f, 0.0f, -10.0f};
    request.projectile.groundHeightUu = 0.0f;
    request.projectile.collisionRadiusMeters = 0.04f;
    request.projectile.detonateOnGround = false;
    request.projectile.detonateOnParticipant = false;
    request.projectile.maxGroundImpacts = 5;
    request.projectile.groundBounceDamping = 0.33f;
    request.projectile.groundBounceVerticalDamping = 0.85f;

    const SpawnResult spawn = authority.SpawnProjectile(request);
    ASSERT_TRUE(spawn.action.accepted());
    const AdvanceResult advanced = authority.Advance(1.0f);
    ASSERT_TRUE(advanced.advanced);
    EXPECT_EQ(CountEvents(advanced.events, EventKind::ProjectileDetonated), 0u);
    const auto projectile = authority.GetProjectile(spawn.projectileId);
    ASSERT_TRUE(projectile.has_value());
    EXPECT_EQ(projectile->groundImpacts, 5u);
    EXPECT_TRUE(projectile->settledOnGround);
    EXPECT_NEAR(projectile->positionUu.z, 2.0f, 1.0e-4f);
    EXPECT_FLOAT_EQ(projectile->velocityMetersPerSecond.x, 0.0f);
    EXPECT_FLOAT_EQ(projectile->velocityMetersPerSecond.y, 0.0f);
    EXPECT_FLOAT_EQ(projectile->velocityMetersPerSecond.z, 0.0f);
    EXPECT_NEAR(projectile->ageSeconds, 1.0f, 1.0e-5f);
}

TEST(CombatAuthority, ProjectileZeroDeltaDoesNotCreateGroundContacts) {
    Authority authority = TwoPlayerWorld();
    ProjectileRequest request;
    request.shooterId = 1;
    request.originUu = {0.0f, 0.0f, 2.0f};
    request.direction = {0.0f, 0.0f, -1.0f};
    request.projectile.launchSpeedMetersPerSecond = 10.0f;
    request.projectile.groundHeightUu = 0.0f;
    request.projectile.collisionRadiusMeters = 0.04f;
    request.projectile.detonateOnGround = false;
    request.projectile.detonateOnParticipant = false;
    request.projectile.maxGroundImpacts = 5;
    request.projectile.groundBounceDamping = 0.33f;
    request.projectile.groundBounceVerticalDamping = 0.85f;

    const SpawnResult spawn = authority.SpawnProjectile(request);
    ASSERT_TRUE(spawn.action.accepted());
    const auto before = authority.GetProjectile(spawn.projectileId);
    ASSERT_TRUE(before.has_value());
    const double timeBefore = authority.GetTimeSeconds();
    const AdvanceResult zero = authority.Advance(0.0f);
    ASSERT_TRUE(zero.advanced);
    EXPECT_TRUE(zero.events.empty());
    EXPECT_DOUBLE_EQ(authority.GetTimeSeconds(), timeBefore);
    const auto after = authority.GetProjectile(spawn.projectileId);
    ASSERT_TRUE(after.has_value());
    EXPECT_FLOAT_EQ(after->ageSeconds, before->ageSeconds);
    EXPECT_FLOAT_EQ(after->positionUu.z, before->positionUu.z);
    EXPECT_FLOAT_EQ(after->velocityMetersPerSecond.z,
                    before->velocityMetersPerSecond.z);
    EXPECT_EQ(after->groundImpacts, 0u);
    EXPECT_FALSE(after->settledOnGround);
}

TEST(CombatAuthority, ProjectileBounceSpecValidationFailsBeforeAmmoMutation) {
    Authority authority = TwoPlayerWorld();
    ProjectileRequest request;
    request.shooterId = 1;
    request.originUu = {0.0f, 0.0f, 10.0f};
    request.direction = {0.0f, 0.0f, -1.0f};
    request.projectile.groundHeightUu = 0.0f;
    request.projectile.detonateOnGround = false;
    request.projectile.detonateOnParticipant = false;

    request.projectile.groundBounceDamping =
        std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(authority.SpawnProjectile(request).action.reason,
              RejectReason::InvalidArgument);
    request.projectile.groundBounceDamping = 1.01f;
    EXPECT_EQ(authority.SpawnProjectile(request).action.reason,
              RejectReason::InvalidArgument);
    request.projectile.groundBounceDamping = 0.33f;
    request.projectile.groundBounceVerticalDamping = -0.01f;
    EXPECT_EQ(authority.SpawnProjectile(request).action.reason,
              RejectReason::InvalidArgument);
    request.projectile.groundBounceVerticalDamping = 0.85f;
    request.projectile.maxGroundImpacts =
        kMaxProjectileGroundImpacts + 1u;
    EXPECT_EQ(authority.SpawnProjectile(request).action.reason,
              RejectReason::InvalidArgument);

    const auto shooter = authority.GetParticipant(1);
    ASSERT_TRUE(shooter && shooter->weapon);
    EXPECT_EQ(shooter->weapon->magazineAmmo, 10u);
    EXPECT_EQ(authority.GetProjectileCount(), 0u);
}

TEST(CombatAuthority, SettledProjectileContinuesFuseClock) {
    Authority authority = TwoPlayerWorld();
    ProjectileRequest request;
    request.shooterId = 1;
    request.originUu = {0.0f, 0.0f, 2.0f};
    request.direction = {0.0f, 0.0f, -1.0f};
    request.projectile.launchSpeedMetersPerSecond = 1.0f;
    request.projectile.gravityMetersPerSecondSquared = {};
    request.projectile.fuseSeconds = 0.2f;
    request.projectile.groundHeightUu = 0.0f;
    request.projectile.collisionRadiusMeters = 0.04f;
    request.projectile.detonateOnGround = false;
    request.projectile.detonateOnParticipant = false;
    request.projectile.maxGroundImpacts = 1;
    request.projectile.groundBounceDamping = 0.33f;
    request.projectile.groundBounceVerticalDamping = 0.85f;

    const SpawnResult spawn = authority.SpawnProjectile(request);
    ASSERT_TRUE(spawn.action.accepted());
    const AdvanceResult settled = authority.Advance(0.1f);
    ASSERT_TRUE(settled.advanced);
    EXPECT_EQ(CountEvents(settled.events, EventKind::ProjectileDetonated), 0u);
    const auto snapshot = authority.GetProjectile(spawn.projectileId);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_TRUE(snapshot->settledOnGround);
    EXPECT_EQ(snapshot->groundImpacts, 1u);
    EXPECT_NEAR(snapshot->ageSeconds, 0.1f, 1.0e-5f);

    const AdvanceResult fused = authority.Advance(0.1f);
    EXPECT_EQ(CountEvents(fused.events, EventKind::ProjectileDetonated), 1u);
    EXPECT_FALSE(authority.GetProjectile(spawn.projectileId).has_value());
}

TEST(CombatAuthority, BouncingProjectileMapRemovalNeverDetonates) {
    Authority authority = TwoPlayerWorld();
    ProjectileRequest request;
    request.shooterId = 1;
    request.originUu = {0.0f, 0.0f, 2.0f};
    request.direction = {0.0f, 0.0f, -1.0f};
    request.projectile.launchSpeedMetersPerSecond = 5.0f;
    request.projectile.fuseSeconds = 4.2f;
    request.projectile.groundHeightUu = 0.0f;
    request.projectile.collisionRadiusMeters = 0.04f;
    request.projectile.detonateOnGround = false;
    request.projectile.detonateOnParticipant = false;
    request.projectile.maxGroundImpacts = 5;
    request.projectile.groundBounceDamping = 0.33f;
    request.projectile.groundBounceVerticalDamping = 0.85f;

    const SpawnResult spawn = authority.SpawnProjectile(request);
    ASSERT_TRUE(spawn.action.accepted());
    ASSERT_TRUE(authority.Advance(0.01f).advanced);
    ASSERT_TRUE(authority.GetProjectile(spawn.projectileId).has_value());
    EXPECT_EQ(authority.GetProjectile(spawn.projectileId)->groundImpacts, 1u);

    const std::vector<CombatEvent> removed =
        authority.RemoveAllProjectiles();
    ASSERT_EQ(removed.size(), static_cast<size_t>(1));
    EXPECT_EQ(removed.front().kind, EventKind::ProjectileRemoved);
    EXPECT_EQ(removed.front().projectileId, spawn.projectileId);
    EXPECT_EQ(CountEvents(removed, EventKind::ProjectileDetonated), 0u);
    EXPECT_EQ(authority.GetProjectileCount(), 0u);
}

TEST(CombatAuthority, HighSpeedGroundSweepCannotTunnelThroughPlane) {
    AuthorityConfig config;
    config.projectileMaxStepSeconds = 0.2f;
    Authority authority = TwoPlayerWorld(1, 2, config);
    ASSERT_TRUE(authority.SetParticipantPosition(
        1, Vector3(0.0f, 0.0f, 5002.0f)));
    ProjectileRequest request;
    request.shooterId = 1;
    request.originUu = {0.0f, 0.0f, 5002.0f};
    request.direction = {0.0f, 0.0f, -1.0f};
    request.projectile.launchSpeedMetersPerSecond = 1000.0f;
    request.projectile.gravityMetersPerSecondSquared = {};
    request.projectile.groundHeightUu = 0.0f;
    request.projectile.collisionRadiusMeters = 0.04f;
    request.projectile.detonateOnGround = false;
    request.projectile.detonateOnParticipant = false;
    request.projectile.maxGroundImpacts = 5;
    request.projectile.groundBounceDamping = 0.33f;
    request.projectile.groundBounceVerticalDamping = 0.85f;

    const SpawnResult spawn = authority.SpawnProjectile(request);
    ASSERT_TRUE(spawn.action.accepted());
    ASSERT_TRUE(authority.Advance(0.1f).advanced);
    const auto projectile = authority.GetProjectile(spawn.projectileId);
    ASSERT_TRUE(projectile.has_value());
    EXPECT_EQ(projectile->groundImpacts, 1u);
    EXPECT_FALSE(projectile->settledOnGround);
    EXPECT_NEAR(projectile->positionUu.z, 2.0f, 1.0e-3f);
    EXPECT_NEAR(projectile->velocityMetersPerSecond.z, 280.5f, 1.0e-2f);
}

TEST(CombatAuthority, FuseDetonationProducesExplosionDamageEvents) {
    Authority authority;
    ASSERT_TRUE(authority.AddParticipant(
        Soldier(1, 1, {0.0f, 0.0f, 0.0f}, 1000.0f)));
    ASSERT_TRUE(authority.AddParticipant(
        Soldier(2, 2, {100.0f, 0.0f, 0.0f})));
    ASSERT_TRUE(authority.EquipWeapon(1, Rifle()));

    ProjectileRequest request;
    request.shooterId = 1;
    request.originUu = {0.0f, 0.0f, 0.0f};
    request.direction = {1.0f, 0.0f, 0.0f};
    request.projectile.launchSpeedMetersPerSecond = 1.0f;
    request.projectile.gravityMetersPerSecondSquared = {};
    request.projectile.fuseSeconds = 0.1f;
    request.projectile.explosionRadiusMeters = 3.0f;
    request.projectile.maxExplosionDamage = 250.0f;
    request.projectile.groundHeightUu = -500.0f;
    request.projectile.detonateOnGround = false;
    const SpawnResult spawn = authority.SpawnProjectile(request);
    ASSERT_TRUE(spawn.action.accepted());
    const AdvanceResult advanced = authority.Advance(0.1f);
    EXPECT_EQ(CountEvents(advanced.events, EventKind::ProjectileDetonated), 1u);
    EXPECT_GE(CountEvents(advanced.events, EventKind::DamageApplied), 1u);
    EXPECT_FALSE(authority.GetParticipant(2)->alive);
    EXPECT_FALSE(authority.GetProjectile(spawn.projectileId).has_value());
}

TEST(CombatAuthority, MapTransitionRemovesProjectilesWithoutDetonation) {
    Authority authority = TwoPlayerWorld();
    ProjectileRequest request;
    request.shooterId = 1;
    request.originUu = {0.0f, 0.0f, 100.0f};
    request.direction = {1.0f, 0.0f, 0.0f};
    request.projectile.launchSpeedMetersPerSecond = 10.0f;
    request.projectile.fuseSeconds = 5.0f;
    request.projectile.explosionRadiusMeters = 3.0f;
    request.projectile.maxExplosionDamage = 100.0f;
    request.projectile.groundHeightUu = -1000.0f;
    request.projectile.detonateOnGround = false;

    const SpawnResult spawn = authority.SpawnProjectile(request);
    ASSERT_TRUE(spawn.action.accepted());
    const std::vector<CombatEvent> removed =
        authority.RemoveAllProjectiles();
    ASSERT_EQ(removed.size(), static_cast<size_t>(1));
    EXPECT_EQ(removed[0].kind, EventKind::ProjectileRemoved);
    EXPECT_EQ(removed[0].projectileId, spawn.projectileId);
    EXPECT_EQ(CountEvents(removed, EventKind::ProjectileDetonated), 0u);
    EXPECT_EQ(authority.GetProjectileCount(), static_cast<size_t>(0));
}

TEST(CombatAuthority,
     DelayedProjectileRetainsTeamAndExplosionMetadataAfterShooterRemoval) {
    Authority authority;
    ASSERT_TRUE(authority.AddParticipant(
        Soldier(1, 1, {0.0f, 0.0f, 0.0f})));
    ASSERT_TRUE(authority.AddParticipant(
        Soldier(2, 1, {100.0f, 0.0f, 0.0f})));
    ASSERT_TRUE(authority.EquipWeapon(1, Grenade()));

    ProjectileRequest request;
    request.shooterId = 1;
    request.weaponId = "M61";
    request.originUu = {};
    request.direction = {1.0f, 0.0f, 0.0f};
    request.projectile.launchSpeedMetersPerSecond = 1.0f;
    request.projectile.gravityMetersPerSecondSquared = {};
    request.projectile.fuseSeconds = 0.1f;
    request.projectile.explosionRadiusMeters = 5.0f;
    request.projectile.maxExplosionDamage = 100.0f;
    request.projectile.groundHeightUu = -500.0f;
    request.projectile.detonateOnGround = false;

    const SpawnResult spawn = authority.SpawnProjectile(request);
    ASSERT_TRUE(spawn.action.accepted());
    const auto projectile = authority.GetProjectile(spawn.projectileId);
    ASSERT_TRUE(projectile.has_value());
    EXPECT_EQ(projectile->shooterTeam, 1u);

    // Removing the shooter must not erase the relationship carried by a live
    // projectile. Otherwise a delayed grenade could bypass friendly-fire.
    ASSERT_TRUE(authority.RemoveParticipant(1));
    const AdvanceResult advanced = authority.Advance(0.1f);
    ASSERT_TRUE(advanced.advanced);
    EXPECT_EQ(CountEvents(advanced.events, EventKind::FriendlyFireBlocked), 1u);
    EXPECT_EQ(CountEvents(advanced.events, EventKind::DamageApplied), 0u);
    EXPECT_FLOAT_EQ(authority.GetParticipant(2)->health, 100.0f);

    const auto detonation = std::find_if(
        advanced.events.begin(), advanced.events.end(),
        [](const CombatEvent& event) {
            return event.kind == EventKind::ProjectileDetonated;
        });
    ASSERT_TRUE(detonation != advanced.events.end());
    EXPECT_EQ(detonation->sourceTeam, 1u);
    EXPECT_FLOAT_EQ(detonation->explosionRadiusMeters, 5.0f);
    EXPECT_FLOAT_EQ(detonation->maxExplosionDamage, 100.0f);
}

TEST(CombatAuthority, GroundCollisionUsesConfiguredNegativeHeight) {
    Authority authority;
    ASSERT_TRUE(authority.AddParticipant(
        Soldier(1, 1, {0.0f, 0.0f, -400.0f})));
    ASSERT_TRUE(authority.EquipWeapon(1, Rifle()));
    ProjectileRequest request;
    request.shooterId = 1;
    request.originUu = {0.0f, 0.0f, -400.0f};
    request.direction = {0.0f, 0.0f, -1.0f};
    request.projectile.launchSpeedMetersPerSecond = 10.0f;
    request.projectile.gravityMetersPerSecondSquared = {};
    request.projectile.groundHeightUu = -500.0f;
    request.projectile.collisionRadiusMeters = 0.05f;
    request.projectile.detonateOnGround = true;
    const SpawnResult spawn = authority.SpawnProjectile(request);
    ASSERT_TRUE(spawn.action.accepted());
    const AdvanceResult advanced = authority.Advance(0.2f);
    EXPECT_EQ(CountEvents(advanced.events, EventKind::ProjectileDetonated), 1u);
    EXPECT_FALSE(authority.GetProjectile(spawn.projectileId).has_value());
    const auto detonation = std::find_if(
        advanced.events.begin(), advanced.events.end(),
        [](const CombatEvent& event) {
            return event.kind == EventKind::ProjectileDetonated;
        });
    ASSERT_TRUE(detonation != advanced.events.end());
    EXPECT_NEAR(detonation->positionUu.z, -497.5f, 1.0e-3f);
}

TEST(CombatAuthority, ProjectileRequiresExplicitGroundAndAdvanceIsBounded) {
    Authority authority = TwoPlayerWorld();
    ProjectileRequest request;
    request.shooterId = 1;
    request.originUu = {};
    request.direction = {1.0f, 0.0f, 0.0f};
    // groundHeightUu intentionally remains NaN.
    const SpawnResult rejected = authority.SpawnProjectile(request);
    EXPECT_FALSE(rejected.action.accepted());
    EXPECT_EQ(rejected.action.reason, RejectReason::InvalidArgument);
    EXPECT_EQ(authority.GetParticipant(1)->weapon->magazineAmmo, 10u);

    const double before = authority.GetTimeSeconds();
    const AdvanceResult tooLarge = authority.Advance(2.0f);
    EXPECT_FALSE(tooLarge.advanced);
    EXPECT_EQ(tooLarge.reason, RejectReason::InvalidArgument);
    EXPECT_DOUBLE_EQ(authority.GetTimeSeconds(), before);
}

TEST(CombatAuthority, ExplicitProjectileWeaponConsumesOnlyGrenadeAmmo) {
    Authority authority = TwoPlayerWorld();
    ASSERT_TRUE(authority.AddWeapon(1, Grenade(), false));

    ProjectileRequest request;
    request.shooterId = 1;
    request.originUu = {};
    request.direction = {1.0f, 0.0f, 0.0f};
    request.projectile.launchSpeedMetersPerSecond = 20.0f;
    request.projectile.gravityMetersPerSecondSquared = {};
    request.projectile.groundHeightUu = -500.0f;
    request.projectile.detonateOnGround = false;
    request.weaponId = "M61";

    const SpawnResult spawn = authority.SpawnProjectile(request);
    ASSERT_TRUE(spawn.action.accepted());
    ASSERT_NE(spawn.projectileId, 0u);
    ASSERT_TRUE(authority.GetProjectile(spawn.projectileId).has_value());
    EXPECT_EQ(authority.GetProjectile(spawn.projectileId)->weaponId, "M61");

    const auto participant = authority.GetParticipant(1);
    ASSERT_TRUE(participant && participant->weapon);
    EXPECT_EQ(participant->activeWeaponId, "M16A1");
    EXPECT_EQ(participant->weapon->id, "M16A1");
    ASSERT_TRUE(participant->weapons.contains("M16A1"));
    ASSERT_TRUE(participant->weapons.contains("M61"));
    EXPECT_EQ(participant->weapons.at("M16A1").magazineAmmo, 10u);
    EXPECT_EQ(participant->weapons.at("M61").magazineAmmo, 0u);
}

TEST(CombatAuthority, ActiveWeaponSelectionAndSnapshotsAreCopyIsolated) {
    Authority authority = TwoPlayerWorld();
    ASSERT_TRUE(authority.AddWeapon(1, Grenade(), false));
    ASSERT_TRUE(authority.SetActiveWeapon(1, "M61"));

    auto snapshot = authority.GetParticipant(1);
    ASSERT_TRUE(snapshot && snapshot->weapon);
    EXPECT_EQ(snapshot->activeWeaponId, "M61");
    EXPECT_EQ(snapshot->weapon->id, "M61");
    EXPECT_EQ(snapshot->weapons.at("M16A1").magazineAmmo, 10u);
    EXPECT_EQ(snapshot->weapons.at("M61").magazineAmmo, 1u);

    // ParticipantSnapshot is an isolated value view, not a mutable handle into
    // authoritative inventory state.
    snapshot->weapon->magazineAmmo = 999u;
    snapshot->weapons.at("M16A1").magazineAmmo = 998u;
    snapshot->weapons.at("M61").magazineAmmo = 997u;

    const auto unchanged = authority.GetParticipant(1);
    ASSERT_TRUE(unchanged && unchanged->weapon);
    EXPECT_EQ(unchanged->activeWeaponId, "M61");
    EXPECT_EQ(unchanged->weapon->magazineAmmo, 1u);
    EXPECT_EQ(unchanged->weapons.at("M16A1").magazineAmmo, 10u);
    EXPECT_EQ(unchanged->weapons.at("M61").magazineAmmo, 1u);

    ProjectileRequest request;
    request.shooterId = 1;
    request.originUu = {};
    request.direction = {1.0f, 0.0f, 0.0f};
    request.projectile.launchSpeedMetersPerSecond = 20.0f;
    request.projectile.gravityMetersPerSecondSquared = {};
    request.projectile.groundHeightUu = -500.0f;
    request.projectile.detonateOnGround = false;
    // Empty weaponId follows the newly selected active weapon.
    ASSERT_TRUE(authority.SpawnProjectile(request).action.accepted());
    EXPECT_EQ(authority.GetParticipant(1)->weapons.at("M61").magazineAmmo, 0u);
    EXPECT_EQ(authority.GetParticipant(1)->weapons.at("M16A1").magazineAmmo,
              10u);
}

TEST(CombatAuthority, WeaponCadenceAndReloadStateAreIndependent) {
    Authority authority = TwoPlayerWorld();
    WeaponProfile secondary = Rifle(5.0f, 2, 4);
    secondary.id = "M14";
    secondary.reloadSeconds = 0.2f;
    ASSERT_TRUE(authority.AddWeapon(1, secondary, false));

    ASSERT_TRUE(authority.FireHitscan(TorsoShot()).confirmedHit());
    const ActionResult rifleCadence = authority.FireHitscan(TorsoShot());
    EXPECT_FALSE(rifleCadence.accepted());
    EXPECT_EQ(rifleCadence.reason, RejectReason::Cadence);

    HitscanRequest secondaryShot = TorsoShot();
    secondaryShot.weaponId = "M14";
    ASSERT_TRUE(authority.FireHitscan(secondaryShot).confirmedHit());

    ASSERT_TRUE(authority.RequestReload(1, "M16A1").accepted());
    ASSERT_TRUE(authority.RequestReload(1, "M14").accepted());
    const AdvanceResult secondaryCompleted = authority.Advance(0.2f);
    EXPECT_EQ(CountEvents(secondaryCompleted.events,
                          EventKind::ReloadCompleted), 1u);
    ASSERT_EQ(secondaryCompleted.events.size(), 1u);
    EXPECT_EQ(secondaryCompleted.events.front().weaponId, "M14");

    auto participant = authority.GetParticipant(1);
    ASSERT_TRUE(participant);
    EXPECT_TRUE(participant->weapons.at("M16A1").reloading);
    EXPECT_FALSE(participant->weapons.at("M14").reloading);
    EXPECT_EQ(participant->weapons.at("M16A1").magazineAmmo, 9u);
    EXPECT_EQ(participant->weapons.at("M14").magazineAmmo, 2u);

    const AdvanceResult rifleCompleted = authority.Advance(0.3f);
    EXPECT_EQ(CountEvents(rifleCompleted.events,
                          EventKind::ReloadCompleted), 1u);
    ASSERT_EQ(rifleCompleted.events.size(), 1u);
    EXPECT_EQ(rifleCompleted.events.front().weaponId, "M16A1");
    participant = authority.GetParticipant(1);
    ASSERT_TRUE(participant);
    EXPECT_FALSE(participant->weapons.at("M16A1").reloading);
    EXPECT_EQ(participant->weapons.at("M16A1").magazineAmmo, 10u);
}

TEST(CombatAuthority, InvalidAndUnknownWeaponsFailClosedWithoutMutation) {
    Authority authority = TwoPlayerWorld();
    ASSERT_TRUE(authority.AddWeapon(1, Grenade(), false));

    WeaponProfile invalid = Grenade();
    invalid.id = "Invalid";
    invalid.roundsPerMinute = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(authority.AddWeapon(1, invalid, true));
    EXPECT_FALSE(authority.SetActiveWeapon(1, "Unknown"));

    HitscanRequest hitscan = TorsoShot();
    hitscan.weaponId = "Unknown";
    const ActionResult rejectedHitscan = authority.FireHitscan(hitscan);
    EXPECT_FALSE(rejectedHitscan.accepted());
    EXPECT_EQ(rejectedHitscan.reason, RejectReason::NoWeapon);

    ProjectileRequest projectile;
    projectile.shooterId = 1;
    projectile.originUu = {};
    projectile.direction = {1.0f, 0.0f, 0.0f};
    projectile.projectile.groundHeightUu = -500.0f;
    projectile.weaponId = "Unknown";
    const SpawnResult rejectedProjectile = authority.SpawnProjectile(projectile);
    EXPECT_FALSE(rejectedProjectile.action.accepted());
    EXPECT_EQ(rejectedProjectile.action.reason, RejectReason::NoWeapon);
    EXPECT_EQ(authority.GetProjectileCount(), 0u);

    const ActionResult rejectedReload = authority.RequestReload(1, "Unknown");
    EXPECT_FALSE(rejectedReload.accepted());
    EXPECT_EQ(rejectedReload.reason, RejectReason::NoWeapon);

    const auto participant = authority.GetParticipant(1);
    ASSERT_TRUE(participant && participant->weapon);
    EXPECT_EQ(participant->activeWeaponId, "M16A1");
    EXPECT_EQ(participant->weapons.size(), 2u);
    EXPECT_EQ(participant->weapons.at("M16A1").magazineAmmo, 10u);
    EXPECT_EQ(participant->weapons.at("M61").magazineAmmo, 1u);
    EXPECT_FLOAT_EQ(authority.GetParticipant(2)->health, 100.0f);
}

TEST(CombatAuthority, LegacyEquipAndEmptySelectionRetainSingleWeaponSemantics) {
    Authority authority;
    ASSERT_TRUE(authority.AddParticipant(Soldier(1, 1, {})));
    ASSERT_TRUE(authority.AddParticipant(
        Soldier(2, 2, {500.0f, 0.0f, 0.0f})));
    ASSERT_TRUE(authority.EquipWeapon(1, Rifle()));

    auto participant = authority.GetParticipant(1);
    ASSERT_TRUE(participant && participant->weapon);
    EXPECT_EQ(participant->activeWeaponId, "M16A1");
    EXPECT_EQ(participant->weapons.size(), 1u);
    ASSERT_TRUE(authority.FireHitscan(TorsoShot()).confirmedHit());
    EXPECT_EQ(authority.GetParticipant(1)->weapon->magazineAmmo, 9u);

    WeaponProfile replacement = Rifle(10.0f, 3, 4);
    ASSERT_TRUE(authority.EquipWeapon(1, replacement));
    participant = authority.GetParticipant(1);
    ASSERT_TRUE(participant && participant->weapon);
    EXPECT_EQ(participant->activeWeaponId, "M16A1");
    EXPECT_EQ(participant->weapons.size(), 1u);
    EXPECT_EQ(participant->weapon->magazineAmmo, 3u);
    EXPECT_EQ(participant->weapon->reserveAmmo, 4u);
}

RS2V_TEST_MAIN()
