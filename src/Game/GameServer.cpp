// src/Game/GameServer.cpp

#include "Game/GameServer.h"
#include "Utils/Logger.h"
#include "Network/NetworkManager.h"
#include "Game/AdminManager.h"
#include "Game/ChatManager.h"
#include "Game/CommandManager.h"
#include "Network/ConsoleInput.h"
#include "Network/RemoteAdminServer.h"
#include "Game/GameMode.h"
#include "Config/ConfigManager.h"
#include "Config/GameConfig.h"
#include "../../telemetry/TelemetryManager.h"
#include "Config/NetworkConfig.h"
#include "Config/ServerConfig.h"
#include "Config/SecurityConfig.h"
#include "Config/MapConfig.h"
#include "Game/PlayerManager.h"
#include "Game/TeamManager.h"
#include "Game/MapManager.h"
#include "Game/MapVoteManager.h"
#include "Game/WorkshopManager.h"
#include "Game/ModManager.h"
#include "Game/MutatorManager.h"
#include "Game/RoleSystem.h"
#include "Game/TicketSystem.h"
#include "Game/ObjectiveSystem.h"
#include "Game/CommanderAbilities.h"
#include "Game/SpawnSystem.h"
#include "Game/SpawnPoint.h"
#include "Game/WeaponDatabase.h"
#include "Game/DamageSystem.h"
#include "Game/CombatAuthority.h"
#include "Game/MantleAuthority.h"
#include "Game/ProjectileManager.h"
#include "Game/HelicopterPhysics.h"
#include "Game/TerritoryMode.h"
#include "Game/SupremacyMode.h"
#include "Game/SkirmishMode.h"
#include "Game/GameState.h"
#include "Game/RoundManager.h"
#include "Game/ConnectionLoginBridge.h"
#include "Game/BotManager.h"
#include "Time/FrameTiming.h"
#include "Protocol/ReplicationManager.h"
#include "Network/ClientConnection.h"
#include "Network/ClientTravelReplication.h"
#include "Network/GameplayRpcReplication.h"
#include "Network/MantleReplication.h"
#include "Network/RetailBootstrap.h"
#include "Network/WeaponCombatReplication.h"
#include "Utils/PathUtils.h"
#include "Utils/StringUtils.h"
#include "Utils/HandlerLibraryManager.h"
#include "Utils/CrashHandler.h"
#include "Protocol/ReverseEngineering/ProtocolDecoder.h"
#include <array>
#include <climits>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>

using namespace GeneratedHandlers;

namespace {

bool BuildDefaultCombatWeaponProfile(
    const WeaponDatabase* database, uint32_t team,
    CombatAuthority::WeaponProfile& profile) {
    if (!database) return false;

    // The owning retail graph currently exposes a capture-derived primary on
    // ch210 but does not yet carry a trustworthy role/loadout class identity.
    // Use the canonical faction rifle until that identity is decoded; never
    // infer a different weapon from an unverified channel/class guess.
    const char* weaponId = team == 2 ? "AK47" : "M16A1";
    const WeaponDefinition* definition = database->GetWeapon(weaponId);
    if (!definition || definition->stats.magazineSize <= 0 ||
        definition->stats.maxAmmo < definition->stats.magazineSize) {
        return false;
    }

    profile = {};
    profile.id = definition->id;
    profile.magazineCapacity =
        static_cast<uint32_t>(definition->stats.magazineSize);
    profile.initialMagazine = profile.magazineCapacity;
    profile.initialReserve = static_cast<uint32_t>(
        definition->stats.maxAmmo - definition->stats.magazineSize);
    profile.roundsPerMinute = definition->stats.fireRate;
    profile.reloadSeconds = definition->stats.reloadTime;
    profile.maxRangeMeters = definition->stats.range;
    profile.baseDamage = definition->stats.damage;
    profile.minDamageFractionAtMaxRange = 0.3f;
    return true;
}

CombatAuthority::WeaponProfile BuildGrenadeCombatWeaponProfile(
    const char* weaponId) {
    CombatAuthority::WeaponProfile profile;
    profile.id = weaponId ? weaponId : "";
    profile.magazineCapacity = 2;
    profile.initialMagazine = 2;
    profile.initialReserve = 0;
    profile.roundsPerMinute = 30.0f;
    profile.reloadSeconds = 1.0f;
    profile.maxRangeMeters = 50.0f;
    // Explosion damage is owned by ProjectileSpec, not the weapon's hitscan
    // damage curve.
    profile.baseDamage = 0.0f;
    profile.minDamageFractionAtMaxRange = 1.0f;
    return profile;
}

uint16_t RadiansToUnrealRotator(double radians) {
    constexpr double kTwoPi = 6.28318530717958647692;
    if (!std::isfinite(radians)) return 0;
    double turns = std::fmod(radians / kTwoPi, 1.0);
    if (turns < 0.0) turns += 1.0;
    const uint32_t units = static_cast<uint32_t>(
        std::llround(turns * 65536.0)) & 0xFFFFu;
    return static_cast<uint16_t>(units);
}

bool BuildM61VisualSnapshot(
    const CombatAuthority::Authority& authority,
    const CombatAuthority::ProjectileSnapshot& projectile,
    WeaponCombatRepl::M61VisualSnapshot& output) {
    const float unitsPerMeter = authority.GetConfig().ueUnitsPerMeter;
    if (!std::isfinite(unitsPerMeter) || unitsPerMeter <= 0.0f ||
        !std::isfinite(projectile.positionUu.x) ||
        !std::isfinite(projectile.positionUu.y) ||
        !std::isfinite(projectile.positionUu.z) ||
        !std::isfinite(projectile.velocityMetersPerSecond.x) ||
        !std::isfinite(projectile.velocityMetersPerSecond.y) ||
        !std::isfinite(projectile.velocityMetersPerSecond.z) ||
        !std::isfinite(projectile.ageSeconds) ||
        !std::isfinite(projectile.spec.fuseSeconds)) {
        return false;
    }

    output = {};
    output.positionUu = projectile.positionUu;
    output.velocityUuPerSecond =
        projectile.velocityMetersPerSecond * unitsPerMeter;
    const Vector3& velocity = projectile.velocityMetersPerSecond;
    const double horizontal = std::hypot(
        static_cast<double>(velocity.x), static_cast<double>(velocity.y));
    output.pitch = RadiansToUnrealRotator(std::atan2(
        static_cast<double>(velocity.z), horizontal));
    output.yaw = RadiansToUnrealRotator(std::atan2(
        static_cast<double>(velocity.y), static_cast<double>(velocity.x)));
    // Collision/damage remain server authoritative. This visual-only roll
    // supplies a deterministic tumble until angular velocity is modeled.
    output.roll = RadiansToUnrealRotator(
        static_cast<double>(projectile.ageSeconds) * 9.42477796076938);
    output.fuseSeconds = std::clamp(
        projectile.spec.fuseSeconds - projectile.ageSeconds,
        0.0f, WeaponCombatRepl::kM61MaxReplicatedFuseSeconds);
    return true;
}

} // namespace

void GameServer::OnClientDisconnected(uint32_t clientId)
{
    OnClientMapTravelQueued(clientId);
}

void GameServer::OnClientMapTravelQueued(uint32_t clientId)
{
    RemoveCombatParticipant(clientId);
    if (m_roleSystem) {
        m_roleSystem->RemovePlayer(clientId);
    }
    if (m_loginBridge) {
        m_loginBridge->OnClientDisconnected(clientId);
        return;
    }

    // Defensive fallback for a partially initialized server.  Under the normal
    // runtime path the bridge owns this cleanup (including security and PRI).
    if (m_playerManager && m_playerManager->GetPlayer(clientId)) {
        m_playerManager->OnPlayerDisconnect(clientId);
    } else if (m_teamManager) {
        m_teamManager->RemovePlayer(clientId);
    }
}

void GameServer::RegisterCombatParticipant(uint32_t clientId) {
    SynchronizeCombatParticipant(clientId, false);
}

void GameServer::RemoveCombatParticipant(uint32_t clientId) {
    m_retailGrenadeCooks.erase(clientId);
    if (m_mantleAuthority) {
        m_mantleAuthority->ForgetPlayer(clientId);
    }
    if (m_combatAuthority) {
        m_combatAuthority->RemoveParticipant(clientId);
    }
}

void GameServer::RespawnCombatParticipant(uint32_t clientId) {
    m_retailGrenadeCooks.erase(clientId);
    if (m_mantleAuthority) {
        m_mantleAuthority->ForgetPlayer(clientId);
    }
    SynchronizeCombatParticipant(clientId, true);
    if (!m_playerManager || !m_networkManager) return;
    const std::shared_ptr<Player> player =
        m_playerManager->GetPlayer(clientId);
    if (!player) return;
    m_networkManager->ReplicateRetailCombatState(
        clientId, player->GetHealth(),
        m_playerManager->GetPlayerKills(clientId),
        m_playerManager->GetPlayerDeaths(clientId),
        m_playerManager->GetPlayerScore(clientId),
        !player->IsAlive(), true, true);
}

void GameServer::SynchronizeCombatParticipant(uint32_t clientId,
                                              bool forceRespawn) {
    if (!m_combatAuthority || !m_playerManager) return;
    const std::shared_ptr<Player> player =
        m_playerManager->GetPlayer(clientId);
    if (!player) {
        m_retailGrenadeCooks.erase(clientId);
        if (m_mantleAuthority) {
            m_mantleAuthority->ForgetPlayer(clientId);
        }
        m_combatAuthority->RemoveParticipant(clientId);
        return;
    }

    const uint32_t playerTeam = player->GetTeam();
    const uint32_t authorityTeam = playerTeam == 0
        ? CombatAuthority::kNoTeam
        : playerTeam;
    const Vector3 position = player->GetPosition();
    const bool playerAlive = player->IsAlive();
    const float health = playerAlive
        ? static_cast<float>(std::clamp(player->GetHealth(), 1, 100))
        : 0.0f;

    auto equipCombatLoadout = [&]() {
        if (playerTeam != 1 && playerTeam != 2) return;
        CombatAuthority::WeaponProfile profile;
        if (!BuildDefaultCombatWeaponProfile(
                m_weaponDatabase.get(), playerTeam, profile) ||
            !m_combatAuthority->EquipWeapon(clientId, profile)) {
            Logger::Warn(
                "[CombatAuthority] could not equip faction primary for client %u "
                "(team %u)",
                clientId, playerTeam);
        }
        const char* grenadeId = playerTeam == 2 ? "Type67" : "M61";
        const CombatAuthority::WeaponProfile grenade =
            BuildGrenadeCombatWeaponProfile(grenadeId);
        if (!m_combatAuthority->AddWeapon(
                clientId, grenade, /*makeActive=*/false)) {
            Logger::Warn(
                "[CombatAuthority] could not add %s inventory for client %u",
                grenadeId, clientId);
        }
    };

    auto addCurrent = [&]() {
        CombatAuthority::ParticipantSpec spec;
        spec.id = clientId;
        spec.team = authorityTeam;
        spec.positionUu = position;
        spec.maxHealth = 100.0f;
        spec.health = health;
        if (!m_combatAuthority->AddParticipant(spec)) {
            Logger::Warn(
                "[CombatAuthority] failed to register client %u (team %u)",
                clientId, playerTeam);
            return false;
        }
        equipCombatLoadout();
        return true;
    };

    std::optional<CombatAuthority::ParticipantSnapshot> snapshot =
        m_combatAuthority->GetParticipant(clientId);
    if (!snapshot) {
        addCurrent();
        return;
    }

    // Team is part of the authority's immutable participant spec. Team changes
    // happen before deployment, so rebuild rather than retaining stale friendly-
    // fire identity. PlayerManager remains the durable scoreboard authority.
    if (snapshot->team != authorityTeam ||
        (!playerAlive && snapshot->alive)) {
        m_retailGrenadeCooks.erase(clientId);
        if (m_mantleAuthority) {
            m_mantleAuthority->ForgetPlayer(clientId);
        }
        m_combatAuthority->RemoveParticipant(clientId);
        addCurrent();
        return;
    }

    m_combatAuthority->SetParticipantPosition(clientId, position);
    if (forceRespawn || (playerAlive && !snapshot->alive)) {
        m_retailGrenadeCooks.erase(clientId);
        if (m_mantleAuthority) {
            m_mantleAuthority->ForgetPlayer(clientId);
        }
        if (m_combatAuthority->RespawnParticipant(
                clientId, position, health)) {
            equipCombatLoadout(); // a deployment receives a fresh loadout
        } else {
            Logger::Warn(
                "[CombatAuthority] failed to respawn client %u at "
                "(%.1f, %.1f, %.1f)",
                clientId, position.x, position.y, position.z);
        }
    }
}

void GameServer::SynchronizeCombatParticipants() {
    if (!m_combatAuthority || !m_playerManager) return;
    for (const std::shared_ptr<Player>& player :
         m_playerManager->GetAllPlayers()) {
        if (!player || !player->GetConnection()) continue;
        SynchronizeCombatParticipant(
            player->GetConnection()->GetClientId(), false);
    }
}

bool GameServer::RequestRetailMantle(
    uint32_t clientId, const MantleRepl::Attempt& attempt,
    uint8_t& outSpecialMove, MantleRepl::DynamicInfo& outDynamicInfo) {
    outSpecialMove = 0;
    outDynamicInfo = {};
    if (!m_mantleAuthority || !m_playerManager || !m_mapManager ||
        !attempt.valid || !attempt.hasLastGoodInfo ||
        !attempt.lastGood.valid ||
        m_mapManager->GetCurrentMapName().empty()) {
        return false;
    }

    const std::shared_ptr<Player> player =
        m_playerManager->GetPlayer(clientId);
    if (!player || !player->IsAlive() ||
        (player->GetTeam() != 1u && player->GetTeam() != 2u)) {
        return false;
    }

    // GAP: MapManager exposes configured AABB bounds but no cooked-world
    // collision raycast; add the raycast here when export geometry is parsed.
    // Enforce a valid configured map AABB when present;
    // otherwise retain InputValidator's conservative absolute world envelope.
    // The LastGood surface is still treated as untrusted and must pass the
    // authority/normal/destination checks in MantleAuthority.
    Bounds worldBounds{
        Vector3{-100000.0f, -100000.0f, -100000.0f},
        Vector3{100000.0f, 100000.0f, 100000.0f}};
    const Bounds configuredBounds = m_mapManager->GetMapBounds();
    const auto finiteVector = [](const Vector3& value) {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
               std::isfinite(value.z);
    };
    if (finiteVector(configuredBounds.min) &&
        finiteVector(configuredBounds.max) &&
        configuredBounds.min.x < configuredBounds.max.x &&
        configuredBounds.min.y < configuredBounds.max.y &&
        configuredBounds.min.z < configuredBounds.max.z) {
        worldBounds = configuredBounds;
    }

    MantleAuthority::Request request;
    request.playerId = clientId;
    request.gameplayActive = IsBotGameplayLive();
    request.playerAlive = player->IsAlive();
    request.wantsToClimb = attempt.wantsToClimb;
    request.serverTimeSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    request.authoritativePosition = player->GetPosition();
    request.clientTracePosition = attempt.lastGood.playerLocation;
    request.hitLocation = attempt.lastGood.hitLocation;
    request.hitNormal = attempt.lastGood.hitNormal;
    request.worldBounds = worldBounds;

    const MantleAuthority::Decision decision =
        m_mantleAuthority->TryAccept(request);
    if (!decision.Accepted()) {
        Logger::Debug(
            "[MantleAuthority] client %u h280 rejected (reason %u)",
            clientId, static_cast<unsigned>(decision.reason));
        return false;
    }

    // Commit only after every validation and cooldown gate succeeds. This is
    // the sole position mutation caused by h280; an appended client ServerMove
    // is ignored by ConnectionManager for this transaction.
    player->SetPosition(decision.traversal.endLocation);
    SynchronizeCombatParticipant(clientId, false);

    outSpecialMove = decision.traversal.specialMove;
    outDynamicInfo.canDynamicMantle = true;
    outDynamicInfo.mantleCrouched =
        decision.traversal.mantleCrouched;
    outDynamicInfo.startLocation = decision.traversal.startLocation;
    outDynamicInfo.endLocation = decision.traversal.endLocation;
    outDynamicInfo.normal = decision.traversal.wallNormal;
    outDynamicInfo.height = decision.traversal.height;
    return true;
}

bool GameServer::HandleRetailCombatHit(
    uint32_t clientId,
    const WeaponCombatRepl::ServerHandleClientHitsOne& rpc) {
    if (!m_combatAuthority || rpc.firedMode > 1) return false;
    SynchronizeCombatParticipant(clientId, false);

    if (rpc.impact.participantId &&
        !rpc.impact.participantId->IsValid()) {
        return false;
    }

    Vector3 endTraceUu = rpc.firstHitLocation;
    // FirstHitLocation is the source-confirmed fire-line endpoint. Fall back to
    // the reported impact only for a degenerate equal-to-start endpoint; the
    // authority still rejects a zero-length or out-of-range segment.
    if (endTraceUu.Distance(rpc.startTrace) <= 1.0e-5f) {
        endTraceUu = rpc.impact.hitLocation;
    }

    if (rpc.impact.participantId &&
        rpc.impact.participantId->IsBot()) {
        CombatAuthority::ExternalHitscanRequest request;
        request.shooterId = clientId;
        request.claimedTargetId = *rpc.impact.participantId;
        request.startTraceUu = rpc.startTrace;
        request.endTraceUu = endTraceUu;
        request.reportedImpactUu = rpc.impact.hitLocation;

        // BotSnapshot is the same value view used by remote-pawn replication.
        // Include dead snapshots so CombatAuthority can reject TargetDead
        // before mutating cadence/ammo, and include every live bot so a client
        // cannot claim a farther target through a nearer authoritative capsule.
        if (m_botManager) {
            const std::vector<BotSnapshot> bots = m_botManager->GetBots();
            request.targets.reserve(bots.size());
            for (const BotSnapshot& bot : bots) {
                CombatAuthority::ExternalParticipantSpec target;
                target.id = bot.id;
                target.team = static_cast<uint32_t>(bot.teamId);
                target.positionUu = bot.position;
                target.hitVolume = CombatAuthority::HitVolume{};
                target.alive = bot.lifecycle == BotLifecycle::Alive;
                request.targets.push_back(target);
            }
        }

        const CombatAuthority::ExternalHitscanResult result =
            m_combatAuthority->FireHitscanExternal(request);
        ProcessCombatEvents(result.action.events);
        if (!result.action.accepted()) {
            Logger::Warn(
                "[CombatAuthority] client %u bot h56 rejected (reason %u)",
                clientId, static_cast<unsigned>(result.action.reason));
            return false;
        }
        if (!result.hit) {
            return true; // authoritative miss/mismatch; ammo was consumed once
        }

        ExternalBotDamageAuthorization botAuthorization;
        switch (result.hit->relationship) {
            case CombatAuthority::ExternalHitRelationship::Hostile:
                botAuthorization = ExternalBotDamageAuthorization::Hostile;
                break;
            case CombatAuthority::ExternalHitRelationship::FriendlyFireBlocked:
                // A geometrically valid blocked shot still fires and consumes
                // ammo, matching the existing Human->Human authority path.
                Logger::Debug(
                    "[CombatAuthority] client %u friendly fire on bot %u blocked",
                    clientId, result.hit->participantId.value);
                return true;
            case CombatAuthority::ExternalHitRelationship::FriendlyFireAuthorized:
                botAuthorization =
                    ExternalBotDamageAuthorization::FriendlyFireAuthorized;
                break;
            default:
                Logger::Error(
                    "[CombatAuthority] client %u produced invalid bot hit relationship",
                    clientId);
                return true;
        }

        // This is the only mutation of external health. BotManager queues one
        // combat/death transition; the normal next-tick queue collapse then
        // emits one final replication snapshot for the bot.
        const bool applied = m_botManager &&
            m_botManager->ApplyExternalDamageToBot(
                result.hit->participantId, ParticipantId::Human(clientId),
                result.hit->damage, botAuthorization);
        if (!applied) {
            Logger::Error(
                "[CombatAuthority] accepted bot h56 from client %u but failed "
                "to apply %.1f damage to bot %u",
                clientId, result.hit->damage,
                result.hit->participantId.value);
        }
        return true;
    }

    CombatAuthority::HitscanRequest request;
    request.shooterId = clientId;
    if (rpc.impact.participantId &&
        rpc.impact.participantId->IsHuman()) {
        request.claimedTargetId = rpc.impact.participantId->value;
    }
    request.startTraceUu = rpc.startTrace;
    request.endTraceUu = endTraceUu;
    request.reportedImpactUu = rpc.impact.hitLocation;

    // A decoded future remote actor may be an admin-protected player. Preserve
    // cadence/ammo semantics by authorizing the report as a miss instead of
    // ever routing it through the legacy DamageSystem or damaging god mode.
    if (request.claimedTargetId && m_playerManager) {
        const std::shared_ptr<Player> target =
            m_playerManager->GetPlayer(*request.claimedTargetId);
        if (target && target->IsGodMode()) {
            request.claimedTargetId.reset();
        }
    }

    const CombatAuthority::ActionResult result =
        m_combatAuthority->FireHitscan(request);
    ProcessCombatEvents(result.events);
    if (!result.accepted()) {
        Logger::Warn(
            "[CombatAuthority] client %u h56 rejected (reason %u)",
            clientId, static_cast<unsigned>(result.reason));
    }
    return result.accepted();
}

bool GameServer::RequestCombatReload(uint32_t clientId) {
    if (!m_combatAuthority) return false;
    SynchronizeCombatParticipant(clientId, false);
    const CombatAuthority::ActionResult result =
        m_combatAuthority->RequestReload(clientId);
    ProcessCombatEvents(result.events);
    return result.accepted();
}

bool GameServer::SelectCombatWeaponChannel(uint32_t clientId,
                                           uint32_t weaponChannel,
                                           uint32_t weaponClassRef) {
    if (!m_combatAuthority || !m_playerManager) return false;
    SynchronizeCombatParticipant(clientId, false);

    const std::shared_ptr<Player> player =
        m_playerManager->GetPlayer(clientId);
    if (!player) return false;
    const uint32_t team = player->GetTeam();

    std::string weaponId;
    if (weaponChannel == 212u) {
        constexpr uint32_t kM61WeaponClassRef = 286464u;
        constexpr uint32_t kType67WeaponClassRef = 286804u;
        if (team == 1u && weaponClassRef == kM61WeaponClassRef) {
            weaponId = "M61";
        } else if (team == 2u && weaponClassRef == kType67WeaponClassRef) {
            weaponId = "Type67";
        } else {
            return false;
        }
    } else if (weaponChannel == 210u) {
        constexpr uint32_t kM16A1WeaponClassRef = 286374u;
        constexpr uint32_t kType56WeaponClassRef = 286271u;
        if ((team == 1u && weaponClassRef != kM16A1WeaponClassRef) ||
            (team == 2u && weaponClassRef != kType56WeaponClassRef) ||
            (team != 1u && team != 2u)) {
            return false;
        }
        CombatAuthority::WeaponProfile primary;
        if (!BuildDefaultCombatWeaponProfile(
                m_weaponDatabase.get(), team, primary)) {
            return false;
        }
        weaponId = primary.id;
    } else {
        return false;
    }
    const bool selected =
        m_combatAuthority->SetActiveWeapon(clientId, weaponId);
    if (selected && weaponChannel != 212u) {
        m_retailGrenadeCooks.erase(clientId);
    }
    return selected;
}

bool GameServer::BeginRetailGrenadeCook(uint32_t clientId,
                                        uint8_t fireMode) {
    if (!m_combatAuthority ||
        (fireMode != GameplayRpc::kM61OverhandFireMode &&
         fireMode != GameplayRpc::kM61TossFireMode)) {
        return false;
    }

    SynchronizeCombatParticipant(clientId, false);
    const auto participant = m_combatAuthority->GetParticipant(clientId);
    if (!participant || !participant->alive ||
        (participant->activeWeaponId != "M61" &&
         participant->activeWeaponId != "Type67")) {
        return false;
    }
    const std::string& grenadeId = participant->activeWeaponId;
    const auto grenade = participant->weapons.find(grenadeId);
    if (grenade == participant->weapons.end() ||
        grenade->second.magazineAmmo == 0 || grenade->second.reloading) {
        return false;
    }

    // Reliable actor-bunch retransmission may deliver the same h29 more than
    // once. A same-mode duplicate acknowledges the existing cook without ever
    // moving its start time; a conflicting mode cannot replace it.
    if (const auto existing = m_retailGrenadeCooks.find(clientId);
        existing != m_retailGrenadeCooks.end()) {
        return existing->second.fireMode == fireMode;
    }

    const double now = m_combatAuthority->GetTimeSeconds();
    if (!std::isfinite(now) || now < 0.0) return false;
    const RetailGrenadeCookState::Variant variant =
        grenadeId == "Type67"
            ? RetailGrenadeCookState::Variant::Type67
            : RetailGrenadeCookState::Variant::M61;
    m_retailGrenadeCooks.emplace(
        clientId, RetailGrenadeCookState{fireMode, now, variant});
    return true;
}

bool GameServer::ReleaseRetailGrenade(uint32_t clientId, uint8_t fireMode,
                                      const Vector3& aimDirection) {
    constexpr uint32_t kM61ProjectileClassRef = 60245;
    if (!m_combatAuthority ||
        (fireMode != GameplayRpc::kM61OverhandFireMode &&
         fireMode != GameplayRpc::kM61TossFireMode)) {
        return false;
    }
    const auto cook = m_retailGrenadeCooks.find(clientId);
    if (cook == m_retailGrenadeCooks.end() ||
        cook->second.fireMode != fireMode) {
        return false;
    }
    const RetailGrenadeCookState cookState = cook->second;
    // h30 is a one-shot transition even when later validation fails. Releasing
    // a cook never leaves a stale entry that can be replayed after a weapon or
    // lifecycle change.
    m_retailGrenadeCooks.erase(cook);

    SynchronizeCombatParticipant(clientId, false);
    const auto participant = m_combatAuthority->GetParticipant(clientId);
    const bool type67 =
        cookState.variant == RetailGrenadeCookState::Variant::Type67;
    const char* grenadeId = type67 ? "Type67" : "M61";
    if (!participant || !participant->alive ||
        participant->activeWeaponId != grenadeId) {
        return false;
    }
    const auto grenade = participant->weapons.find(grenadeId);
    if (grenade == participant->weapons.end() ||
        grenade->second.magazineAmmo == 0 || grenade->second.reloading) {
        return false;
    }

    const double now = m_combatAuthority->GetTimeSeconds();
    const double elapsed = now - cookState.beganAtAuthoritySeconds;
    const GameplayRpc::M61ThrowParameters throwParameters = type67
        ? GameplayRpc::BuildType67ThrowParameters(
              aimDirection, fireMode, elapsed)
        : GameplayRpc::BuildM61ThrowParameters(
              aimDirection, fireMode, elapsed);
    if (!std::isfinite(now) || now < cookState.beganAtAuthoritySeconds ||
        !throwParameters.valid) {
        return false;
    }

    const GameplayRpc::AimDirection normalizedAim =
        GameplayRpc::NormalizeAimDirection(aimDirection);
    if (!normalizedAim.valid) return false;
    // ROThrowableExplosiveProjectile::Init always adds TossZ. M61 keeps the
    // 180-UU default for overhand throws; fire mode 1 replaces it with the
    // source UnderhandTossZ=150 UU. Rebuild velocity here so overhand throws do
    // not silently lose their vertical component.
    constexpr float kM61OverhandTossZUu = 180.0f;
    constexpr float kM61UnderhandTossZUu = 150.0f;
    Vector3 launchVelocityUu = normalizedAim.value *
        throwParameters.baseSpeedUuPerSecond;
    launchVelocityUu.z += fireMode == GameplayRpc::kM61TossFireMode
        ? kM61UnderhandTossZUu
        : kM61OverhandTossZUu;
    const float launchSpeedUuPerSecond = launchVelocityUu.Length();
    const Vector3 launchDirection = launchVelocityUu.Normalized();
    Vector3 originUu = participant->positionUu +
        normalizedAim.value * 30.0f;
    originUu.z += 50.0f;
    // ROPawn's source collision cylinder has CollisionHeight=42 UU. Until the
    // map layer exposes a cooked-world raycast, the authoritative standing pawn
    // bottom is the narrowest source-grounded launch-plane estimate available.
    // This is intentionally local to the throw; CombatAuthority still requires
    // callers to provide explicit finite ground evidence.
    constexpr float kRetailPawnCollisionHeightUu = 42.0f;
    const float groundHeightUu =
        participant->positionUu.z - kRetailPawnCollisionHeightUu;
    const float combatUnitsPerMeter =
        m_combatAuthority->GetConfig().ueUnitsPerMeter;
    if (!std::isfinite(originUu.x) || !std::isfinite(originUu.y) ||
        !std::isfinite(originUu.z) || !std::isfinite(groundHeightUu) ||
        !std::isfinite(combatUnitsPerMeter) || combatUnitsPerMeter <= 0.0f ||
        !std::isfinite(launchSpeedUuPerSecond) ||
        launchSpeedUuPerSecond <= 0.0f ||
        !std::isfinite(launchDirection.x) ||
        !std::isfinite(launchDirection.y) ||
        !std::isfinite(launchDirection.z)) {
        return false;
    }

    CombatAuthority::ProjectileRequest request;
    request.shooterId = clientId;
    request.originUu = originUu;
    request.direction = launchDirection;
    request.weaponId = grenadeId;
    request.projectile.launchSpeedMetersPerSecond =
        launchSpeedUuPerSecond / combatUnitsPerMeter;
    request.projectile.gravityMetersPerSecondSquared =
        {0.0f, 0.0f, -9.81f};
    request.projectile.fuseSeconds = throwParameters.fuseSeconds;
    request.projectile.maxLifetimeSeconds = 10.0f;
    // M61GrenadeProjectile: radius/height 2 UU, DampenFactor=0.33,
    // Bounces=5. ROStickGrenadeProjectile decrements before checking zero,
    // reflects four times, and settles on contact five. It then applies an
    // additional 0.85 multiplier to reflected Z.
    request.projectile.collisionRadiusMeters = 2.0f / combatUnitsPerMeter;
    request.projectile.directDamage = 0.0f;
    request.projectile.explosionRadiusMeters = 12.5f; // 625 UU
    request.projectile.maxExplosionDamage = 200.0f;
    // Retain explicit server-known ground evidence even though the M61 is
    // fuse-triggered rather than impact-triggered.
    request.projectile.groundHeightUu = groundHeightUu;
    request.projectile.detonateOnGround = false;
    request.projectile.detonateOnParticipant = false;
    request.projectile.ownerCollisionGraceSeconds = 0.2f;
    request.projectile.maxGroundImpacts = 5;
    request.projectile.groundBounceDamping = 0.33f;
    request.projectile.groundBounceVerticalDamping = 0.85f;

    const CombatAuthority::SpawnResult spawned =
        m_combatAuthority->SpawnProjectile(request);
    ProcessCombatEvents(spawned.action.events);
    if (!spawned.action.accepted()) {
        Logger::Warn(
            "[CombatAuthority] client %u M61 throw rejected (reason %u)",
            clientId, static_cast<unsigned>(spawned.action.reason));
        return false;
    }

    if (type67) {
        Logger::Info(
            "[CombatAuthority] client %u spawned authoritative Type67 projectile %llu "
            "(cook %.2fs, fuse %.2fs); retail visual suppressed until its "
            "capture-era projectile CDO/schema is grounded",
            clientId, static_cast<unsigned long long>(spawned.projectileId),
            throwParameters.cookSeconds, throwParameters.fuseSeconds);
    } else {
        Logger::Info(
            "[CombatAuthority] client %u spawned authoritative M61 projectile %llu "
            "(retail class ref %u, cook %.2fs, fuse %.2fs); "
            "retail visual actor replication queued",
            clientId, static_cast<unsigned long long>(spawned.projectileId),
            kM61ProjectileClassRef, throwParameters.cookSeconds,
            throwParameters.fuseSeconds);
    }
    return true;
}

void GameServer::AdvanceRetailGrenadeCooks() {
    if (!m_combatAuthority || m_retailGrenadeCooks.empty()) return;

    const double now = m_combatAuthority->GetTimeSeconds();
    if (!std::isfinite(now) || now < 0.0) return;

    std::vector<std::pair<uint32_t, uint8_t>> expired;
    expired.reserve(m_retailGrenadeCooks.size());
    for (const auto& [clientId, cook] : m_retailGrenadeCooks) {
        const double maximumCookSeconds =
            cook.variant == RetailGrenadeCookState::Variant::Type67
                ? GameplayRpc::kType67MaximumCookSeconds
                : GameplayRpc::kM61MaximumCookSeconds;
        if (!std::isfinite(cook.beganAtAuthoritySeconds) ||
            now < cook.beganAtAuthoritySeconds ||
            now - cook.beganAtAuthoritySeconds >=
                maximumCookSeconds) {
            expired.emplace_back(clientId, cook.fireMode);
        }
    }

    for (const auto& [clientId, fireMode] : expired) {
        // h29 starts a fuse, so losing/withholding h30 cannot preserve a live
        // grenade indefinitely. Use the player's last server-known facing when
        // valid; the deterministic +X fallback merely satisfies the projectile
        // API for a near-immediate radial in-hand detonation.
        Vector3 aim{1.0f, 0.0f, 0.0f};
        if (m_playerManager) {
            if (const std::shared_ptr<Player> player =
                    m_playerManager->GetPlayer(clientId)) {
                const GameplayRpc::AimDirection facing =
                    GameplayRpc::NormalizeAimDirection(
                        player->GetOrientation());
                if (facing.valid) aim = facing.value;
            }
        }

        Logger::Warn(
            "[CombatAuthority] client %u held faction grenade through its fuse; "
            "forcing authoritative in-hand detonation",
            clientId);
        if (!ReleaseRetailGrenade(clientId, fireMode, aim)) {
            CancelRetailGrenadeCook(clientId);
        }
    }
}

void GameServer::CancelRetailGrenadeCook(uint32_t clientId) {
    m_retailGrenadeCooks.erase(clientId);
}

void GameServer::ReplicateRetailM61Projectiles(float deltaSeconds) {
    constexpr float kReplicationIntervalSeconds = 0.1f;
    if (!m_networkManager || !m_combatAuthority ||
        m_retailM61Projectiles.empty() || !std::isfinite(deltaSeconds) ||
        deltaSeconds < 0.0f) {
        return;
    }
    m_retailM61ReplicationAccumulator = std::min(
        m_retailM61ReplicationAccumulator + deltaSeconds,
        kReplicationIntervalSeconds * 2.0f);
    if (m_retailM61ReplicationAccumulator + 1.0e-6f <
        kReplicationIntervalSeconds) {
        return;
    }
    m_retailM61ReplicationAccumulator = std::fmod(
        m_retailM61ReplicationAccumulator, kReplicationIntervalSeconds);

    std::vector<uint64_t> retired;
    retired.reserve(m_retailM61Projectiles.size());
    for (const uint64_t projectileId : m_retailM61Projectiles) {
        const auto projectile = m_combatAuthority->GetProjectile(projectileId);
        if (!projectile || projectile->weaponId != "M61") {
            m_networkManager->BroadcastRetailM61Remove(projectileId);
            retired.push_back(projectileId);
            continue;
        }
        WeaponCombatRepl::M61VisualSnapshot snapshot;
        if (!BuildM61VisualSnapshot(
                *m_combatAuthority, *projectile, snapshot)) {
            m_networkManager->BroadcastRetailM61Remove(projectileId);
            retired.push_back(projectileId);
            continue;
        }
        m_networkManager->BroadcastRetailM61Update(projectileId, snapshot);
    }
    for (const uint64_t projectileId : retired) {
        m_retailM61Projectiles.erase(projectileId);
    }
}

bool GameServer::ProcessBotHumanCombatEvent(
    const BotHumanCombatEvent& event) {
    if (!m_botManager || !m_combatAuthority || !m_playerManager ||
        !m_botManager->IsPendingHumanCombatEvent(event)) {
        return false;
    }
    const auto reject = [this, &event]() {
        m_botManager->ResolveHumanCombatEvent(
            event, BotHumanCombatOutcome::Rejected);
        return false;
    };

    const BotManagerConfig& config = m_botManager->GetConfig();
    const BotSnapshot* attacker = m_botManager->FindBot(event.attackerId);
    const float measuredDistance = event.origin.Distance(event.impact);
    if (!attacker || !event.attackerId.IsBot() ||
        !event.victimId.IsHuman() ||
        attacker->teamId != event.attackerTeamId ||
        (event.attackerTeamId != BotManager::kTeamOne &&
         event.attackerTeamId != BotManager::kTeamTwo) ||
        !std::isfinite(measuredDistance) ||
        !std::isfinite(event.distanceUu) ||
        !std::isfinite(event.damage) || event.damage <= 0.0f ||
        event.distanceUu > config.combatRangeUu ||
        std::fabs(measuredDistance - event.distanceUu) >
            std::max(0.01f, measuredDistance * 0.0001f)) {
        return reject();
    }

    const std::shared_ptr<Player> target =
        m_playerManager->GetPlayer(event.victimId.value);
    if (!target || !target->IsAlive() || target->IsGodMode() ||
        (target->GetTeam() != BotManager::kTeamOne &&
         target->GetTeam() != BotManager::kTeamTwo) ||
        target->GetTeam() == event.attackerTeamId) {
        return reject();
    }

    CombatAuthority::ExternalDamageRequest request;
    request.sourceId = event.attackerId;
    request.sourceTeam = event.attackerTeamId;
    request.targetId = event.victimId.value;
    request.weaponId = "BotRifle";
    request.originUu = event.origin;
    request.impactUu = event.impact;
    request.maxRangeUu = config.combatRangeUu;
    request.damage = event.damage;
    request.hitZone = CombatAuthority::HitZone::UpperTorso;
    const CombatAuthority::ExternalDamageResult result =
        m_combatAuthority->ApplyExternalDamage(request);
    if (!result.applied) {
        Logger::Debug(
            "[CombatAuthority] bot %u -> human %u rejected (reason %u)",
            event.attackerId.value, event.victimId.value,
            static_cast<unsigned>(result.reason));
        return reject();
    }

    const int health = std::clamp(
        static_cast<int>(std::lround(result.healthAfter)), 0, 100);
    target->SetHealth(health);
    if (result.killed) {
        m_retailGrenadeCooks.erase(event.victimId.value);
        m_playerManager->OnPlayerDeath(event.victimId.value);
        m_playerManager->AddPlayerDeath(event.victimId.value);
        if (m_ticketSystem &&
            m_ticketSystem->GetInitialTickets(target->GetTeam()) > 0) {
            m_ticketSystem->OnPlayerKilled(target->GetTeam());
        }
        // The native mode callbacks currently use ids only as a transition
        // notification. Preserve the tagged identity everywhere that stores or
        // scores it; pass the raw bot value only across this legacy no-storage
        // boundary until the mode API itself becomes participant-typed.
        switch (m_activeModeDriver) {
            case ActiveModeDriver::Territory:
                if (m_territoryMode) {
                    m_territoryMode->OnPlayerKilled(
                        event.attackerId.value, event.victimId.value);
                }
                break;
            case ActiveModeDriver::Supremacy:
                if (m_supremacyMode) {
                    m_supremacyMode->OnPlayerKilled(
                        event.attackerId.value, event.victimId.value);
                }
                break;
            case ActiveModeDriver::Skirmish:
                if (m_skirmishMode) {
                    m_skirmishMode->OnPlayerKilled(
                        event.attackerId.value, event.victimId.value);
                }
                break;
            case ActiveModeDriver::Generic:
                break;
        }
    }

    const BotHumanCombatOutcome outcome = result.killed
        ? BotHumanCombatOutcome::TargetKilled
        : BotHumanCombatOutcome::DamageApplied;
    const bool resolved =
        m_botManager->ResolveHumanCombatEvent(event, outcome);
    if (!resolved) {
        Logger::Error(
            "[GameServer] bot-human shot %llu applied but resolution failed",
            static_cast<unsigned long long>(event.shotSequence));
    }

    if (m_networkManager) {
        m_networkManager->ReplicateRetailCombatState(
            event.victimId.value, target->GetHealth(),
            m_playerManager->GetPlayerKills(event.victimId.value),
            m_playerManager->GetPlayerDeaths(event.victimId.value),
            m_playerManager->GetPlayerScore(event.victimId.value),
            !target->IsAlive(), /*sendHealth=*/true,
            /*sendDeathRpc=*/result.killed);
    }
    Logger::Info(
        "[BotCombat] Bot(%u) hit Human(%u) for %.1f (%d health)%s",
        event.attackerId.value, event.victimId.value, result.damage, health,
        result.killed ? " [KILL]" : "");
    return result.killed && resolved;
}

void GameServer::ProcessCombatEvents(
    const std::vector<CombatAuthority::CombatEvent>& events) {
    if (events.empty()) return;

    auto replicateState = [this](uint32_t participantId, bool sendHealth,
                                 bool sendDeathRpc) {
        if (!m_playerManager || !m_networkManager) return;
        const std::shared_ptr<Player> player =
            m_playerManager->GetPlayer(participantId);
        if (!player) return;
        m_networkManager->ReplicateRetailCombatState(
            participantId, player->GetHealth(),
            m_playerManager->GetPlayerKills(participantId),
            m_playerManager->GetPlayerDeaths(participantId),
            m_playerManager->GetPlayerScore(participantId),
            !player->IsAlive(), sendHealth, sendDeathRpc);
    };

    for (const CombatAuthority::CombatEvent& event : events) {
        switch (event.kind) {
            case CombatAuthority::EventKind::DamageApplied: {
                if (!m_playerManager) break;
                const std::shared_ptr<Player> target =
                    m_playerManager->GetPlayer(event.targetId);
                if (!target) break;
                const int health = std::clamp(
                    static_cast<int>(std::lround(event.healthAfter)), 0, 100);
                target->SetHealth(health);
                replicateState(event.targetId, true, false);
                Logger::Info(
                    "[CombatAuthority] %u -> %u with %s: %.1f damage, %d health",
                    event.sourceId, event.targetId, event.weaponId.c_str(),
                    event.damage, health);
                break;
            }
            case CombatAuthority::EventKind::ParticipantDied: {
                m_retailGrenadeCooks.erase(event.targetId);
                if (!m_playerManager) break;
                if (const std::shared_ptr<Player> target =
                        m_playerManager->GetPlayer(event.targetId)) {
                    if (target->GetHealth() > 0) target->SetHealth(0);
                    m_playerManager->OnPlayerDeath(event.targetId);
                    m_playerManager->AddPlayerDeath(event.targetId);
                }
                if (event.sourceId != event.targetId &&
                    m_playerManager->GetPlayer(event.sourceId)) {
                    m_playerManager->AddPlayerKill(event.sourceId);
                    if (!event.friendlyFire) {
                        m_playerManager->AddPlayerScore(event.sourceId, 1);
                    }
                }

                if (m_ticketSystem && !event.friendlyFire && m_teamManager) {
                    m_ticketSystem->OnPlayerKilled(
                        m_teamManager->GetPlayerTeam(event.targetId));
                }
                switch (m_activeModeDriver) {
                    case ActiveModeDriver::Territory:
                        if (m_territoryMode) {
                            m_territoryMode->OnPlayerKilled(
                                event.sourceId, event.targetId);
                        }
                        break;
                    case ActiveModeDriver::Supremacy:
                        if (m_supremacyMode) {
                            m_supremacyMode->OnPlayerKilled(
                                event.sourceId, event.targetId);
                        }
                        break;
                    case ActiveModeDriver::Skirmish:
                        if (m_skirmishMode) {
                            m_skirmishMode->OnPlayerKilled(
                                event.sourceId, event.targetId);
                        }
                        break;
                    case ActiveModeDriver::Generic:
                        break;
                }
                if (m_mutatorManager) {
                    m_mutatorManager->DispatchPlayerKilled(
                        event.sourceId, event.targetId);
                }

                replicateState(event.targetId, true, true);
                if (event.sourceId != event.targetId) {
                    replicateState(event.sourceId, false, false);
                }
                Logger::Info(
                    "[CombatAuthority] %u killed %u with %s%s",
                    event.sourceId, event.targetId, event.weaponId.c_str(),
                    event.friendlyFire ? " (team kill)" : "");
                break;
            }
            case CombatAuthority::EventKind::ReloadStarted:
                Logger::Debug(
                    "[CombatAuthority] client %u started reloading %s (%u/%u)",
                    event.sourceId, event.weaponId.c_str(),
                    event.magazineAmmo, event.reserveAmmo);
                break;
            case CombatAuthority::EventKind::ReloadCompleted:
                Logger::Debug(
                    "[CombatAuthority] client %u completed reload %s (%u/%u)",
                    event.sourceId, event.weaponId.c_str(),
                    event.magazineAmmo, event.reserveAmmo);
                break;
            case CombatAuthority::EventKind::WeaponFired:
            case CombatAuthority::EventKind::FriendlyFireBlocked:
                break;
            case CombatAuthority::EventKind::ProjectileSpawned: {
                if (event.weaponId != "M61" || event.projectileId == 0 ||
                    !m_networkManager || !m_combatAuthority) {
                    break;
                }
                const auto projectile =
                    m_combatAuthority->GetProjectile(event.projectileId);
                WeaponCombatRepl::M61VisualSnapshot snapshot;
                if (!projectile ||
                    !BuildM61VisualSnapshot(
                        *m_combatAuthority, *projectile, snapshot)) {
                    Logger::Warn(
                        "[M61Visual] rejected authoritative projectile %llu "
                        "snapshot",
                        static_cast<unsigned long long>(event.projectileId));
                    break;
                }
                m_networkManager->BroadcastRetailM61Spawn(
                    event.projectileId, event.sourceId, snapshot);
                m_retailM61Projectiles.insert(event.projectileId);
                break;
            }
            case CombatAuthority::EventKind::ProjectileDetonated: {
                if (event.weaponId == "M61" && event.projectileId != 0) {
                    if (m_networkManager) {
                        m_networkManager->BroadcastRetailM61Detonate(
                            event.projectileId, 0.0f);
                    }
                    m_retailM61Projectiles.erase(event.projectileId);
                }
                // CombatAuthority owns human capsules, while headless bots
                // intentionally live in BotManager. Bridge the same captured
                // projectile metadata into that second authoritative roster
                // exactly once at detonation.
                if (!m_botManager || !m_combatAuthority || event.sourceId == 0 ||
                    (event.sourceTeam != BotManager::kTeamOne &&
                     event.sourceTeam != BotManager::kTeamTwo) ||
                    !std::isfinite(event.positionUu.x) ||
                    !std::isfinite(event.positionUu.y) ||
                    !std::isfinite(event.positionUu.z) ||
                    !std::isfinite(event.explosionRadiusMeters) ||
                    event.explosionRadiusMeters <= 0.0f ||
                    !std::isfinite(event.maxExplosionDamage) ||
                    event.maxExplosionDamage <= 0.0f) {
                    break;
                }

                const CombatAuthority::AuthorityConfig& config =
                    m_combatAuthority->GetConfig();
                const double radiusUu =
                    static_cast<double>(event.explosionRadiusMeters) *
                    static_cast<double>(config.ueUnitsPerMeter);
                if (!std::isfinite(radiusUu) || radiusUu <= 0.0) break;

                const ParticipantId attacker =
                    ParticipantId::Human(event.sourceId);
                for (const BotSnapshot& bot : m_botManager->GetBots()) {
                    if (bot.lifecycle != BotLifecycle::Alive ||
                        (bot.teamId != BotManager::kTeamOne &&
                         bot.teamId != BotManager::kTeamTwo) ||
                        !std::isfinite(bot.position.x) ||
                        !std::isfinite(bot.position.y) ||
                        !std::isfinite(bot.position.z)) {
                        continue;
                    }

                    const double dx = static_cast<double>(bot.position.x) -
                        static_cast<double>(event.positionUu.x);
                    const double dy = static_cast<double>(bot.position.y) -
                        static_cast<double>(event.positionUu.y);
                    const double dz = static_cast<double>(bot.position.z) -
                        static_cast<double>(event.positionUu.z);
                    const double distanceUu = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (!std::isfinite(distanceUu) || distanceUu > radiusUu) continue;

                    float damage = event.maxExplosionDamage * static_cast<float>(
                        1.0 - std::clamp(distanceUu / radiusUu, 0.0, 1.0));
                    ExternalBotDamageAuthorization authorization =
                        ExternalBotDamageAuthorization::Hostile;
                    if (bot.teamId == event.sourceTeam) {
                        if (!config.friendlyFireEnabled) {
                            authorization =
                                ExternalBotDamageAuthorization::FriendlyFireBlocked;
                        } else {
                            damage *= config.friendlyFireDamageScale;
                            authorization =
                                ExternalBotDamageAuthorization::FriendlyFireAuthorized;
                        }
                    }
                    if (!std::isfinite(damage) || damage <= 0.0f) continue;
                    (void)m_botManager->ApplyExternalDamageToBot(
                        bot.id, attacker, damage, authorization);
                }
                break;
            }
            case CombatAuthority::EventKind::ProjectileRemoved:
                if (event.weaponId == "M61" && event.projectileId != 0) {
                    if (m_networkManager) {
                        m_networkManager->BroadcastRetailM61Remove(
                            event.projectileId);
                    }
                    m_retailM61Projectiles.erase(event.projectileId);
                }
                break;
        }
    }
}

// --- Handler Regen/Reload Implementation ---

void GameServer::Cmd_RegenHandlers(const std::vector<std::string>& /*args*/) {
    Logger::Trace("[GameServer::Cmd_RegenHandlers] Entry");
    std::string codeGenExe = PathUtils::ResolveFromExecutable("PacketHandlerCodeGen");
#ifdef _WIN32
    if (codeGenExe.find(".exe") == std::string::npos) codeGenExe += ".exe";
#endif

    // GUARD (latent-defect fix): never shell out to a tool that isn't there.
    // The PacketHandlerCodeGen executable is a build-time tool and is not
    // guaranteed to be deployed next to the server. If it's missing, log a
    // clear warning and no-op instead of invoking std::system() on a bogus
    // command line (which previously fired every hour from a detached thread).
    if (!std::filesystem::exists(codeGenExe)) {
        Logger::Warn("[GameServer::Cmd_RegenHandlers] Code generator not found at '%s'. "
                     "Skipping handler regeneration. Generated handlers are compiled "
                     "statically at build time; runtime regeneration is optional and "
                     "only available when PacketHandlerCodeGen is deployed.",
                     codeGenExe.c_str());
        Logger::Trace("[GameServer::Cmd_RegenHandlers] Exit (code generator missing)");
        return;
    }

    std::string handlersDir = PathUtils::ResolveFromExecutable("src/Generated/Handlers");
    std::string cmd = codeGenExe + " " + handlersDir;

    Logger::Info("[GameServer::Cmd_RegenHandlers] Executing handler regeneration: %s", cmd.c_str());
    int ret = std::system(cmd.c_str());
    if (ret == 0) {
        Logger::Debug("[GameServer::Cmd_RegenHandlers] System command succeeded, reloading handlers");
        Logger::Info("Handler stubs regenerated successfully.");
        DynamicReloadGeneratedHandlers();
    } else {
        Logger::Error("[GameServer::Cmd_RegenHandlers] Failed to regenerate handler stubs (exit %d)", ret);
    }
    Logger::Trace("[GameServer::Cmd_RegenHandlers] Exit");
}

void GameServer::StartAutoRegen(int intervalSeconds) {
    Logger::Trace("[GameServer::StartAutoRegen] Entry, intervalSeconds=%d", intervalSeconds);
    if (m_regenRunning) {
        Logger::Debug("[GameServer::StartAutoRegen] Already running, skipping");
        Logger::Trace("[GameServer::StartAutoRegen] Exit (already running)");
        return;
    }
    m_regenRunning = true;
    m_regenIntervalSeconds = intervalSeconds;
    Logger::Debug("[GameServer::StartAutoRegen] Starting regen thread with interval %d seconds", intervalSeconds);
    m_regenThread = std::thread([this]() {
        Logger::Info("Auto handler regeneration thread started (interval: %d seconds)", m_regenIntervalSeconds);
        while (m_regenRunning) {
            // Sleep for the configured interval, checking for shutdown each second
            for (int i = 0; i < m_regenIntervalSeconds && m_regenRunning; ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (m_regenRunning) {
                Logger::Debug("[GameServer::StartAutoRegen] Triggering periodic handler regeneration");
                // Detached thread: an uncaught exception here would reach
                // std::terminate with no one to join/observe it. Guard so a
                // failed regeneration is reported non-fatally and the periodic
                // thread keeps running.
                rs2v::Guard("auto handler regen", [this] { Cmd_RegenHandlers(); });
            }
        }
        Logger::Info("Auto handler regeneration thread stopped");
    });
    m_regenThread.detach();
    Logger::Trace("[GameServer::StartAutoRegen] Exit");
}

void GameServer::StopAutoRegen() {
    Logger::Trace("[GameServer::StopAutoRegen] Entry");
    m_regenRunning = false;
    Logger::Info("Auto handler regeneration requested to stop.");
    Logger::Trace("[GameServer::StopAutoRegen] Exit");
}

void GameServer::DynamicReloadGeneratedHandlers() {
    Logger::Trace("[GameServer::DynamicReloadGeneratedHandlers] Entry");

    auto& mgr = HandlerLibraryManager::Instance();

    if (m_handlerLibraryPath.empty()) {
        Logger::Debug("[GameServer::DynamicReloadGeneratedHandlers] Library path empty, resolving platform-specific path");
#ifdef _WIN32
        m_handlerLibraryPath = PathUtils::ResolveFromExecutable("GeneratedHandlers.dll");
#elif defined(__APPLE__)
        m_handlerLibraryPath = PathUtils::ResolveFromExecutable("libGeneratedHandlers.dylib");
#else
        m_handlerLibraryPath = PathUtils::ResolveFromExecutable("libGeneratedHandlers.so");
#endif
        Logger::Debug("[GameServer::DynamicReloadGeneratedHandlers] Resolved library path: %s", m_handlerLibraryPath.c_str());

        // GUARD (latent-defect fix): the dynamic hot-reload library is OFF by
        // default and is not shipped. If it isn't present, do NOT try to load
        // it — fall back to the statically-compiled handlers (the normal path).
        if (!std::filesystem::exists(m_handlerLibraryPath)) {
            if (mgr.HasStaticHandlers()) {
                Logger::Info("Using statically-compiled generated handlers (no dynamic library present at '%s').",
                             m_handlerLibraryPath.c_str());
            } else {
                Logger::Warn("No generated handlers available: dynamic library '%s' not found and "
                             "no static handler registry was compiled in. Packet handling will rely "
                             "on built-in handlers only.", m_handlerLibraryPath.c_str());
            }
            Logger::Trace("[GameServer::DynamicReloadGeneratedHandlers] Exit (no dynamic library; using static fallback)");
            return;
        }

        // A dynamic library IS present — opt into the hot-reload path.
        if (!mgr.Initialize(m_handlerLibraryPath)) {
            Logger::Error("[GameServer::DynamicReloadGeneratedHandlers] Failed to initialize handler library manager; "
                          "continuing with static handlers if available");
            Logger::Trace("[GameServer::DynamicReloadGeneratedHandlers] Exit (init failed)");
            return;
        }
        Logger::Debug("[GameServer::DynamicReloadGeneratedHandlers] Handler library manager initialized");
    }

    // Only force a reload when a dynamic library has actually been initialized.
    // ForceReload() itself no-ops + warns otherwise, so this is doubly safe.
    if (mgr.ForceReload()) {
        Logger::Info("Generated handlers reloaded successfully.");
    } else {
        Logger::Warn("[GameServer::DynamicReloadGeneratedHandlers] Dynamic reload unavailable; "
                     "using statically-compiled handlers.");
    }
    Logger::Trace("[GameServer::DynamicReloadGeneratedHandlers] Exit");
}

// --- Core GameServer Implementation ---

GameServer::GameServer() {
    Logger::Trace("[GameServer::GameServer] Entry");
    Logger::Info("GameServer constructed");
    Logger::Trace("[GameServer::GameServer] Exit");
}

GameServer::~GameServer() {
    Logger::Trace("[GameServer::~GameServer] Entry");
    Shutdown();
    StopAutoRegen();
    GetProtocolDecoder().Shutdown();
    HandlerLibraryManager::Instance().Shutdown();
    Logger::Trace("[GameServer::~GameServer] Exit");
}

std::string GameServer::GetEffectiveGameModeName() const {
    // An explicit server setting wins.  When it is absent (the shipped local
    // config only specifies initial_map), use the loaded map's declared mode
    // instead of GameConfig's unrelated generic "Conquest" fallback.
    if (m_configManager && m_configManager->HasKey("Game.game_mode")) {
        const std::string configured = m_configManager->GetString("Game.game_mode", "");
        if (!configured.empty()) return configured;
    }
    if (m_mapManager) {
        const std::string& mapDefault = m_mapManager->GetCurrentMap().defaultMode;
        if (!mapDefault.empty()) return mapDefault;
    }
    return m_gameConfig ? m_gameConfig->GetGameSettings().gameMode : std::string("Territories");
}

GameServer::ActiveModeDriver GameServer::ResolveActiveModeDriver() const {
    std::string mode = GetEffectiveGameModeName();
    std::transform(mode.begin(), mode.end(), mode.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (mode.find("territor") != std::string::npos) return ActiveModeDriver::Territory;
    if (mode.find("suprem") != std::string::npos) return ActiveModeDriver::Supremacy;
    if (mode.find("skirm") != std::string::npos) return ActiveModeDriver::Skirmish;
    return ActiveModeDriver::Generic;
}

void GameServer::ActivateRoleAuthorityForMap(const std::string& mapName) {
    if (!m_roleSystem) {
        Logger::Warn(
            "[GameServer] Cannot activate map role authority for '%s': "
            "RoleSystem is unavailable",
            mapName.c_str());
        return;
    }

    // Every activation starts from the canonical Vietnam defaults so an
    // override from the previous map can never leak through a rotation.
    Faction teamOneFaction = Faction::USArmy;
    Faction teamTwoFaction = Faction::NVA;
    if (mapName == "VNSK-Compound") {
        teamOneFaction = Faction::USMC;
        teamTwoFaction = Faction::NLFSV;
    } else if (mapName == "VNTE-CuChi") {
        teamTwoFaction = Faction::NLFSV;
    }

    const uint32_t squadGeneration = m_roleSystem->ResetRetailSquads();
    m_roleSystem->SetTeamFaction(1, teamOneFaction);
    m_roleSystem->SetTeamFaction(2, teamTwoFaction);
    Logger::Info(
        "[GameServer] Activated role authority for map '%s': team 1=%s, "
        "team 2=%s, retail squad generation=%u",
        mapName.c_str(),
        m_roleSystem->GetFactionShortName(teamOneFaction).c_str(),
        m_roleSystem->GetFactionShortName(teamTwoFaction).c_str(),
        squadGeneration);
}

void GameServer::InitializeActiveModeDriver() {
    // Exactly one of these clocks may own round transitions.  Initializing and
    // ticking all three used to let Skirmish end rounds underneath Territory,
    // while the generic GameMode independently rotated the map without client
    // travel replication.
    // Tear down the central pair before constructing a replacement native
    // driver. ReconcileCentralRoundState recreates it later only when the new
    // map resolves to Generic and the config explicitly requests it.
    ResetNativeModeMapChangeLatch();
    m_roundManager.reset();
    m_gameState.reset();
    m_territoryMode.reset();
    m_supremacyMode.reset();
    m_skirmishMode.reset();
    m_activeModeDriver = ResolveActiveModeDriver();
    ActivateRoleAuthorityForMap(
        m_mapManager ? m_mapManager->GetCurrentMapName() : std::string{});

    // Native modes use the legacy 20-second deployment cadence unless their
    // retail rules say otherwise. Reset this on every map transition so a map
    // following Skirmish does not inherit its 25-second cadence.
    if (m_spawnSystem) m_spawnSystem->SetWaveInterval(20.0f);

    switch (m_activeModeDriver) {
        case ActiveModeDriver::Territory:
            m_territoryMode = std::make_unique<TerritoryMode>(this);
            m_territoryMode->Initialize();
            if (m_mapManager && m_mapManager->GetCurrentMapName() == "VNTE-CuChi") {
                // Cooked Cu Chi map settings: 35 minutes, US attackers with
                // 680 reinforcements, NVA defenders with 580.
                m_territoryMode->SetRoundTime(2100.0f);
                m_territoryMode->SetAttackerTickets(680);
                m_territoryMode->SetDefenderTickets(580);
            }
            break;
        case ActiveModeDriver::Supremacy:
            m_supremacyMode = std::make_unique<SupremacyMode>(this);
            m_supremacyMode->Initialize();
            break;
        case ActiveModeDriver::Skirmish:
            m_skirmishMode = std::make_unique<SkirmishMode>(this);
            m_skirmishMode->Initialize();
            if (m_spawnSystem) {
                m_spawnSystem->SetWaveInterval(
                    static_cast<float>(SkirmishMode::kSpawnWaveIntervalSeconds));
            }
            break;
        case ActiveModeDriver::Generic:
            break;
    }
    Logger::Info("[GameServer] Active gameplay driver: %s",
                 GetEffectiveGameModeName().c_str());
}

void GameServer::ResetNativeModeMapChangeLatch() {
    m_nativeModeMapChangeLatched = false;
}

void GameServer::ObserveNativeModeCompletion(bool nativeDriverOwnsRound,
                                             bool nativeModePresent,
                                             bool finished) {
    if (m_nativeModeMapChangeLatched || IsShutdownRequested() ||
        !nativeDriverOwnsRound || !nativeModePresent || !finished) {
        return;
    }

    // Latch before queueing. If the next tick cannot rotate (no MapManager,
    // empty rotation, invalid ClientTravel, or LoadMap failure), this terminal
    // mode still gets only one attempt instead of retrying every frame.
    m_nativeModeMapChangeLatched = true;
    RequestMapChange();
}

void GameServer::RequestNativeModeMapChangeIfFinished() {
    const bool nativeDriverOwnsRound =
        !m_roundManager && m_activeModeDriver != ActiveModeDriver::Generic;
    bool nativeModePresent = false;
    bool finished = false;

    switch (m_activeModeDriver) {
        case ActiveModeDriver::Territory:
            nativeModePresent = static_cast<bool>(m_territoryMode);
            finished = m_territoryMode &&
                m_territoryMode->GetPhase() == TerritoryMode::Phase::Finished;
            break;
        case ActiveModeDriver::Supremacy:
            nativeModePresent = static_cast<bool>(m_supremacyMode);
            finished = m_supremacyMode &&
                m_supremacyMode->GetPhase() == SupremacyMode::Phase::Finished;
            break;
        case ActiveModeDriver::Skirmish:
            nativeModePresent = static_cast<bool>(m_skirmishMode);
            finished = m_skirmishMode &&
                m_skirmishMode->GetPhase() == SkirmishMode::Phase::Finished;
            break;
        case ActiveModeDriver::Generic:
            break;
    }

    ObserveNativeModeCompletion(nativeDriverOwnsRound,
                                nativeModePresent,
                                finished);
}

void GameServer::ReconcileCentralRoundState() {
    // RoundManager keeps a raw pointer to GameState, so destroy it first. This
    // also guarantees that a Generic -> native map change cannot leave the old
    // central clock suppressing the newly-created native driver in Run().
    m_roundManager.reset();
    m_gameState.reset();

    const bool roundManagerRequested = m_configManager &&
        m_configManager->GetBool("Game.use_round_manager", false);
    if (!roundManagerRequested) {
        Logger::Debug("[GameServer] Central round manager disabled; active mode driver is authoritative");
        return;
    }
    if (m_activeModeDriver != ActiveModeDriver::Generic) {
        Logger::Warn("[GameServer] Ignoring Game.use_round_manager for native "
                     "Territory/Supremacy/Skirmish mode; its native driver remains authoritative");
        return;
    }

    Logger::Info("[GameServer] Central round manager ENABLED (Game.use_round_manager)");
    m_gameState = std::make_unique<GameState>(this);
    m_gameState->Initialize();

    m_roundManager = std::make_unique<RoundManager>(this);
    m_roundManager->Initialize();
    m_roundManager->SetOnRoundStart([this]() {
        Logger::Info("[GameServer] Round started (RoundManager)");
    });
    m_roundManager->SetOnRoundEnd([this]() {
        Logger::Info("[GameServer] Round ended (RoundManager)");
    });
}

float GameServer::GetObjectiveCaptureDelta(float deltaSeconds) const {
    if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f) return 0.0f;

    switch (m_activeModeDriver) {
        case ActiveModeDriver::Territory:
            if (!m_territoryMode || !m_territoryMode->CanCaptureObjectives()) return 0.0f;
            // SuddenDeath has no timer boundary; objective control remains live
            // until the last-team-standing condition resolves the round.
            if (m_territoryMode->GetPhase() == TerritoryMode::Phase::SuddenDeath) {
                return deltaSeconds;
            }
            return ObjectiveSystem::ClampCaptureDeltaToPhase(
                deltaSeconds, m_territoryMode->GetRoundTimeRemaining());

        case ActiveModeDriver::Supremacy:
            if (!m_supremacyMode || !m_supremacyMode->CanCaptureObjectives()) return 0.0f;
            return ObjectiveSystem::ClampCaptureDeltaToPhase(
                deltaSeconds, m_supremacyMode->GetPhaseTimeRemaining());

        case ActiveModeDriver::Skirmish:
            if (!m_skirmishMode || !m_skirmishMode->CanCaptureObjectives()) return 0.0f;
            // InstantDeath ends through elimination/objective authority, not
            // through m_phaseTimer expiry, so it has no capture-time boundary.
            if (m_skirmishMode->GetPhase() == SkirmishMode::Phase::InstantDeath) {
                return deltaSeconds;
            }
            return ObjectiveSystem::ClampCaptureDeltaToPhase(
                deltaSeconds, m_skirmishMode->GetPhaseTimeRemaining());

        case ActiveModeDriver::Generic:
            // Generic/RoundManager capture gating remains inside ObjectiveSystem.
            return deltaSeconds;
    }
    return 0.0f;
}

bool GameServer::Initialize() {
    return Initialize("config/server.ini", 0, {}, 0);
}

bool GameServer::Initialize(const std::string& configFile, uint16_t portOverride,
                            const std::string& mapOverride,
                            uint16_t eacPortOverride) {
    Logger::Trace("[GameServer::Initialize] Entry");
    Logger::Info("GameServer initialization starting...");

    // Load configurations
    Logger::Debug("[GameServer::Initialize] Creating ConfigManager");
    m_configManager = std::make_shared<ConfigManager>();
    if (!m_configManager->Initialize(configFile)) {
        // Never bind with default security/network values after an explicit
        // primary config is missing or malformed. Continuing would launch a
        // different server than the operator requested.
        Logger::Error(
            "[GameServer::Initialize] Failed to initialize primary config '%s'",
            configFile.c_str());
        return false;
    }
    Logger::Debug("[GameServer::Initialize] ConfigManager initialized successfully from '%s'",
                  configFile.c_str());

    // CLI overrides have the highest precedence and must be installed before
    // ServerConfig is wrapped and NetworkManager binds the game socket.
    if (portOverride != 0) {
        Logger::Info("[GameServer::Initialize] Applying command-line port override: %u",
                     portOverride);
        m_configManager->SetInt("Network.port", static_cast<int>(portOverride));
    }
    if (eacPortOverride != 0) {
        Logger::Info("[GameServer::Initialize] Applying command-line EAC port override: %u",
                     eacPortOverride);
        m_configManager->SetInt("EAC.listen_port",
                                static_cast<int>(eacPortOverride));
    }

    // Create config wrappers — ServerConfig wraps ConfigManager directly,
    // the rest wrap ServerConfig
    Logger::Debug("[GameServer::Initialize] Creating config wrappers");
    m_serverConfig   = std::make_shared<ServerConfig>(m_configManager);
    m_networkConfig  = std::make_shared<NetworkConfig>(*m_serverConfig);
    m_securityConfig = std::make_shared<SecurityConfig>(*m_serverConfig);
    m_gameConfig     = std::make_shared<GameConfig>(*m_serverConfig);
    m_mapConfig      = std::make_shared<MapConfig>(*m_serverConfig);
    // Load the map definitions from disk (config/maps.ini). Without this the
    // MapConfig is empty and every LoadMap() fails with "definition not found",
    // leaving the server mapless. Non-fatal: a failure falls back to whatever
    // CreateDefaultConfig/empty set yields, and the server continues.
    if (!m_mapConfig->Initialize()) {
        Logger::Warn("[GameServer::Initialize] MapConfig::Initialize failed; map definitions may be unavailable");
    }

    // Initialize network manager
    m_networkManager = std::make_unique<NetworkManager>(this);
    uint16_t listenPort = (uint16_t)m_serverConfig->GetPort();
    Logger::Debug("[GameServer::Initialize] Initializing NetworkManager on port %u", listenPort);
    if (!m_networkManager->Initialize(listenPort)) {
        Logger::Error("[GameServer::Initialize] NetworkManager init failed on port %u", listenPort);
        Logger::Trace("[GameServer::Initialize] Exit, returning false");
        return false;
    }
    Logger::Debug("[GameServer::Initialize] NetworkManager initialized successfully");

    // Initialize game managers
    Logger::Debug("[GameServer::Initialize] Creating game managers");
    m_playerManager = std::make_unique<PlayerManager>(this);
    m_teamManager   = std::make_unique<TeamManager>(this);
    m_mapManager    = std::make_unique<MapManager>(this, m_mapConfig);

    // Map voting
    m_mapVoteManager = std::make_unique<MapVoteManager>(m_mapConfig);
    m_mapVoteManager->SetEnabled(m_serverConfig->IsMapVoteEnabled());
    m_mapVoteManager->SetOptionCount(m_serverConfig->GetMapVoteOptions());
    m_mapVoteManager->SetVoteDurationSeconds(m_serverConfig->GetMapVoteDuration());

    // Steam Workshop items (custom maps / mods / assets)
    m_workshopManager = std::make_unique<WorkshopManager>(m_serverConfig);
    m_workshopManager->Initialize();
    m_workshopManager->DownloadMissing();   // dry-run unless Workshop.download_enabled
    m_workshopManager->LogSummary();

    // Mods + cosmetic assets (sourced from Workshop manifest + local mods dir)
    m_modManager = std::make_unique<ModManager>(m_serverConfig);
    m_modManager->Initialize(m_workshopManager.get());
    m_modManager->LogSummary();

    m_adminManager  = std::make_unique<AdminManager>(this, m_serverConfig);
    m_adminManager->Initialize();

    m_chatManager   = std::make_unique<ChatManager>(this);
    m_chatManager->Initialize();

    // Unified command system — single source of truth for admin/dev/mod/player/
    // console/config/automation commands. All transports (chat, console, SOAP)
    // dispatch through this one registry.
    m_commandManager = std::make_unique<CommandManager>(this);
    m_commandManager->Initialize();

    // Initialize RS2V game systems
    Logger::Debug("[GameServer::Initialize] Initializing RS2V game systems");
    m_weaponDatabase = std::make_unique<WeaponDatabase>();
    m_weaponDatabase->Initialize();

    m_roleSystem = std::make_unique<RoleSystem>(this);
    m_roleSystem->Initialize();

    m_ticketSystem = std::make_unique<TicketSystem>(this);
    m_ticketSystem->Initialize(300, 300);
    m_ticketSystem->SetOnTicketsDepleted([this](uint32_t teamId) {
        Logger::Info("Team %u tickets depleted", teamId);
        switch (m_activeModeDriver) {
            case ActiveModeDriver::Territory:
                if (m_territoryMode) m_territoryMode->OnTicketsDepleted(teamId);
                break;
            case ActiveModeDriver::Supremacy:
                if (m_supremacyMode) m_supremacyMode->OnTicketsDepleted(teamId);
                break;
            case ActiveModeDriver::Skirmish:
                if (m_skirmishMode) m_skirmishMode->OnTicketsDepleted(teamId);
                break;
            case ActiveModeDriver::Generic:
                break;
        }
    });

    m_objectiveSystem = std::make_unique<ObjectiveSystem>(this);
    m_objectiveSystem->Initialize();
    m_objectiveSystem->SetOnObjectiveCaptured([this](uint32_t objId, uint32_t capTeam, uint32_t prevTeam) {
        Logger::Info("Objective %u captured by team %u (was team %u)", objId, capTeam, prevTeam);
        switch (m_activeModeDriver) {
            case ActiveModeDriver::Territory:
                if (m_territoryMode) m_territoryMode->OnObjectiveCaptured(objId, capTeam);
                break;
            case ActiveModeDriver::Supremacy:
                if (m_supremacyMode) m_supremacyMode->OnObjectiveCaptured(objId, capTeam);
                break;
            case ActiveModeDriver::Skirmish:
                if (m_skirmishMode) m_skirmishMode->OnObjectiveCaptured(objId, capTeam);
                break;
            case ActiveModeDriver::Generic:
                break;
        }
        // Central round/state layer (opt-in): mirror ownership into GameState
        // and let RoundManager evaluate early round-end win conditions.
        if (m_gameState)    m_gameState->CaptureObjective(objId, capTeam);
        if (m_roundManager) m_roundManager->OnObjectiveCaptured(objId, capTeam);
    });

    m_commanderAbilities = std::make_unique<CommanderAbilities>(this);
    m_commanderAbilities->Initialize();

    m_spawnSystem = std::make_unique<SpawnSystem>(this);
    m_spawnSystem->Initialize();

    m_damageSystem = std::make_unique<DamageSystem>(this);
    m_damageSystem->Initialize();
    m_damageSystem->SetFriendlyFireEnabled(m_gameConfig->IsFriendlyFire());

    CombatAuthority::AuthorityConfig combatConfig;
    combatConfig.friendlyFireEnabled = m_gameConfig->IsFriendlyFire();
    m_retailGrenadeCooks.clear();
    m_retailM61Projectiles.clear();
    m_retailM61ReplicationAccumulator = 0.0f;
    m_combatAuthority =
        std::make_unique<CombatAuthority::Authority>(combatConfig);
    if (!m_combatAuthority->IsConfigurationValid()) {
        Logger::Error("[GameServer::Initialize] CombatAuthority configuration is invalid");
        m_combatAuthority.reset();
    } else {
        Logger::Info("[GameServer::Initialize] CombatAuthority ready "
                     "(50 UU/m, h56 hitscan, friendly-fire=%s)",
                     combatConfig.friendlyFireEnabled ? "enabled" : "disabled");
    }

    m_mantleAuthority = std::make_unique<MantleAuthority::Authority>();
    if (!m_mantleAuthority->IsConfigurationValid()) {
        Logger::Error(
            "[GameServer::Initialize] MantleAuthority configuration is invalid");
        m_mantleAuthority.reset();
    } else {
        Logger::Info(
            "[GameServer::Initialize] MantleAuthority ready "
            "(h280 admission, h344 response, server cooldown)");
    }

    // Gameplay mutators — created after DamageSystem since several mutators
    // adjust damage/friendly-fire settings in their OnInit.
    m_mutatorManager = std::make_unique<MutatorManager>(this);
    if (m_serverConfig->IsMutatorsEnabled()) {
        m_mutatorManager->LoadFromConfig(m_serverConfig->GetEnabledMutators());
    } else {
        Logger::Debug("[GameServer::Initialize] Mutators disabled by config");
    }
    m_mutatorManager->LogSummary();

    m_projectileManager = std::make_unique<ProjectileManager>(this);
    m_projectileManager->Initialize();
    m_damageSystem->SetOnKill([this](const KillEvent& kill) {
        if (m_ticketSystem && !kill.isTeamKill) {
            // GUARD: TeamManager can be null/destroyed during shutdown; the kill
            // callback may still fire from DamageSystem. Skip ticket accounting
            // rather than dereference a null manager.
            if (auto* tm = GetTeamManager()) {
                uint32_t victimTeam = tm->GetPlayerTeam(kill.victimId);
                m_ticketSystem->OnPlayerKilled(victimTeam);
            } else {
                Logger::Warn("[GameServer] Kill callback: TeamManager unavailable; skipping ticket update for victim %u", kill.victimId);
            }
        }
        switch (m_activeModeDriver) {
            case ActiveModeDriver::Territory:
                if (m_territoryMode) m_territoryMode->OnPlayerKilled(kill.killerId, kill.victimId);
                break;
            case ActiveModeDriver::Supremacy:
                if (m_supremacyMode) m_supremacyMode->OnPlayerKilled(kill.killerId, kill.victimId);
                break;
            case ActiveModeDriver::Skirmish:
                if (m_skirmishMode) m_skirmishMode->OnPlayerKilled(kill.killerId, kill.victimId);
                break;
            case ActiveModeDriver::Generic:
                break;
        }
        if (m_mutatorManager) m_mutatorManager->DispatchPlayerKilled(kill.killerId, kill.victimId);
    });

    m_helicopterPhysics = std::make_unique<HelicopterPhysics>(this);
    m_helicopterPhysics->Initialize();

    Logger::Info("RS2V game systems initialized (mode driver deferred until map load)");

    // ------------------------------------------------------------------
    // Security + replication + the connection->player login bridge.
    //
    // This is the wiring that turns a connected client into a spawned player:
    // SecurityManager runs the PreLogin ban gate, ReplicationManager carries the
    // GRI/PRI, and ConnectionLoginBridge mirrors the UE3 Login/PostLogin flow
    // onto the control-channel ClientLoggedIn / ClientJoined events.
    // ------------------------------------------------------------------
    Logger::Debug("[GameServer::Initialize] Creating outbound ReplicationManager");
    m_replicationManager = std::make_unique<ReplicationManager>(
        [this](const Packet& packet) {
            if (!m_networkManager) return false;
            m_networkManager->BroadcastPacket(packet);
            return true;
        });

    Logger::Debug("[GameServer::Initialize] Creating ConnectionLoginBridge and subscribing handshake callbacks");
    {
        // The bridge constructs and owns the SecurityManager itself (from
        // m_securityConfig). This keeps the Security headers out of this TU,
        // avoiding the duplicate ClientAddress definition shared between
        // Security/NetworkBlocker.h and Network/BandwidthManager.h.
        ConnectionLoginBridge::Dependencies deps;
        deps.playerManager      = m_playerManager.get();
        deps.teamManager        = m_teamManager.get();
        deps.spawnSystem        = m_spawnSystem.get();
        deps.securityConfig     = m_securityConfig;
        deps.replicationManager = m_replicationManager.get();
        deps.serverConfig       = m_serverConfig;
        deps.resolveConnection  = [this](uint32_t clientId) {
            return GetClientConnection(clientId);
        };
        deps.dropConnection     = [this](uint32_t clientId, const std::string& reason) {
            Logger::Info("[GameServer] Dropping client %u after failed PreLogin: %s", clientId, reason.c_str());
            if (auto conn = GetClientConnection(clientId)) {
                conn->MarkDisconnected();
            }
        };
        m_loginBridge = std::make_unique<ConnectionLoginBridge>(std::move(deps));
    }

    // Subscribe GameServer to the two control-channel handshake events. This is
    // the GameServer subscription point that routes Network -> Game.
    if (m_networkManager) {
        m_networkManager->SetClientLoggedInCallback(
            [this](const ClientLoggedInEvent& ev) {
                if (m_loginBridge) m_loginBridge->OnClientLoggedIn(ev);
            });
        m_networkManager->SetClientJoinedCallback(
            [this](const ClientJoinedEvent& ev) {
                if (m_loginBridge) m_loginBridge->OnClientJoined(ev);
            });
        Logger::Info("[GameServer::Initialize] Subscribed to ClientLoggedIn/ClientJoined handshake callbacks");
    }

    // Start first map and game mode
    const std::string configuredMap = m_gameConfig->GetGameSettings().mapName;
    const std::string mapName = mapOverride.empty() ? configuredMap : mapOverride;
    bool initialMapLoaded = false;
    Logger::Info(
        "[GameServer::Initialize] Attempting to load initial map: '%s'%s",
        mapName.c_str(), mapOverride.empty() ? "" : " (command-line override)");
    if (!mapName.empty() && !m_mapManager->LoadMap(mapName)) {
        Logger::Warn("[GameServer::Initialize] Failed to load map: %s — continuing without map", mapName.c_str());
    } else if (!mapName.empty()) {
        initialMapLoaded = true;
        Logger::Debug("[GameServer::Initialize] Map '%s' loaded successfully", mapName.c_str());
    }

    // Resolve the map/config mode before registering objectives: Territory
    // ordering depends on the selected driver, and only that driver may tick.
    InitializeActiveModeDriver();
    if (initialMapLoaded) {
        // Register the loaded map's objectives with the (already-ticking)
        // ObjectiveSystem so capture zones become live.
        PopulateObjectivesFromMap();
        // Feed the map's spawn points into the SpawnSystem. Without this the
        // SpawnSystem stays empty and SpawnPlayerAtDefault fails with "No
        // available spawn locations", so a joined player never gets a pawn.
        PopulateSpawnsFromMap();
        InitializeBotsForCurrentMap();
    }

    // GameState must observe the freshly registered map objectives. This same
    // helper runs after every successful ChangeMap so ownership cannot go stale.
    ReconcileCentralRoundState();

    // Generic GameMode is a fallback driver, not an additional clock layered on
    // top of a native RS2 Territory/Supremacy/Skirmish match.
    if (m_activeModeDriver == ActiveModeDriver::Generic) {
    const auto& gmDef = m_gameConfig->GetGameModeDefinition(GetEffectiveGameModeName());
    if (gmDef) {
        Logger::Debug("[GameServer::Initialize] Creating GameMode from definition");
        m_gameMode = std::make_unique<GameMode>(this, *gmDef);
        m_gameMode->OnStart();
    } else {
        Logger::Warn("[GameServer::Initialize] No valid game mode definition found — continuing without game mode");
    }

    }

    // Initialize protocol reverse-engineering decoder
    {
        ProtocolDecoderConfig decoderCfg;
        decoderCfg.enabled = m_configManager->GetBool("ReverseEngineering.enabled", true);
        // SAFE-BY-DEFAULT: retaining raw attacker payloads and writing JSON on a
        // shipped server is a memory/disk-fill + info-leak risk, so these default
        // OFF; an operator opts in for an active RE session.
        decoderCfg.logRawPackets = m_configManager->GetBool("ReverseEngineering.log_raw_packets", false);
        decoderCfg.exportJsonDefinitions = m_configManager->GetBool("ReverseEngineering.export_json", false);
        decoderCfg.detectUE3Bunches = m_configManager->GetBool("ReverseEngineering.detect_ue3_bunches", true);
        decoderCfg.decodeBunchProperties = m_configManager->GetBool("ReverseEngineering.decode_bunch_properties", true);
        decoderCfg.asyncAnalysis = m_configManager->GetBool("ReverseEngineering.async_analysis", true);
        decoderCfg.persistState = m_configManager->GetBool("ReverseEngineering.persist_state", true);
        decoderCfg.outputDirectory = m_configManager->GetString("ReverseEngineering.output_dir", "protocol_analysis");
        decoderCfg.netfieldsDir = m_configManager->GetString("ReverseEngineering.netfields_dir", "data/re/netfields");
        decoderCfg.maxChannels = static_cast<uint32_t>(m_configManager->GetInt("ReverseEngineering.max_channels", 1024));
        decoderCfg.exportIntervalSeconds = m_configManager->GetInt("ReverseEngineering.export_interval", 300);
        GetProtocolDecoder().Initialize(decoderCfg);

        GetProtocolDecoder().SetDiscoveryCallback([](const std::string& tag, const DecodedPacketStructure& s) {
            Logger::Info("[RE] New packet type discovered: '%s' (payload=%zu bytes)",
                         tag.c_str(), s.avgPayloadSize > 0 ? (size_t)s.avgPayloadSize : 0);
        });
    }

    // Periodic handler regeneration is OFF by default. Generated packet
    // handlers are compiled statically into the server at build time, so no
    // runtime regeneration is needed for normal operation. The auto-regen
    // thread only makes sense for developers who deploy the PacketHandlerCodeGen
    // tool alongside the server and want live regeneration; it is opt-in via
    // config and StartAutoRegen() itself guards against a missing tool.
    bool autoRegenEnabled = false;
    int  autoRegenInterval = 3600;
    if (m_configManager) {
        autoRegenEnabled  = m_configManager->GetBool("Handlers.auto_regen", false);
        autoRegenInterval = m_configManager->GetInt("Handlers.auto_regen_interval", 3600);
    }
    if (autoRegenEnabled) {
        Logger::Info("[GameServer::Initialize] Handler auto-regeneration ENABLED via config (interval=%ds)", autoRegenInterval);
        StartAutoRegen(autoRegenInterval);
    } else {
        Logger::Debug("[GameServer::Initialize] Handler auto-regeneration disabled (default). "
                      "Using statically-compiled generated handlers.");
    }

    // Resolve generated handlers for runtime use. Prefers the dynamic library
    // when present, otherwise falls back to the statically-compiled registry.
    // Fully guarded — never crashes when neither is available.
    DynamicReloadGeneratedHandlers();

    // Record the configured tick rate so `status`/`tickrate` and deterministic
    // direct Run() calls use the same nominal interval even before main()
    // installs the GameClock hook.
    if (m_serverConfig) {
        const int configuredTickRate = m_serverConfig->GetTickRate();
        if (configuredTickRate >= 1 && configuredTickRate <= 256) {
            m_currentTickRate = configuredTickRate;
            m_tickDeltaSeconds = 1.0f / static_cast<float>(configuredTickRate);
        } else {
            Logger::Warn("[GameServer::Initialize] Invalid configured tick rate %d; using 60 Hz",
                         configuredTickRate);
            m_currentTickRate = 60;
            m_tickDeltaSeconds = 1.0f / 60.0f;
        }
    }

    // Local console (stdin) command transport. Enabled by default; a headless
    // launch with no TTY simply sees EOF and the reader thread exits cleanly.
    {
        bool consoleEnabled = m_configManager ? m_configManager->GetBool("Console.enabled", true) : true;
        if (consoleEnabled) {
            m_consoleInput = std::make_unique<ConsoleInput>(this);
            m_consoleInput->Start();
        } else {
            Logger::Info("[GameServer::Initialize] Console command input disabled by config");
        }
    }

    // Remote SOAP command transport (for tooling / AI automation). Off unless a
    // port AND password are configured — never expose remote control by default.
    {
        RemoteAdminConfig rc;
        rc.port     = m_configManager ? static_cast<uint16_t>(m_configManager->GetInt("RemoteAdmin.soap_port", 0)) : 0;
        rc.password = m_configManager ? m_configManager->GetString("RemoteAdmin.password", "") : "";
        rc.defaultLevel = m_configManager ? m_configManager->GetInt("RemoteAdmin.level", static_cast<int>(CommandLevel::Admin)) : static_cast<int>(CommandLevel::Admin);
        if (rc.port != 0 && !rc.password.empty()) {
            m_remoteAdminServer = std::make_unique<RemoteAdminServer>(this, rc);
            if (!m_remoteAdminServer->Start()) {
                Logger::Warn("[GameServer::Initialize] Remote SOAP admin server failed to start on port %u", rc.port);
                m_remoteAdminServer.reset();
            }
        } else {
            Logger::Info("[GameServer::Initialize] Remote SOAP admin server disabled "
                         "(set RemoteAdmin.soap_port and RemoteAdmin.password to enable)");
        }
    }

    m_running = true;
    Logger::Info("GameServer initialized successfully");
    Logger::Trace("[GameServer::Initialize] Exit, returning true");
    return true;
}

void GameServer::Run(float elapsedSeconds) {
    Logger::Trace("[GameServer::Run] Entry");
    if (!m_running) {
        Logger::Debug("[GameServer::Run] Server not running, skipping tick");
        Logger::Trace("[GameServer::Run] Exit (not running)");
        return;
    }

    // Console and SOAP transports are worker-thread I/O only. Execute their
    // mutating handlers here, before any mutable subsystem advances, so every
    // command shares the same authoritative thread as in-game chat commands.
    if (m_commandManager) {
        for (size_t i = 0; i < CommandManager::MAX_COMMANDS_PER_TICK; ++i) {
            if (m_commandManager->ProcessQueued(1) == 0) break;
            // Preserve FIFO shutdown semantics: once `shutdown` executes, reject
            // later queued mutations instead of running the rest of this batch.
            if (m_shutdownRequested.load()) {
                m_commandManager->StopAccepting("server shutting down");
                Logger::Trace(
                    "[GameServer::Run] Exit (queued shutdown requested)");
                return;
            }
        }
    }

    // Start frame timing
    auto frameStart = std::chrono::high_resolution_clock::now();

    // Poll network and process received packets (with timing)
    {
        auto netStart = std::chrono::high_resolution_clock::now();
        if (m_networkManager) {
            m_networkManager->PollNetwork();
        }
        auto netEnd = std::chrono::high_resolution_clock::now();
        double netMs = std::chrono::duration<double, std::milli>(netEnd - netStart).count();
        Telemetry::TelemetryManager::Instance().GetCustomMetrics().UpdateFrameTiming(0, 0, netMs, 0);
    }

    // Process queued packets
    auto packets = FetchPendingPackets();
    Logger::Trace("[GameServer::Run] Processing %zu queued packets", packets.size());
    for (auto& qpkt : packets) {
        auto conn = GetClientConnection(qpkt.clientId);
        if (!conn) {
            Logger::Debug("[GameServer::Run] No connection for clientId=%u, skipping packet", qpkt.clientId);
            continue;
        }

        // Per-packet guard: packet payloads are attacker-controlled. A handler
        // that throws on a crafted packet is logged non-fatally and we advance to
        // the next packet, rather than letting one bad packet abort the whole tick.
        rs2v::Guard("packet dispatch", [&] {
        std::string tag = qpkt.packet.GetTag();
        Logger::Debug("[GameServer::Run] Processing packet tag='%s' from clientId=%u", tag.c_str(), qpkt.clientId);

        // AUTH GATE (defense-in-depth): every tag below CHAT drives state-changing gameplay
        // (role/spawn/commander/squad/vehicle/weapon, plus the GameMode action fallthrough)
        // and MUST require a completed handshake/login. Without this, any client that never
        // logged in could drive gameplay pre-auth - this legacy string-tag path is reachable
        // by any non-UE3 datagram. Real UE3 clients never use this path (their gameplay rides
        // the UE3 control channel via DecodeInboundActorBunch), so the gate does not affect
        // them. CHAT is allowed (already length-capped) so lobby chat is unimpeded.
        if (tag != "CHAT_MESSAGE" && !conn->IsHandshakeComplete()) {
            Logger::Warn("[GameServer::Run] Rejecting pre-auth gameplay packet tag='%s' from clientId=%u "
                         "(handshake not complete)", tag.c_str(), qpkt.clientId);
            return;  // inside rs2v::Guard lambda: return skips this packet (was 'continue' in the raw loop)
        }

        if (tag == "CHAT_MESSAGE" && m_chatManager) {
            Packet pktCopy = qpkt.packet;
            std::string chatText = pktCopy.ReadString();
            // GUARD: chat text is attacker-controlled. Cap its length before it
            // propagates into chat history / broadcast buffers. Valid chat is
            // short; this only rejects abusive/oversized payloads. Non-fatal.
            constexpr size_t kMaxChatLen = 1024;
            if (chatText.size() > kMaxChatLen) {
                Logger::Warn("[GameServer::Run] Oversized chat from clientId=%u (%zu bytes); truncating to %zu",
                             qpkt.clientId, chatText.size(), kMaxChatLen);
                chatText.resize(kMaxChatLen);
            }
            m_chatManager->ProcessChatCommand(qpkt.clientId, chatText);
        } else if (tag == "ROLE_SELECT") {
            HandleRoleSelection(qpkt.clientId, qpkt.packet.RawData());
        } else if (tag == "SPAWN_REQUEST") {
            HandleSpawnRequest(qpkt.clientId, qpkt.packet.RawData());
        } else if (tag == "COMMANDER_ABILITY") {
            HandleCommanderAbility(qpkt.clientId, qpkt.packet.RawData());
        } else if (tag == "SQUAD_ACTION") {
            HandleSquadAction(qpkt.clientId, qpkt.packet.RawData());
        } else if (tag == "VEHICLE_ACTION") {
            HandleVehicleAction(qpkt.clientId, qpkt.packet.RawData());
        } else if (tag == "WEAPON_FIRE") {
            HandleWeaponFire(qpkt.clientId, qpkt.packet.RawData());
        } else if (m_gameMode) {
            Logger::Debug("[GameServer::Run] Forwarding unhandled tag '%s' to GameMode", tag.c_str());
            m_gameMode->HandlePlayerAction(qpkt.clientId, tag, qpkt.packet.RawData());
        } else {
            Logger::Debug("[GameServer::Run] Unhandled packet tag '%s' and no GameMode available", tag.c_str());
        }
        });  // rs2v::Guard (per-packet dispatch)
    }

    // GameClock measures elapsed monotonic time between completed loop starts.
    // Using the configured 1/tick-rate here used to discard time whenever a
    // loaded tick missed its cadence, so phase/capture timers ran slow. Keep a
    // bounded wall delta to account for ordinary load without applying a giant
    // debugger/OS-pause step to the simulation.
    const float wallDeltaSeconds =
        FrameTiming::ResolveTickDeltaSeconds(elapsedSeconds, m_tickDeltaSeconds);
    const float dt = wallDeltaSeconds * m_timeScale;

    // Tick core subsystems with game logic timing
    {
        auto gameStart = std::chrono::high_resolution_clock::now();

        if (m_gameMode) m_gameMode->Update();
        ConsumeDeferredMapChange();
        if (m_playerManager) m_playerManager->Update();
        if (m_replicationManager) m_replicationManager->Tick(dt);

        if (m_combatAuthority) {
            SynchronizeCombatParticipants();
            AdvanceRetailGrenadeCooks();
            float remaining = std::max(0.0f, dt);
            size_t steps = 0;
            do {
                const float step = std::min(
                    remaining,
                    m_combatAuthority->GetConfig().maxAdvanceSeconds);
                const CombatAuthority::AdvanceResult advanced =
                    m_combatAuthority->Advance(step);
                if (!advanced.advanced) {
                    Logger::Warn(
                        "[CombatAuthority] rejected server tick %.4fs (reason %u)",
                        step, static_cast<unsigned>(advanced.reason));
                    break;
                }
                ProcessCombatEvents(advanced.events);
                AdvanceRetailGrenadeCooks();
                remaining -= step;
                ++steps;
            } while (remaining > 1.0e-6f && steps < 64);
            if (remaining > 1.0e-6f) {
                Logger::Warn(
                    "[CombatAuthority] discarded excessive tick remainder %.4fs",
                    remaining);
            }
        }
        ReplicateRetailM61Projectiles(dt);

        // Tick RS2V game systems
        if (m_ticketSystem) m_ticketSystem->Update(dt);
        RefreshBotWorldState();
        // Human fill changes can remove bots during preparation, before the
        // live-simulation branch below runs. Drain removals every frame so a
        // retired bot can never leave a stale viewer-local PRI/pawn actor.
        if (m_botManager) {
            const std::vector<BotRemovalEvent> removals =
                m_botManager->ConsumeRemovalEvents();
            if (m_networkManager) {
                for (const BotRemovalEvent& removal : removals) {
                    m_networkManager->RemoveRetailParticipant(removal.botId);
                }
            }
        }
        if (m_botManager && IsBotGameplayLive()) {
            m_botManager->Update(dt);

            // Collapse every bot combat/death/respawn transition produced by
            // this fixed-step update to one final replication snapshot per bot.
            // The BotManager event queues were previously never consumed and
            // grew for the lifetime of the server.
            std::map<ParticipantId, bool> changedBots;
            for (const BotHumanCombatEvent& event :
                 m_botManager->ConsumeHumanCombatEvents()) {
                if (ProcessBotHumanCombatEvent(event)) {
                    changedBots[event.attackerId] = true;
                }
            }
            for (const BotCombatEvent& event :
                 m_botManager->ConsumeCombatEvents()) {
                changedBots[event.victimId] = true;
            }
            for (const BotDeathEvent& event :
                 m_botManager->ConsumeDeathEvents()) {
                changedBots[event.victimId] = true;
            }
            for (const BotRespawnEvent& event :
                 m_botManager->ConsumeRespawnEvents()) {
                changedBots[event.botId] = true;
            }
            if (m_networkManager) {
                for (const auto& changed : changedBots) {
                    const BotSnapshot* bot =
                        m_botManager->FindBot(changed.first);
                    if (!bot) {
                        m_networkManager->RemoveRetailParticipant(changed.first);
                        continue;
                    }
                    m_networkManager->ReplicateRetailParticipantCombatState(
                        bot->id,
                        std::clamp(static_cast<int>(std::lround(bot->health)),
                                   0, 100),
                        static_cast<int>(std::min<std::uint32_t>(
                            bot->kills,
                            static_cast<std::uint32_t>(INT_MAX))),
                        static_cast<int>(std::min<std::uint32_t>(
                            bot->deathSequence,
                            static_cast<std::uint32_t>(INT_MAX))),
                        static_cast<int>(std::min<std::uint32_t>(
                            bot->score,
                            static_cast<std::uint32_t>(INT_MAX))),
                        bot->lifecycle != BotLifecycle::Alive,
                        /*sendHealth=*/true,
                        /*sendDeathRpc=*/false);
                }
            }
        }
        if (m_objectiveSystem) {
            // ObjectiveSystem runs before the native mode update so Territory
            // can use freshly-refreshed occupancy when deciding overtime. Only
            // feed it the part of this frame that belongs to the current
            // capturable phase; the native driver does not carry timer
            // overshoot into its next phase either.
            m_objectiveSystem->Update(dt, GetObjectiveCaptureDelta(dt));
        }
        if (m_commanderAbilities) m_commanderAbilities->Update(dt);
        if (m_spawnSystem) m_spawnSystem->Update(dt);
        if (m_damageSystem) m_damageSystem->Update(dt);
        if (m_projectileManager) m_projectileManager->Update(dt);

        auto gameEnd = std::chrono::high_resolution_clock::now();
        double gameMs = std::chrono::duration<double, std::milli>(gameEnd - gameStart).count();

        // Physics timing (helicopter physics is the main physics update)
        auto physStart = std::chrono::high_resolution_clock::now();
        if (m_helicopterPhysics) m_helicopterPhysics->Update(dt);
        auto physEnd = std::chrono::high_resolution_clock::now();
        double physMs = std::chrono::duration<double, std::milli>(physEnd - physStart).count();

        // Tick exactly one native mode driver.  When the opt-in central round
        // manager is enabled it supersedes these per-mode phase clocks.
        const bool mayAdvanceRoundClock =
            !m_networkManager ||
            m_networkManager->ShouldAdvanceRetailRoundClock();
        if (!m_roundManager && mayAdvanceRoundClock) {
            switch (m_activeModeDriver) {
                case ActiveModeDriver::Territory:
                    if (m_territoryMode) m_territoryMode->Update(dt);
                    break;
                case ActiveModeDriver::Supremacy:
                    if (m_supremacyMode) m_supremacyMode->Update(dt);
                    break;
                case ActiveModeDriver::Skirmish:
                    if (m_skirmishMode) m_skirmishMode->Update(dt);
                    break;
                case ActiveModeDriver::Generic:
                    break;
            }
        }

        // Native mode Update has fully unwound. A terminal observation queues
        // exactly one request here; the destructive ChangeMap is consumed at
        // the beginning of the next Run tick.
        RequestNativeModeMapChangeIfFinished();

        // Central round cycle (opt-in). RoundManager drives GameState's phases
        // directly, so GameState::Update() is intentionally not ticked here to
        // avoid a second, conflicting phase driver.
        if (m_roundManager && mayAdvanceRoundClock) m_roundManager->Update();

        // The countdown transition must observe the phase after its clock has
        // advanced. It deploys prepared clients at the retail final-eight-
        // second threshold and releases their input when the round becomes live.
        if (m_networkManager) m_networkManager->UpdateRetailDeploymentCountdown();

        // Update performance metrics with timing data
        auto frameEnd = std::chrono::high_resolution_clock::now();
        double frameMs = std::chrono::duration<double, std::milli>(frameEnd - frameStart).count();
        Telemetry::TelemetryManager::Instance().GetCustomMetrics().UpdateFrameTiming(
            frameMs, physMs, 0, gameMs);
    }

    if (m_networkManager) m_networkManager->Flush();
    Logger::Trace("[GameServer::Run] Exit");
}

void GameServer::Shutdown() {
    Logger::Trace("[GameServer::Shutdown] Entry");
    if (!m_running) {
        Logger::Debug("[GameServer::Shutdown] Already shut down, skipping");
        Logger::Trace("[GameServer::Shutdown] Exit (already shut down)");
        return;
    }
    Logger::Info("Shutting down GameServer...");

    m_running = false;
    StopAutoRegen();

    // Wake any command transport waiting for the game thread before joining it.
    // This also closes the enqueue-vs-shutdown race: StopAccepting holds the same
    // queue mutex as Enqueue, so no late request can outlive subsystem teardown.
    if (m_commandManager) {
        m_commandManager->StopAccepting("server shutting down");
    }

    // Stop command transports first so no late command runs against subsystems
    // that are about to be torn down.
    if (m_remoteAdminServer) {
        m_remoteAdminServer->Stop();
        m_remoteAdminServer.reset();
    }
    if (m_consoleInput) {
        m_consoleInput->Stop();
        m_consoleInput.reset();
    }

    // Shutdown login bridge / replication first (they reference the game
    // subsystems below). The bridge owns and shuts down its SecurityManager.
    Logger::Debug("[GameServer::Shutdown] Destroying login bridge and replication");
    m_loginBridge.reset();
    m_replicationManager.reset();

    // Shutdown RS2V systems
    Logger::Debug("[GameServer::Shutdown] Destroying RS2V game systems");
    // Mutators first: OnShutdown may reset DamageSystem state, so do it while
    // DamageSystem is still alive.
    if (m_mutatorManager) {
        m_mutatorManager->Shutdown();
        m_mutatorManager.reset();
    }
    m_roundManager.reset();
    m_gameState.reset();
    m_skirmishMode.reset();
    m_supremacyMode.reset();
    m_territoryMode.reset();
    m_helicopterPhysics.reset();
    if (m_objectiveSystem) {
        m_objectiveSystem->SetBotCaptureWeightProvider({});
    }
    m_botManager.reset();
    m_botObjectiveSignature.clear();
    m_projectileManager.reset();
    m_retailGrenadeCooks.clear();
    m_retailM61Projectiles.clear();
    m_retailM61ReplicationAccumulator = 0.0f;
    m_mantleAuthority.reset();
    m_combatAuthority.reset();
    m_damageSystem.reset();
    m_spawnSystem.reset();
    m_commanderAbilities.reset();
    m_objectiveSystem.reset();
    m_ticketSystem.reset();
    m_roleSystem.reset();
    m_weaponDatabase.reset();

    if (m_gameMode) {
        Logger::Debug("[GameServer::Shutdown] Ending active GameMode");
        m_gameMode->OnEnd();
        m_gameMode.reset();
    } else {
        Logger::Debug("[GameServer::Shutdown] No active GameMode to end");
    }
    if (m_commandManager) {
        m_commandManager->Shutdown();
        m_commandManager.reset();
    }
    m_chatManager.reset();
    if (m_adminManager) {
        Logger::Debug("[GameServer::Shutdown] Shutting down AdminManager");
        m_adminManager->Shutdown();
        m_adminManager.reset();
    }
    m_modManager.reset();
    m_workshopManager.reset();
    m_mapVoteManager.reset();
    m_mapManager.reset();
    m_teamManager.reset();
    m_playerManager.reset();
    if (m_networkManager) {
        Logger::Debug("[GameServer::Shutdown] Shutting down NetworkManager");
        m_networkManager->Shutdown();
        m_networkManager.reset();
    }

    Logger::Info("GameServer shutdown complete");
    Logger::Trace("[GameServer::Shutdown] Exit");
}

void GameServer::BroadcastChatMessage(const std::string& msg) {
    Logger::Trace("[GameServer::BroadcastChatMessage] Entry, msg='%s'", msg.c_str());
    if (m_chatManager) {
        Logger::Debug("[GameServer::BroadcastChatMessage] Broadcasting via ChatManager");
        m_chatManager->BroadcastChat(msg);
    } else {
        Logger::Debug("[GameServer::BroadcastChatMessage] No ChatManager available, message dropped");
    }
    Logger::Trace("[GameServer::BroadcastChatMessage] Exit");
}

uint32_t GameServer::FindClientBySteamID(const std::string& steamId) const {
    Logger::Trace("[GameServer::FindClientBySteamID] Entry, steamId='%s'", steamId.c_str());
    uint32_t result = m_networkManager ? m_networkManager->FindClientBySteamID(steamId) : UINT32_MAX;
    if (result == UINT32_MAX) {
        Logger::Debug("[GameServer::FindClientBySteamID] No client found for steamId='%s'", steamId.c_str());
    } else {
        Logger::Debug("[GameServer::FindClientBySteamID] Found clientId=%u for steamId='%s'", result, steamId.c_str());
    }
    Logger::Trace("[GameServer::FindClientBySteamID] Exit, returning %u", result);
    return result;
}

std::shared_ptr<ClientConnection> GameServer::GetClientConnection(uint32_t clientId) const {
    Logger::Trace("[GameServer::GetClientConnection] Entry, clientId=%u", clientId);
    auto result = m_networkManager ? m_networkManager->GetConnection(clientId) : nullptr;
    if (!result) {
        Logger::Debug("[GameServer::GetClientConnection] No connection found for clientId=%u", clientId);
    }
    Logger::Trace("[GameServer::GetClientConnection] Exit, result=%s", result ? "valid" : "null");
    return result;
}

std::vector<std::shared_ptr<ClientConnection>> GameServer::GetAllConnections() const {
    Logger::Trace("[GameServer::GetAllConnections] Entry");
    auto result = m_networkManager ? m_networkManager->GetAllConnections() : std::vector<std::shared_ptr<ClientConnection>>{};
    Logger::Trace("[GameServer::GetAllConnections] Exit, returning %zu connections", result.size());
    return result;
}

// Subsystem accessors
PlayerManager*      GameServer::GetPlayerManager()      const { return m_playerManager.get();      }
TeamManager*        GameServer::GetTeamManager()        const { return m_teamManager.get();        }
MapManager*         GameServer::GetMapManager()         const { return m_mapManager.get();         }
MapVoteManager*     GameServer::GetMapVoteManager()     const { return m_mapVoteManager.get();     }
WorkshopManager*    GameServer::GetWorkshopManager()    const { return m_workshopManager.get();    }
ModManager*         GameServer::GetModManager()         const { return m_modManager.get();         }
MutatorManager*     GameServer::GetMutatorManager()     const { return m_mutatorManager.get();     }
NetworkManager*     GameServer::GetNetworkManager()     const { return m_networkManager.get();     }
AdminManager*       GameServer::GetAdminManager()       const { return m_adminManager.get();       }
SecurityManager*    GameServer::GetSecurityManager()    const { return m_loginBridge ? m_loginBridge->GetSecurityManager() : nullptr; }
ChatManager*        GameServer::GetChatManager()        const { return m_chatManager.get();        }
CommandManager*     GameServer::GetCommandManager()     const { return m_commandManager.get();     }
RoleSystem*         GameServer::GetRoleSystem()         const { return m_roleSystem.get();         }
TicketSystem*       GameServer::GetTicketSystem()       const { return m_ticketSystem.get();       }
ObjectiveSystem*    GameServer::GetObjectiveSystem()    const { return m_objectiveSystem.get();    }
CommanderAbilities* GameServer::GetCommanderAbilities() const { return m_commanderAbilities.get(); }
SpawnSystem*        GameServer::GetSpawnSystem()        const { return m_spawnSystem.get();        }
WeaponDatabase*     GameServer::GetWeaponDatabase()     const { return m_weaponDatabase.get();     }
DamageSystem*       GameServer::GetDamageSystem()       const { return m_damageSystem.get();       }
CombatAuthority::Authority* GameServer::GetCombatAuthority() const { return m_combatAuthority.get(); }
ProjectileManager*  GameServer::GetProjectileManager()  const { return m_projectileManager.get();  }
HelicopterPhysics*  GameServer::GetHelicopterPhysics()  const { return m_helicopterPhysics.get();  }
BotManager*         GameServer::GetBotManager()         const { return m_botManager.get();         }
TerritoryMode*      GameServer::GetTerritoryMode()      const { return m_territoryMode.get();      }
SupremacyMode*      GameServer::GetSupremacyMode()      const { return m_supremacyMode.get();      }
SkirmishMode*       GameServer::GetSkirmishMode()       const { return m_skirmishMode.get();       }
GameState*          GameServer::GetGameState()          const { return m_gameState.get();          }
RoundManager*       GameServer::GetRoundManager()       const { return m_roundManager.get();       }

std::shared_ptr<GameConfig>    GameServer::GetGameConfig()    const { return m_gameConfig;    }
std::shared_ptr<ServerConfig>  GameServer::GetServerConfig()  const { return m_serverConfig;  }
std::shared_ptr<ConfigManager> GameServer::GetConfigManager() const { return m_configManager; }

// --- Ban administration: forward to the single authoritative store behind the
// login bridge (SecurityManager/BanManager). The bridge keeps the Security
// headers out of this TU (they clash with the Network ClientAddress). ---

bool GameServer::BanSteamId(const std::string& steamId, int durationMinutes, const std::string& reason) {
    return m_loginBridge ? m_loginBridge->BanSteamId(steamId, durationMinutes, reason) : false;
}

bool GameServer::UnbanSteamId(const std::string& steamId) {
    return m_loginBridge ? m_loginBridge->UnbanSteamId(steamId) : false;
}

bool GameServer::IsSteamIdBanned(const std::string& steamId) const {
    return m_loginBridge ? m_loginBridge->IsSteamIdBanned(steamId) : false;
}

std::vector<BanRecord> GameServer::GetActiveBans() const {
    return m_loginBridge ? m_loginBridge->GetActiveBans() : std::vector<BanRecord>{};
}

// --- Runtime controls driven by the command system ---

void GameServer::RequestShutdown() {
    Logger::Info("[GameServer::RequestShutdown] Graceful shutdown requested via command");
    m_shutdownRequested.store(true);
}

void GameServer::SetTickRateHook(std::function<void(int)> hook) {
    m_tickRateHook = std::move(hook);
}

bool GameServer::SetTickRate(int rate) {
    if (rate < 1 || rate > 256) {
        Logger::Warn("[GameServer::SetTickRate] Rejected out-of-range tick rate %d", rate);
        return false;
    }
    m_currentTickRate = rate;
    m_tickDeltaSeconds = 1.0f / static_cast<float>(rate);
    if (m_tickRateHook) m_tickRateHook(rate);
    if (m_configManager) m_configManager->SetInt("Network.tick_rate", rate);
    Logger::Info("[GameServer::SetTickRate] Tick rate set to %d Hz", rate);
    return true;
}

void GameServer::SetTimeScale(float scale) {
    // std::stof accepts spellings such as "nan" and "inf". Comparisons do
    // not clamp NaN, so accepting one here would permanently poison subsystem
    // cooldowns and wave timers even after the scale is corrected.
    if (!std::isfinite(scale)) {
        Logger::Warn("[GameServer::SetTimeScale] Rejected non-finite time scale");
        return;
    }
    if (scale < 0.05f) scale = 0.05f;
    if (scale > 8.0f)  scale = 8.0f;
    m_timeScale = scale;
    Logger::Info("[GameServer::SetTimeScale] Time scale set to %.2f", scale);
}

void GameServer::PopulateObjectivesFromMap() {
    Logger::Trace("[GameServer::PopulateObjectivesFromMap] Entry");
    if (!m_objectiveSystem || !m_mapManager) {
        Logger::Debug("[GameServer::PopulateObjectivesFromMap] ObjectiveSystem or MapManager unavailable; skipping");
        return;
    }

    // Start from a clean slate so map changes don't accumulate stale zones.
    m_objectiveSystem->Clear();

    // Register in territory order so any caller-supplied ids are preserved and
    // the linear ordering (below) lines up with insertion.
    std::vector<CaptureZone> zones = m_mapManager->GetObjectiveZones();
    if (zones.empty()) {
        Logger::Info("[GameServer::PopulateObjectivesFromMap] Map provides no objectives");
        return;
    }
    std::sort(zones.begin(), zones.end(),
              [](const CaptureZone& a, const CaptureZone& b) { return a.territoryOrder < b.territoryOrder; });

    std::vector<uint32_t> orderedIds;
    orderedIds.reserve(zones.size());
    std::array<uint32_t, 16> objectiveIdByClientSlot{};
    for (const auto& z : zones) {
        CaptureZone runtimeZone = z;
        // Only Territories may advance the linear phase chain from
        // ObjectiveSystem::OnObjectiveCaptured. Supremacy and Skirmish keep all
        // authored objectives live and let their own mode drivers handle wins.
        runtimeZone.type = m_activeModeDriver == ActiveModeDriver::Territory
            ? ObjectiveType::Territory
            : ObjectiveType::CapturePoint;
        const uint32_t objectiveId = m_objectiveSystem->AddObjective(runtimeZone);
        orderedIds.push_back(objectiveId);
        if (z.enabled && z.clientSlot < objectiveIdByClientSlot.size()) {
            objectiveIdByClientSlot[z.clientSlot] = objectiveId;
        }
    }

    // Only impose linear territory locking when the active mode is Territory;
    // other modes keep every objective simultaneously capturable.
    if (m_activeModeDriver == ActiveModeDriver::Territory) {
        std::vector<uint32_t> territoryOrderedIds;
        territoryOrderedIds.reserve(orderedIds.size());
        for (size_t i = 0; i < orderedIds.size(); ++i) {
            if (zones[i].enabled) territoryOrderedIds.push_back(orderedIds[i]);
        }
        m_objectiveSystem->SetTerritoryOrder(territoryOrderedIds);
        // Territory objectives begin owned by the defending side. Leaving them
        // neutral makes either team an "attacker" in ObjectiveSystem and lets
        // defenders advance their own objective chain.
        const uint32_t defendingTeam =
            m_territoryMode ? m_territoryMode->GetDefendingTeam() : 2u;
        m_objectiveSystem->ResetTerritoryForRound(defendingTeam);
        Logger::Info("[GameServer::PopulateObjectivesFromMap] Registered %zu enabled objectives "
                     "with Territory ordering (%zu total runtime zones)",
                     territoryOrderedIds.size(), orderedIds.size());
    } else if (m_activeModeDriver == ActiveModeDriver::Supremacy && m_supremacyMode) {
        // Translate the map's stable cooked objective slots into this load's
        // server ids, then configure retail Supremacy connectivity and scoring.
        m_supremacyMode->ClearObjectives();
        std::map<uint32_t, std::vector<uint32_t>> links;
        for (size_t i = 0; i < zones.size(); ++i) {
            const CaptureZone& zone = zones[i];
            if (!zone.enabled) continue;
            const uint32_t objectiveId = orderedIds[i];
            m_supremacyMode->SetObjectiveMetadata(
                objectiveId, zone.controllingTeam, zone.supremacyPointValue);
            if (zone.supremacyHomeTeam == 1 || zone.supremacyHomeTeam == 2) {
                m_supremacyMode->SetTeamHQ(zone.supremacyHomeTeam, objectiveId);
            }

            auto& adjacentIds = links[objectiveId];
            for (uint8_t slot = 0; slot < objectiveIdByClientSlot.size(); ++slot) {
                if ((zone.supremacyAdjacentSlots & (uint16_t{1} << slot)) == 0) continue;
                const uint32_t adjacentId = objectiveIdByClientSlot[slot];
                if (adjacentId != 0) adjacentIds.push_back(adjacentId);
            }
        }
        m_supremacyMode->SetObjectiveLinks(links);
        if (m_mapManager->GetCurrentMapName() == "VNSU-HueCity") {
            m_supremacyMode->SetRoundTime(1500.0f);
            m_supremacyMode->SetScoreTarget(500);
            m_supremacyMode->SetScoringInterval(5.0f);
        }
        Logger::Info("[GameServer::PopulateObjectivesFromMap] Registered %u enabled objectives "
                     "with Supremacy topology (%zu total runtime zones)",
                     m_objectiveSystem->GetObjectiveCount(), orderedIds.size());
    } else {
        Logger::Info("[GameServer::PopulateObjectivesFromMap] Registered %u enabled objectives "
                     "(%zu total runtime zones)",
                     m_objectiveSystem->GetObjectiveCount(), orderedIds.size());
    }
    Logger::Trace("[GameServer::PopulateObjectivesFromMap] Exit");
}

void GameServer::PopulateSpawnsFromMap() {
    Logger::Trace("[GameServer::PopulateSpawnsFromMap] Entry");
    if (!m_spawnSystem || !m_mapManager) {
        Logger::Debug("[GameServer::PopulateSpawnsFromMap] SpawnSystem or MapManager unavailable; skipping");
        return;
    }
    std::vector<SpawnPoint> points = m_mapManager->GetSpawnPoints();
    if (points.empty()) {
        Logger::Warn("[GameServer::PopulateSpawnsFromMap] Map provides no spawn points");
        return;
    }

    // The playable factions are team 1 and team 2. A map spawn point that does
    // not name one of them (teamId 0/255 from the geometry fallback or an
    // unassigned definition) is registered for BOTH teams so each side has
    // somewhere to spawn; a point that DOES name a team is registered as-is.
    auto addFor = [&](const SpawnPoint& sp, uint32_t team) {
        SpawnLocation loc;
        loc.type     = SpawnType::BaseSpawn;
        loc.name     = sp.name.empty() ? ("spawn_" + std::to_string(sp.id)) : sp.name;
        loc.position = sp.position;
        loc.rotation = sp.rotation;
        loc.teamId   = team;
        loc.isActive = sp.enabled;
        loc.minTerritoryPhase = sp.minTerritoryPhase;
        loc.maxTerritoryPhase = sp.maxTerritoryPhase;
        loc.retailSpawnVolumeRef = sp.retailSpawnVolumeRef;
        m_spawnSystem->AddSpawnLocation(loc);
    };

    size_t before1 = m_spawnSystem->GetTeamSpawns(1).size();
    size_t before2 = m_spawnSystem->GetTeamSpawns(2).size();
    for (const auto& sp : points) {
        if (sp.teamId == 1 || sp.teamId == 2) {
            addFor(sp, sp.teamId);
        } else {
            addFor(sp, 1);
            addFor(sp, 2);
        }
    }
    Logger::Info("[GameServer::PopulateSpawnsFromMap] Registered spawns from %zu map points "
                 "(team1: %zu->%zu, team2: %zu->%zu)",
                 points.size(),
                 before1, m_spawnSystem->GetTeamSpawns(1).size(),
                 before2, m_spawnSystem->GetTeamSpawns(2).size());
    Logger::Trace("[GameServer::PopulateSpawnsFromMap] Exit");
}

void GameServer::InitializeBotsForCurrentMap() {
    // A BotManager is a world-local identity namespace. Retire every actor from
    // the previous manager before IDs restart at Bot(1), even when no retail
    // endpoint completed its reconnect yet.
    if (m_botManager && m_networkManager) {
        for (const BotRemovalEvent& removal :
             m_botManager->ConsumeRemovalEvents()) {
            m_networkManager->RemoveRetailParticipant(removal.botId);
        }
        for (const BotSnapshot& bot : m_botManager->GetBots()) {
            m_networkManager->RemoveRetailParticipant(bot.id);
        }
    }
    BotManagerConfig config;
    config.fillTargetPerTeam = 8;
    config.maxBotsPerTeam = 32;
    config.fixedStepSeconds = 0.1f;
    // 600 UU/s is an infantry sprint at the server's 50 UU/m scale. Resort's
    // phase-zero US spawn is an authentic 80k-UU offshore helicopter leg; use
    // BotManager's bounded direct-route transit only for that long leg, then
    // hand back to infantry movement for the final approach. The 40k threshold
    // excludes the 15-18k-UU defending routes, so defenders still contest.
    config.moveSpeed = 600.0f;
    if (m_mapManager &&
        m_mapManager->GetCurrentMapName() == "VNTE-Resort") {
        config.transitMoveSpeed = 3000.0f;
        config.transitRouteDistanceThreshold = 40000.0f;
        config.transitApproachDistance = 1000.0f;
    }
    config.respawnDelaySeconds = 5.0f;
    config.maxHealth = 100.0f;
    config.captureWeight = 1.0f;
    config.objectiveArrivalTolerance = 100.0f;
    config.maxCatchUpSteps = 8;
    config.allowDirectRouteFallback = true;
    // Preserve the former 50 m / 5 damage / 10 Hz bot-vs-bot behavior while
    // routing it through BotManager's typed deterministic rifle authority so
    // the same shot can target connected humans without raw-ID aliasing.
    config.combatRangeUu = 2500.0f;
    config.combatDamage = 5.0f;
    config.combatRoundsPerMinute = 600.0f;
    if (m_botCombatGeneration ==
        std::numeric_limits<std::uint64_t>::max()) {
        Logger::Error(
            "[GameServer] Bot combat generation exhausted; disabling bots "
            "instead of reusing a stale event identity");
        m_botManager.reset();
        m_botObjectiveSignature.clear();
        return;
    }
    config.humanCombatGeneration = ++m_botCombatGeneration;
    config.navigation.enforceBounds = false;
    config.navigation.maxEdgeLength = 200000.0f;
    config.navigation.maxDirectRouteLength = 200000.0f;

    const MapBotNavigationMetadata* activeNavigation = nullptr;
    if (m_mapManager) {
        const MapBotNavigationMetadata& metadata =
            m_mapManager->GetBotNavigationMetadata();
        if (metadata.loaded) {
            const std::string effectiveMode = GetEffectiveGameModeName();
            if (StringUtils::EqualsIgnoreCase(metadata.mode, effectiveMode)) {
                config.navigation = metadata.config;
                config.navigationGraph = metadata.graph;
                config.allowDirectRouteFallback = metadata.allowDirectFallback;
                activeNavigation = &metadata;
            } else {
                Logger::Warn(
                    "[GameServer] ignoring bot navigation metadata for mode '%s' "
                    "while effective mode is '%s'",
                    metadata.mode.c_str(), effectiveMode.c_str());
            }
        }
    }

    m_botManager = std::make_unique<BotManager>(config);
    m_botObjectiveSignature.clear();
    m_botTerritoryRedeployedRound = -1;
    m_botTerritoryDeploymentPhase = -1;
    m_botTerritoryAttackingTeam = 0;
    m_botManager->SetDeathBatchCallback(
        [this](const std::vector<BotDeathEvent>& deaths) {
            ProcessBotDeathBatch(deaths);
        });
    if (m_objectiveSystem) {
        m_objectiveSystem->SetBotCaptureWeightProvider(
            [this](uint32_t objectiveId, uint32_t teamId) {
                return m_botManager
                    ? m_botManager->GetObjectiveCaptureWeight(
                          objectiveId, static_cast<uint8_t>(teamId))
                    : 0.0f;
            });
    }
    RefreshBotWorldState();
    if (activeNavigation) {
        Logger::Info(
            "[GameServer] activated authored bot navigation: %zu nodes / %zu edge rows "
            "(%s, direct fallback %s)",
            activeNavigation->graph.nodes.size(),
            activeNavigation->graph.edges.size(),
            activeNavigation->sourcePath.c_str(),
            activeNavigation->allowDirectFallback ? "enabled" : "disabled");
    }
    Logger::Info("[GameServer] Initialized headless bot fill: %zu team1 / %zu team2",
                 m_botManager->CountBots(1), m_botManager->CountBots(2));
}

void GameServer::ProcessBotDeathBatch(
    const std::vector<BotDeathEvent>& deaths) {
    if (deaths.empty()) return;

    // BotManager publishes a built-in combat volley only after every victim's
    // roster state has committed. Preserve that transaction at the ticket
    // boundary: the first positive-to-zero callback must see every same-volley
    // debit, irrespective of deterministic victim-id publication order.
    if (m_ticketSystem) {
        std::vector<uint32_t> victimTeams;
        victimTeams.reserve(deaths.size());
        for (const BotDeathEvent& death : deaths) {
            if (m_ticketSystem->GetInitialTickets(death.teamId) > 0) {
                victimTeams.push_back(death.teamId);
            }
        }
        m_ticketSystem->OnPlayersKilled(victimTeams);
    }

    // Ticket callbacks establish sudden-death/no-respawn mode state first.
    // Native liveness checks now see both the final ticket pools and the
    // complete BotManager volley. A singleton batch preserves the historical
    // ordering for direct/external bot deaths.
    for (const BotDeathEvent& death : deaths) {
        switch (m_activeModeDriver) {
            case ActiveModeDriver::Territory:
                if (m_territoryMode) {
                    m_territoryMode->OnPlayerKilled(
                        death.killerId.value, death.victimId.value);
                }
                break;
            case ActiveModeDriver::Supremacy:
                if (m_supremacyMode) {
                    m_supremacyMode->OnPlayerKilled(
                        death.killerId.value, death.victimId.value);
                }
                break;
            case ActiveModeDriver::Skirmish:
                if (m_skirmishMode) {
                    m_skirmishMode->OnPlayerKilled(
                        death.killerId.value, death.victimId.value);
                }
                break;
            case ActiveModeDriver::Generic:
                break;
        }
        Logger::Debug(
            "[GameServer] Bot(%u) (team %u) died to %s(%u) [sequence %u]",
            death.victimId.value, static_cast<unsigned>(death.teamId),
            death.killerId.IsHuman() ? "Human" :
                (death.killerId.IsBot() ? "Bot" : "Unknown"),
            death.killerId.value, death.deathSequence);
    }
}

bool GameServer::IsBotGameplayLive() const {
    if (m_territoryMode) {
        switch (m_territoryMode->GetPhase()) {
            case TerritoryMode::Phase::Active:
            case TerritoryMode::Phase::Overtime:
            case TerritoryMode::Phase::Lockdown:
            case TerritoryMode::Phase::SuddenDeath:
                return true;
            default:
                return false;
        }
    }
    if (m_supremacyMode) {
        return m_supremacyMode->GetPhase() == SupremacyMode::Phase::Active ||
               m_supremacyMode->GetPhase() == SupremacyMode::Phase::SuddenDeath;
    }
    if (m_skirmishMode) {
        return m_skirmishMode->GetPhase() == SkirmishMode::Phase::Active ||
               m_skirmishMode->GetPhase() == SkirmishMode::Phase::InstantDeath;
    }
    return m_gameState && m_gameState->GetPhase() == GamePhase::Active;
}

void GameServer::RefreshBotWorldState() {
    if (!m_botManager || !m_spawnSystem || !m_objectiveSystem) return;

    bool respawnTeam1 = false;
    bool respawnTeam2 = false;
    if (m_territoryMode) {
        const auto phase = m_territoryMode->GetPhase();
        respawnTeam1 = respawnTeam2 =
            phase == TerritoryMode::Phase::Active ||
            phase == TerritoryMode::Phase::Overtime ||
            phase == TerritoryMode::Phase::Lockdown;
    } else if (m_supremacyMode) {
        respawnTeam1 = respawnTeam2 =
            m_supremacyMode->GetPhase() == SupremacyMode::Phase::Active;
    } else if (m_skirmishMode) {
        respawnTeam1 = m_skirmishMode->IsSpawnWindowOpen(1);
        respawnTeam2 = m_skirmishMode->IsSpawnWindowOpen(2);
    } else if (m_gameState) {
        respawnTeam1 = respawnTeam2 = m_gameState->GetPhase() == GamePhase::Active;
    }

    // Disable before replacing the spawn list so a queued bot cannot respawn
    // through a phase transition using the previous frame's eligibility.
    m_botManager->SetTeamRespawnAllowed(1, false);
    m_botManager->SetTeamRespawnAllowed(2, false);

    int territoryPhase = -1;
    std::uint8_t territoryAttackingTeam = 0;
    if (m_territoryMode) {
        const uint32_t attackingTeam = m_territoryMode->GetAttackingTeam();
        if (attackingTeam == BotManager::kTeamOne ||
            attackingTeam == BotManager::kTeamTwo) {
            territoryAttackingTeam = static_cast<std::uint8_t>(attackingTeam);
        }
        if (const CaptureZone* current =
                m_objectiveSystem->GetCurrentTerritoryObjective()) {
            territoryPhase = current->territoryOrder;
        }
    }
    if (!m_botManager->SetTerritoryAttackingTeam(territoryAttackingTeam)) {
        Logger::Warn(
            "[GameServer] rejected invalid Territory attacking team %u for bots",
            static_cast<unsigned>(territoryAttackingTeam));
    }

    std::vector<BotSpawnSnapshot> spawns;
    for (uint8_t authoredTeamId : {uint8_t{1}, uint8_t{2}}) {
        for (const SpawnLocation* spawn :
             m_spawnSystem->GetTeamSpawns(authoredTeamId)) {
            if (!spawn) continue;
            // Only cooked Territory base rows encode round-one
            // attacker/defender roles. Runtime-owned spawn types (tunnels,
            // squad leaders, forward bases and helicopters) retain their
            // actual faction ownership across halftime, matching
            // SpawnSystem's retail authorization path.
            std::uint8_t botTeamId = static_cast<std::uint8_t>(spawn->teamId);
            if (m_territoryMode && spawn->type == SpawnType::BaseSpawn) {
                botTeamId = BotManager::RemapTerritorySpawnTeam(
                    authoredTeamId, territoryAttackingTeam);
            }
            if (botTeamId != BotManager::kTeamOne &&
                botTeamId != BotManager::kTeamTwo) {
                continue;
            }
            if (m_territoryMode && spawn->HasTerritoryPhaseBounds() &&
                !spawn->IsAvailableInTerritoryPhase(territoryPhase)) {
                continue;
            }
            spawns.push_back(BotSpawnSnapshot{
                spawn->id, botTeamId, spawn->position, spawn->isActive,
                spawn->isDestroyed, spawn->spawnCooldown});
        }
    }
    m_botManager->SetEligibleSpawns(spawns);

    std::vector<BotObjectiveSnapshot> objectives;
    std::vector<uint32_t> signature;
    for (const CaptureZone* zone : m_objectiveSystem->GetActiveObjectives()) {
        if (!zone || !zone->enabled) continue;
        objectives.push_back(BotObjectiveSnapshot{
            zone->id, zone->position,
            std::max(1.0f, zone->captureRadius) *
                ObjectiveSystem::kUnrealUnitsPerMeter,
            zone->isActive, zone->enabled});
        signature.push_back(zone->id);
    }
    if (signature != m_botObjectiveSignature) {
        m_botManager->SetActiveObjectives(objectives);
        m_botObjectiveSignature = std::move(signature);
    }

    // Territory round boundaries are lifecycle transitions, not combat deaths.
    // Rebuild the eligible spawn/objective views while both team gates are
    // closed, then redeploy exactly once per preparation round. This prevents
    // round-two attackers inheriting defender positions already inside Beach.
    if (m_territoryMode &&
        m_territoryMode->GetPhase() == TerritoryMode::Phase::Preparation) {
        const int currentRound = m_territoryMode->GetCurrentRound();
        if (currentRound != m_botTerritoryRedeployedRound) {
            m_botManager->RedeployForRound();
            m_botTerritoryRedeployedRound = currentRound;
            m_botTerritoryDeploymentPhase = territoryPhase;
            m_botTerritoryAttackingTeam = territoryAttackingTeam;
            Logger::Info(
                "[GameServer] redeployed bots for Territory round %d",
                currentRound + 1);
        }
    } else if (m_territoryMode && IsBotGameplayLive() &&
               territoryPhase >= 0 &&
               (territoryPhase != m_botTerritoryDeploymentPhase ||
                territoryAttackingTeam != m_botTerritoryAttackingTeam)) {
        // Objective activation is also a deployment transaction. Apply the
        // new role-relative spawn view to living bots, not only to later deaths;
        // otherwise both squads leave the prior capture center together and an
        // exact symmetric combat/capture loop is permanent.
        m_botManager->RedeployForTerritoryPhase();
        m_botTerritoryDeploymentPhase = territoryPhase;
        m_botTerritoryAttackingTeam = territoryAttackingTeam;
        Logger::Info(
            "[GameServer] redeployed bots for Territory phase %d "
            "(attacking team %u)",
            territoryPhase,
            static_cast<unsigned>(territoryAttackingTeam));
    } else if (!m_territoryMode) {
        m_botTerritoryDeploymentPhase = -1;
        m_botTerritoryAttackingTeam = 0;
    }

    // Reopen only after the coherent round reset and current-map views exist.
    m_botManager->SetTeamRespawnAllowed(1, respawnTeam1);
    m_botManager->SetTeamRespawnAllowed(2, respawnTeam2);

    const std::size_t team1Humans = m_teamManager
        ? m_teamManager->GetTeamPlayers(1).size() : 0;
    const std::size_t team2Humans = m_teamManager
        ? m_teamManager->GetTeamPlayers(2).size() : 0;
    m_botManager->SetHumanTeamCounts(team1Humans, team2Humans);

    // Replace the complete human perception view every frame. BotManager
    // consumes this snapshot in one Update call and then clears it, so a
    // disconnect, death or malformed position cannot remain targetable.
    std::vector<BotExternalHumanSnapshot> humanCombatants;
    if (m_playerManager) {
        for (const std::shared_ptr<Player>& player :
             m_playerManager->GetAllPlayers()) {
            if (!player || !player->GetConnection()) continue;
            const uint32_t team = player->GetTeam();
            if (team != BotManager::kTeamOne &&
                team != BotManager::kTeamTwo) {
                continue;
            }
            humanCombatants.push_back(BotExternalHumanSnapshot{
                ParticipantId::Human(
                    player->GetConnection()->GetClientId()),
                static_cast<uint8_t>(team), player->GetPosition(),
                player->IsAlive()});
        }
    }
    if (!m_botManager->SetExternalHumanCombatants(humanCombatants)) {
        Logger::Warn(
            "[GameServer] rejected malformed external human bot-combat view");
    }
}

bool GameServer::ConsumeDeferredMapChange() {
    if (!m_mapChangeRequested) return false;

    // Clear before the attempt. Native terminal rotation has a separate latch,
    // so any failure below remains one-shot instead of becoming a hot loop.
    m_mapChangeRequested = false;
    if (IsShutdownRequested()) {
        m_pendingRequestedMap.clear();
        Logger::Debug("[GameServer::Run] dropping deferred map change during shutdown");
        return false;
    }

    ChangeMap();
    return true;
}

void GameServer::ChangeMap() {
    Logger::Trace("[GameServer::ChangeMap] Entry");
    // GUARD: MapManager may be absent (init failed) or already torn down.
    if (!m_mapManager) {
        m_pendingRequestedMap.clear();
        Logger::Error("[GameServer::ChangeMap] No MapManager available; cannot change map");
        Logger::Trace("[GameServer::ChangeMap] Exit (no MapManager)");
        return;
    }
    // An explicit admin request wins over an end-of-round vote and automatic
    // rotation. Consume it before the attempt so all failure paths remain
    // one-shot and cannot hot-loop.
    std::string nextMap;
    if (!m_pendingRequestedMap.empty()) {
        nextMap = std::move(m_pendingRequestedMap);
        m_pendingRequestedMap.clear();
        m_pendingVoteWinner.clear();
        if (m_mapVoteManager && m_mapVoteManager->IsVoteActive()) {
            m_mapVoteManager->Cancel();
        }
        Logger::Info(
            "[GameServer::ChangeMap] Using explicit admin target: '%s'",
            nextMap.c_str());
    } else {
        // If an end-of-round vote concluded, honor its winner; otherwise fall
        // back to the exact-profile automatic rotation order.
        if (m_mapVoteManager && m_mapVoteManager->IsVoteActive()) {
            m_pendingVoteWinner = m_mapVoteManager->ResolveWinner();
            if (!m_pendingVoteWinner.empty()) {
                Logger::Info(
                    "[GameServer::ChangeMap] Using map vote winner: '%s'",
                    m_pendingVoteWinner.c_str());
            }
        }
        if (!m_pendingVoteWinner.empty()) {
            nextMap = std::move(m_pendingVoteWinner);
            m_pendingVoteWinner.clear();
        } else {
            nextMap = m_mapManager->GetNextMap();
        }
    }

    // GUARD: an empty next-map (no rotation configured / lookup miss) must not be
    // fed to LoadMap. Keep the current map running instead of unloading to nothing.
    if (nextMap.empty()) {
        Logger::Warn("[GameServer::ChangeMap] Next map is empty (no rotation entry); staying on current map");
        Logger::Trace("[GameServer::ChangeMap] Exit (empty next map)");
        return;
    }
    // Runtime travel must never reuse ResolveProfile's legacy Resort fallback.
    // Cross-check the exact bootstrap profile against the independently
    // grounded cooked-package resolver before LoadMap, ClientTravel, or any
    // old-world teardown mutates state.
    const std::optional<RetailBootstrap::Profile> travelProfile =
        RetailBootstrap::ResolveExactProfile(nextMap, {});
    const std::optional<RetailBootstrap::GuidBytes> mapGuid =
        RetailBootstrap::ResolveMapPackageGuid(nextMap);
    if (!travelProfile.has_value() || !mapGuid.has_value() ||
        travelProfile->usedFallback ||
        travelProfile->mapPackageGuid != *mapGuid) {
        Logger::Error(
            "[GameServer::ChangeMap] Refusing runtime map change to '%s': "
            "no exact, source-grounded bootstrap URL/GUID profile is available",
            nextMap.c_str());
        Logger::Trace("[GameServer::ChangeMap] Exit (unsupported retail profile)");
        return;
    }
    Logger::Info(
        "[GameServer::ChangeMap] Changing to exact-profile map: '%s'",
        travelProfile->mapUrl.c_str());

    // Preflight the exact RPC before committing any map state. UE3
    // GameInfo::ProcessClientTravel uses TRAVEL_Relative for remote players.
    // This server does not implement seamless world preservation, so false is
    // intentional. The canonical URL and GUID come from the same exact profile,
    // preventing a case-normalization or resolver mismatch on reconnect.
    ClientTravelRepl::Request travelRequest;
    travelRequest.url = travelProfile->mapUrl;
    travelRequest.travelType = ClientTravelRepl::TravelType::Relative;
    travelRequest.seamless = false;
    travelRequest.mapPackageGuid = travelProfile->mapPackageGuid;

    ClientTravelRepl::EncodedRpc travelRpc;
    std::string travelError;
    if (!ClientTravelRepl::Encode(travelRequest, travelRpc, travelError)) {
        Logger::Error(
            "[GameServer::ChangeMap] Refusing map change because ClientTravel "
            "could not be encoded for '%s': %s",
            nextMap.c_str(), travelError.c_str());
        Logger::Trace("[GameServer::ChangeMap] Exit (invalid ClientTravel)");
        return;
    }

    size_t eligibleTravelClients = 0;
    if (m_networkManager &&
        !m_networkManager->CanBroadcastRetailClientTravel(
            travelRpc, travelProfile->mapUrl, &eligibleTravelClients)) {
        Logger::Error(
            "[GameServer::ChangeMap] Refusing local map change to '%s': "
            "the complete joined-client travel cohort is not ready",
            travelProfile->mapUrl.c_str());
        Logger::Trace(
            "[GameServer::ChangeMap] Exit (ClientTravel preflight failed)");
        return;
    }

    const std::string previousMap = m_mapManager->GetCurrentMapName();
    if (m_mapManager->LoadMap(nextMap)) {
        // LoadMap validates the configured definition/package synchronously and
        // restores the current map on failure. Queue travel only after that
        // commit succeeds, but before tearing down the old mode/actor state, as
        // UE3 ProcessClientTravel does before the server world switch.
        const size_t travellingClients = m_networkManager
            ? m_networkManager->BroadcastRetailClientTravel(
                  travelRpc, travelProfile->mapUrl)
            : 0u;
        if (travellingClients != eligibleTravelClients) {
            Logger::Error(
                "[GameServer::ChangeMap] ClientTravel cohort changed while "
                "committing '%s' (%zu expected, %zu queued); rolling back "
                "the local map before old-world teardown",
                travelProfile->mapUrl.c_str(), eligibleTravelClients,
                travellingClients);
            if (previousMap.empty() || !m_mapManager->LoadMap(previousMap)) {
                Logger::Error(
                    "[GameServer::ChangeMap] CRITICAL: failed to restore "
                    "previous map '%s' after ClientTravel commit failure",
                    previousMap.c_str());
            }
            Logger::Trace(
                "[GameServer::ChangeMap] Exit (ClientTravel commit failed)");
            return;
        }
        Logger::Info(
            "[GameServer::ChangeMap] ClientTravel queued for %zu retail "
            "client(s) before local world transition",
            travellingClients);

        if (m_combatAuthority) {
            ProcessCombatEvents(m_combatAuthority->RemoveAllProjectiles());
        }
        m_retailM61Projectiles.clear();
        m_retailM61ReplicationAccumulator = 0.0f;
        if (m_mantleAuthority) m_mantleAuthority->Clear();
        // A travelled endpoint keeps its old reliable ledger long enough to
        // deliver ClientTravel, so immediate connection deletion would be
        // unsafe. Retire only server-owned gameplay state here. The fresh
        // same-endpoint handshake then removes the old protocol session and
        // constructs a new per-map PackageMap/actor bootstrap state.
        if (m_playerManager) {
            for (const std::shared_ptr<Player>& player :
                 m_playerManager->GetAllPlayers()) {
                if (!player) continue;
                const std::shared_ptr<ClientConnection> connection =
                    player->GetConnection();
                if (!connection || !connection->IsUE3Client() ||
                    !connection->IsHandshakeComplete()) {
                    continue;
                }
                const uint32_t clientId = connection->GetClientId();
                player->Reset();
                player->SetReadyToSpawn(false);
                RemoveCombatParticipant(clientId);
            }
        }
        Logger::Debug("[GameServer::ChangeMap] Map loaded successfully, transitioning GameMode");
        if (m_gameMode) {
            Logger::Debug("[GameServer::ChangeMap] Ending current generic GameMode");
            m_gameMode->OnEnd();
            m_gameMode.reset();
        }
        InitializeActiveModeDriver();
        // Re-register the new map's objectives with the ObjectiveSystem.
        PopulateObjectivesFromMap();
        // A generic map gets a freshly initialized central state/manager when
        // requested; a native map destroys any pair left by the previous map.
        ReconcileCentralRoundState();
        // Replace the previous map's spawn registry before adding the newly
        // loaded points. Without the clear, rotations accumulate stale spawns.
        if (m_spawnSystem) m_spawnSystem->Initialize();
        PopulateSpawnsFromMap();
        InitializeBotsForCurrentMap();
        if (m_mutatorManager) m_mutatorManager->DispatchRoundEnd();
        if (m_activeModeDriver == ActiveModeDriver::Generic) {
            const auto& gmDef = m_gameConfig->GetGameModeDefinition(GetEffectiveGameModeName());
            if (gmDef) {
                Logger::Debug("[GameServer::ChangeMap] Creating new generic GameMode instance");
                m_gameMode = std::make_unique<GameMode>(this, *gmDef);
                m_gameMode->OnStart();
            } else {
                Logger::Warn("[GameServer::ChangeMap] No valid game mode definition found after map change");
            }
        }
        if (m_mutatorManager) m_mutatorManager->DispatchRoundStart();
    } else {
        Logger::Error("[GameServer::ChangeMap] Failed to change to map: %s", nextMap.c_str());
    }
    Logger::Trace("[GameServer::ChangeMap] Exit");
}

void GameServer::RequestMapChange() {
    if (IsShutdownRequested()) {
        Logger::Debug("[GameServer::RequestMapChange] ignored during shutdown");
        return;
    }
    if (m_mapChangeRequested) {
        Logger::Debug("[GameServer::RequestMapChange] map change already queued");
        return;
    }
    m_mapChangeRequested = true;
    Logger::Debug("[GameServer::RequestMapChange] queued deferred map change");
}

bool GameServer::RequestMapChange(const std::string& mapName,
                                  std::string& error) {
    if (IsShutdownRequested()) {
        error = "server is shutting down";
        return false;
    }
    if (mapName.empty()) {
        error = "map name is empty";
        return false;
    }
    if (m_mapChangeRequested) {
        error = "another map change is already queued";
        return false;
    }
    if (!m_mapManager || !m_mapConfig) {
        error = "map system is unavailable";
        return false;
    }
    if (!m_mapConfig->GetDefinition(mapName)) {
        error = "map is not present in the configured rotation";
        return false;
    }

    const std::optional<RetailBootstrap::Profile> profile =
        RetailBootstrap::ResolveExactProfile(mapName, {});
    const std::optional<RetailBootstrap::GuidBytes> guid =
        RetailBootstrap::ResolveMapPackageGuid(mapName);
    if (!profile.has_value() || !guid.has_value() || profile->usedFallback ||
        profile->mapPackageGuid != *guid) {
        error = "map has no exact source-grounded retail bootstrap profile";
        return false;
    }

    m_pendingRequestedMap = mapName;
    m_pendingVoteWinner.clear();
    if (m_mapVoteManager && m_mapVoteManager->IsVoteActive()) {
        m_mapVoteManager->Cancel();
    }
    m_mapChangeRequested = true;
    error.clear();
    Logger::Info(
        "[GameServer::RequestMapChange] queued explicit target '%s'",
        mapName.c_str());
    return true;
}

bool GameServer::StartMapVote()
{
    if (!m_mapVoteManager) {
        Logger::Warn("[GameServer::StartMapVote] No MapVoteManager available");
        return false;
    }
    if (!m_mapVoteManager->IsEnabled()) {
        Logger::Debug("[GameServer::StartMapVote] Map voting disabled by config");
        return false;
    }
    std::string current = m_mapManager ? m_mapManager->GetCurrentMapName() : "";
    const auto& candidates = m_mapVoteManager->StartVote(current);
    if (candidates.empty()) {
        Logger::Warn("[GameServer::StartMapVote] No candidates; vote not started");
        return false;
    }

    // Announce options to players.
    std::string msg = "Map vote started! Type votemap <number>:";
    BroadcastChatMessage(msg);
    for (size_t i = 0; i < candidates.size(); ++i) {
        BroadcastChatMessage("  " + std::to_string(i + 1) + ") " + candidates[i].displayName);
    }
    Logger::Info("[GameServer::StartMapVote] Vote started with %zu options", candidates.size());
    return true;
}

bool GameServer::CastMapVote(uint32_t clientId, int optionIndex)
{
    if (!m_mapVoteManager) return false;
    return m_mapVoteManager->CastVote(clientId, optionIndex);
}

// Packet queue implementation
void GameServer::EnqueuePacket(const QueuedPacket& pkt) {
    Logger::Trace("[GameServer::EnqueuePacket] Entry, clientId=%u", pkt.clientId);
    std::lock_guard<std::mutex> lock(m_packetQueueMutex);
    m_packetQueue.push(pkt);
    Logger::Trace("[GameServer::EnqueuePacket] Exit");
}

std::vector<QueuedPacket> GameServer::FetchPendingPackets() {
    Logger::Trace("[GameServer::FetchPendingPackets] Entry");
    std::vector<QueuedPacket> out;
    std::lock_guard<std::mutex> lock(m_packetQueueMutex);
    while (!m_packetQueue.empty()) {
        out.push_back(std::move(m_packetQueue.front()));
        m_packetQueue.pop();
    }
    Logger::Trace("[GameServer::FetchPendingPackets] Exit, returning %zu packets", out.size());
    return out;
}

void GameServer::OnPacketReceived(uint32_t clientId, const Packet& pkt, const PacketMetadata& meta) {
    Logger::Trace("[GameServer::OnPacketReceived] Entry, clientId=%u", clientId);
    EnqueuePacket({clientId, pkt, meta});
    Logger::Trace("[GameServer::OnPacketReceived] Exit");
}

void GameServer::ProcessNetworkMessages() {
    Logger::Trace("[GameServer::ProcessNetworkMessages] Entry");
    if (m_networkManager) {
        m_networkManager->PollNetwork();
    } else {
        Logger::Debug("[GameServer::ProcessNetworkMessages] No NetworkManager available");
    }
    Logger::Trace("[GameServer::ProcessNetworkMessages] Exit");
}

std::string GameServer::GetExeDir() const {
    Logger::Trace("[GameServer::GetExeDir] Entry");
    std::string result = PathUtils::GetExecutableDirectory();
    Logger::Trace("[GameServer::GetExeDir] Exit, returning '%s'", result.c_str());
    return result;
}

// ============================================================================
// RS2V Packet Handlers
// ============================================================================

void GameServer::HandleRoleSelection(uint32_t clientId, const std::vector<uint8_t>& data) {
    Logger::Trace("[GameServer::HandleRoleSelection] Entry, clientId=%u, dataSize=%zu", clientId, data.size());
    if (!m_roleSystem || data.size() < 1) {
        Logger::Debug("[GameServer::HandleRoleSelection] No role system or insufficient data");
        Logger::Trace("[GameServer::HandleRoleSelection] Exit (early return)");
        return;
    }

    CombatRole role = static_cast<CombatRole>(data[0]);
    Logger::Debug("[GameServer::HandleRoleSelection] Player %u requesting role %d", clientId, static_cast<int>(role));
    if (m_roleSystem->AssignRole(clientId, role)) {
        Logger::Info("Player %u selected role: %s", clientId, m_roleSystem->GetRoleName(role).c_str());

        // Apply role loadout to player
        // GUARD: TeamManager/PlayerManager are dereferenced below; either can be
        // null during shutdown or a partial init. The role was already assigned;
        // just skip loadout application rather than crash.
        auto* tm = GetTeamManager();
        auto* pm = GetPlayerManager();
        if (!tm || !pm) {
            Logger::Warn("[GameServer::HandleRoleSelection] TeamManager/PlayerManager unavailable; "
                         "role assigned but loadout skipped for player %u", clientId);
            Logger::Trace("[GameServer::HandleRoleSelection] Exit (manager unavailable)");
            return;
        }
        Faction faction = m_roleSystem->GetTeamFaction(tm->GetPlayerTeam(clientId));
        RoleLoadout loadout = m_roleSystem->GetRoleLoadout(role, faction);
        Logger::Debug("[GameServer::HandleRoleSelection] Applying loadout: primary='%s', secondary='%s'",
                     loadout.primaryWeapon.c_str(), loadout.secondaryWeapon.c_str());

        auto player = pm->GetPlayer(clientId);
        if (player) {
            // Source ROPlayerController.SelectRoleByClass does NOT swap a LIVE pawn's loadout in
            // place mid-round - the new role is deferred to the next spawn (MyDesiredRoleInfo) or
            // forces a respawn (Pawn.Suicide). So only apply the loadout + deploy-ready flag when
            // the player is NOT currently alive. A living player's role is still RECORDED
            // (AssignRole above) and takes effect when they next deploy/respawn; this stops the
            // exploit where an alive player hot-swaps to a fresh kit for free.
            if (!player->IsAlive()) {
                player->ClearInventory();
                player->AddItem(loadout.primaryWeapon, loadout.primaryAmmo);
                if (!loadout.secondaryWeapon.empty()) {
                    player->AddItem(loadout.secondaryWeapon, loadout.secondaryAmmo);
                }
                for (const auto& eq : loadout.equipment) {
                    player->AddItem(eq, 1);
                }
                // Selecting a role IS the deploy action - mark ready so the respawn loop spawns
                // them (the ready-gate in PlayerManager::Update). Mirrors the netcode
                // SelectRoleByClass path in ConnectionManager::DecodeInboundActorBunch.
                player->SetReadyToSpawn(true);
                Logger::Debug("[GameServer::HandleRoleSelection] Loadout applied to player %u", clientId);
            } else {
                Logger::Info("[GameServer::HandleRoleSelection] Player %u changed role while alive; "
                             "role recorded but loadout deferred to next spawn (no in-place swap)", clientId);
            }
        } else {
            Logger::Warn("[GameServer::HandleRoleSelection] Player %u not found in PlayerManager", clientId);
        }
    } else {
        Logger::Warn("Player %u role selection failed: role %d", clientId, static_cast<int>(role));
    }
    Logger::Trace("[GameServer::HandleRoleSelection] Exit");
}

void GameServer::HandleSpawnRequest(uint32_t clientId, const std::vector<uint8_t>& data) {
    Logger::Trace("[GameServer::HandleSpawnRequest] Entry, clientId=%u, dataSize=%zu", clientId, data.size());
    if (!m_spawnSystem) {
        Logger::Debug("[GameServer::HandleSpawnRequest] No spawn system available");
        Logger::Trace("[GameServer::HandleSpawnRequest] Exit (no spawn system)");
        return;
    }

    if (data.size() >= 4) {
        uint32_t spawnLocId = 0;
        memcpy(&spawnLocId, data.data(), sizeof(uint32_t));
        Logger::Debug("[GameServer::HandleSpawnRequest] Player %u requesting spawn at location %u", clientId, spawnLocId);
        if (m_spawnSystem->SpawnPlayer(clientId, spawnLocId)) {
            Logger::Debug("Player %u spawned at location %u", clientId, spawnLocId);
        } else {
            Logger::Debug("[GameServer::HandleSpawnRequest] Spawn at location %u failed, trying default spawn", spawnLocId);
            // Try default spawn
            m_spawnSystem->SpawnPlayerAtDefault(clientId);
        }
    } else {
        Logger::Debug("[GameServer::HandleSpawnRequest] Insufficient data for location, using default spawn");
        m_spawnSystem->SpawnPlayerAtDefault(clientId);
    }
    Logger::Trace("[GameServer::HandleSpawnRequest] Exit");
}

void GameServer::HandleCommanderAbility(uint32_t clientId, const std::vector<uint8_t>& data) {
    Logger::Trace("[GameServer::HandleCommanderAbility] Entry, clientId=%u, dataSize=%zu", clientId, data.size());
    if (!m_commanderAbilities || !m_roleSystem) {
        Logger::Debug("[GameServer::HandleCommanderAbility] Missing commander abilities or role system");
        Logger::Trace("[GameServer::HandleCommanderAbility] Exit (missing systems)");
        return;
    }

    // Verify player is commander
    if (m_roleSystem->GetPlayerRole(clientId) != CombatRole::Commander) {
        Logger::Warn("Player %u tried commander ability but is not commander", clientId);
        Logger::Trace("[GameServer::HandleCommanderAbility] Exit (not commander)");
        return;
    }

    if (data.size() < 13) {
        Logger::Debug("[GameServer::HandleCommanderAbility] Insufficient data (need 13, got %zu)", data.size());
        Logger::Trace("[GameServer::HandleCommanderAbility] Exit (insufficient data)");
        return;
    }  // 1 byte type + 12 bytes position

    AbilityType type = static_cast<AbilityType>(data[0]);
    Vector3 target;
    memcpy(&target.x, data.data() + 1, sizeof(float));
    memcpy(&target.y, data.data() + 5, sizeof(float));
    memcpy(&target.z, data.data() + 9, sizeof(float));
    Logger::Debug("[GameServer::HandleCommanderAbility] Ability type=%d, target=(%.1f, %.1f, %.1f)",
                 static_cast<int>(type), target.x, target.y, target.z);

    Vector3 direction;
    if (data.size() >= 25) {
        memcpy(&direction.x, data.data() + 13, sizeof(float));
        memcpy(&direction.y, data.data() + 17, sizeof(float));
        memcpy(&direction.z, data.data() + 21, sizeof(float));
        Logger::Debug("[GameServer::HandleCommanderAbility] Direction=(%.1f, %.1f, %.1f)",
                     direction.x, direction.y, direction.z);
    } else {
        Logger::Debug("[GameServer::HandleCommanderAbility] No direction data provided");
    }

    if (m_commanderAbilities->RequestAbility(clientId, type, target, direction)) {
        Logger::Info("Commander %u called %s at (%.1f, %.1f, %.1f)",
                     clientId, m_commanderAbilities->GetAbilityName(type).c_str(),
                     target.x, target.y, target.z);
    } else {
        Logger::Debug("[GameServer::HandleCommanderAbility] Ability request denied for commander %u", clientId);
    }
    Logger::Trace("[GameServer::HandleCommanderAbility] Exit");
}

void GameServer::HandleSquadAction(uint32_t clientId, const std::vector<uint8_t>& data) {
    Logger::Trace("[GameServer::HandleSquadAction] Entry, clientId=%u, dataSize=%zu", clientId, data.size());
    if (!m_roleSystem || data.size() < 1) {
        Logger::Debug("[GameServer::HandleSquadAction] No role system or insufficient data");
        Logger::Trace("[GameServer::HandleSquadAction] Exit (early return)");
        return;
    }

    uint8_t action = data[0];
    Logger::Debug("[GameServer::HandleSquadAction] Player %u squad action=%u", clientId, action);
    switch (action) {
        case 0: {  // Create squad
            Logger::Debug("[GameServer::HandleSquadAction] Action: Create squad");
            // GUARD: TeamManager may be null during shutdown/partial init.
            auto* tm = GetTeamManager();
            if (!tm) {
                Logger::Warn("[GameServer::HandleSquadAction] TeamManager unavailable; cannot create squad for player %u", clientId);
                break;
            }
            uint32_t teamId = tm->GetPlayerTeam(clientId);
            uint32_t squadId = m_roleSystem->CreateSquad(teamId);
            if (squadId > 0) {
                m_roleSystem->JoinSquad(clientId, squadId);
                Logger::Info("Player %u created and joined squad %u", clientId, squadId);
            } else {
                Logger::Warn("[GameServer::HandleSquadAction] Squad creation failed for player %u on team %u", clientId, teamId);
            }
            break;
        }
        case 1: {  // Join squad
            Logger::Debug("[GameServer::HandleSquadAction] Action: Join squad");
            if (data.size() >= 5) {
                uint32_t squadId = 0;
                memcpy(&squadId, data.data() + 1, sizeof(uint32_t));
                Logger::Debug("[GameServer::HandleSquadAction] Player %u joining squad %u", clientId, squadId);
                m_roleSystem->JoinSquad(clientId, squadId);
            } else {
                Logger::Debug("[GameServer::HandleSquadAction] Insufficient data for join squad action");
            }
            break;
        }
        case 2:  // Leave squad
            Logger::Debug("[GameServer::HandleSquadAction] Action: Leave squad, player %u", clientId);
            m_roleSystem->LeaveSquad(clientId);
            break;
        case 3:  // Volunteer as commander
            Logger::Debug("[GameServer::HandleSquadAction] Action: Volunteer as commander, player %u", clientId);
            m_roleSystem->VolunteerAsCommander(clientId);
            break;
        case 4:  // Resign as commander
            Logger::Debug("[GameServer::HandleSquadAction] Action: Resign as commander, player %u", clientId);
            m_roleSystem->ResignAsCommander(clientId);
            break;
        default:
            Logger::Debug("[GameServer::HandleSquadAction] Unknown squad action %u from player %u", action, clientId);
            break;
    }
    Logger::Trace("[GameServer::HandleSquadAction] Exit");
}

void GameServer::HandleVehicleAction(uint32_t clientId, const std::vector<uint8_t>& data) {
    Logger::Trace("[GameServer::HandleVehicleAction] Entry, clientId=%u, dataSize=%zu", clientId, data.size());
    if (!m_helicopterPhysics || data.size() < 1) {
        Logger::Debug("[GameServer::HandleVehicleAction] No helicopter physics or insufficient data");
        Logger::Trace("[GameServer::HandleVehicleAction] Exit (early return)");
        return;
    }

    uint8_t action = data[0];
    Logger::Debug("[GameServer::HandleVehicleAction] Player %u vehicle action=%u", clientId, action);

    // SECURITY: heliId for Start/Stop engine is attacker-controlled. Without an ownership
    // check any connected client could start or stop the engine of ANY helicopter on the map
    // by spoofing heliId (a cross-player griefing vector). Mirror the case-2 control path,
    // which already requires the requester to be the heli's pilot. Additive: the legitimate
    // pilot is unaffected; only spoofed/foreign heliIds are rejected. (Review wf_fff418dc-46f.)
    auto requesterPilotsHeli = [this](uint32_t cid, uint32_t hid) -> bool {
        for (auto* h : m_helicopterPhysics->GetAllHelicopters()) {
            if (h->vehicleId == hid) return h->pilotId == cid;
        }
        return false;  // unknown heli id -> not authorized
    };

    switch (action) {
        case 0: {  // Enter helicopter
            Logger::Debug("[GameServer::HandleVehicleAction] Action: Enter helicopter");
            if (data.size() >= 9) {
                uint32_t heliId = 0, seat = 0;
                memcpy(&heliId, data.data() + 1, sizeof(uint32_t));
                memcpy(&seat, data.data() + 5, sizeof(uint32_t));
                Logger::Debug("[GameServer::HandleVehicleAction] Player %u entering heli %u seat %u", clientId, heliId, seat);
                m_helicopterPhysics->EnterHelicopter(clientId, heliId, seat);
            } else {
                Logger::Debug("[GameServer::HandleVehicleAction] Insufficient data for enter helicopter");
            }
            break;
        }
        case 1:  // Exit helicopter
            Logger::Debug("[GameServer::HandleVehicleAction] Action: Exit helicopter, player %u", clientId);
            m_helicopterPhysics->ExitHelicopter(clientId);
            break;
        case 2: {  // Control input
            Logger::Trace("[GameServer::HandleVehicleAction] Action: Control input");
            if (data.size() >= 18) {
                HeliControlInput input;
                memcpy(&input.collective, data.data() + 1, sizeof(float));
                memcpy(&input.cyclic_pitch, data.data() + 5, sizeof(float));
                memcpy(&input.cyclic_roll, data.data() + 9, sizeof(float));
                memcpy(&input.pedal, data.data() + 13, sizeof(float));
                input.fireWeapon = (data.size() > 17 && data[17] != 0);
                // Find helicopter this player is piloting
                bool found = false;
                for (auto* h : m_helicopterPhysics->GetAllHelicopters()) {
                    if (h->pilotId == clientId) {
                        m_helicopterPhysics->SetControlInput(h->vehicleId, input);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    Logger::Debug("[GameServer::HandleVehicleAction] Player %u not piloting any helicopter", clientId);
                }
            } else {
                Logger::Debug("[GameServer::HandleVehicleAction] Insufficient data for control input");
            }
            break;
        }
        case 3: {  // Start engine
            Logger::Debug("[GameServer::HandleVehicleAction] Action: Start engine");
            if (data.size() >= 5) {
                uint32_t heliId = 0;
                memcpy(&heliId, data.data() + 1, sizeof(uint32_t));
                if (!requesterPilotsHeli(clientId, heliId)) {
                    Logger::Warn("[GameServer::HandleVehicleAction] Player %u tried to START engine of heli %u it does not pilot - rejected",
                                 clientId, heliId);
                    break;
                }
                Logger::Debug("[GameServer::HandleVehicleAction] Starting engine on heli %u", heliId);
                m_helicopterPhysics->StartEngine(heliId);
            } else {
                Logger::Debug("[GameServer::HandleVehicleAction] Insufficient data for start engine");
            }
            break;
        }
        case 4: {  // Stop engine
            Logger::Debug("[GameServer::HandleVehicleAction] Action: Stop engine");
            if (data.size() >= 5) {
                uint32_t heliId = 0;
                memcpy(&heliId, data.data() + 1, sizeof(uint32_t));
                if (!requesterPilotsHeli(clientId, heliId)) {
                    Logger::Warn("[GameServer::HandleVehicleAction] Player %u tried to STOP engine of heli %u it does not pilot - rejected",
                                 clientId, heliId);
                    break;
                }
                Logger::Debug("[GameServer::HandleVehicleAction] Stopping engine on heli %u", heliId);
                m_helicopterPhysics->StopEngine(heliId);
            } else {
                Logger::Debug("[GameServer::HandleVehicleAction] Insufficient data for stop engine");
            }
            break;
        }
        default:
            Logger::Debug("[GameServer::HandleVehicleAction] Unknown vehicle action %u from player %u", action, clientId);
            break;
    }
    Logger::Trace("[GameServer::HandleVehicleAction] Exit");
}

void GameServer::HandleWeaponFire(uint32_t clientId, const std::vector<uint8_t>& data) {
    Logger::Trace("[GameServer::HandleWeaponFire] Entry, clientId=%u, dataSize=%zu", clientId, data.size());
    if (!m_damageSystem || data.size() < 29) {
        Logger::Debug("[GameServer::HandleWeaponFire] No damage system or insufficient data (need 29, got %zu)", data.size());
        Logger::Trace("[GameServer::HandleWeaponFire] Exit (early return)");
        return;
    }

    // Parse weapon fire data: weaponId(variable) + origin(12) + direction(12) + hitZone(1) + victimId(4)
    // Simplified: first byte = weapon id string length
    uint8_t weaponIdLen = data[0];
    if (data.size() < static_cast<size_t>(1 + weaponIdLen + 29)) {
        Logger::Debug("[GameServer::HandleWeaponFire] Data too short for weapon id len %u", weaponIdLen);
        Logger::Trace("[GameServer::HandleWeaponFire] Exit (data too short)");
        return;
    }

    std::string weaponId(data.begin() + 1, data.begin() + 1 + weaponIdLen);
    size_t offset = 1 + weaponIdLen;

    Vector3 origin, direction;
    memcpy(&origin.x, data.data() + offset, sizeof(float)); offset += 4;
    memcpy(&origin.y, data.data() + offset, sizeof(float)); offset += 4;
    memcpy(&origin.z, data.data() + offset, sizeof(float)); offset += 4;
    memcpy(&direction.x, data.data() + offset, sizeof(float)); offset += 4;
    memcpy(&direction.y, data.data() + offset, sizeof(float)); offset += 4;
    memcpy(&direction.z, data.data() + offset, sizeof(float)); offset += 4;

    HitZone hitZone = static_cast<HitZone>(data[offset]); offset++;
    uint32_t victimId = 0;
    memcpy(&victimId, data.data() + offset, sizeof(uint32_t));

    Logger::Debug("[GameServer::HandleWeaponFire] Player %u fired '%s' at victim %u, hitZone=%d",
                 clientId, weaponId.c_str(), victimId, static_cast<int>(hitZone));

    if (victimId == 0) {
        Logger::Debug("[GameServer::HandleWeaponFire] Miss (victimId=0), no damage applied");
        Logger::Trace("[GameServer::HandleWeaponFire] Exit (miss)");
        return;  // Miss — no damage
    }

    // GUARD: the attacker must be a live participant. This legacy path trusts an
    // attacker-supplied victimId, so a dead/spectating/never-spawned client could
    // otherwise claim kills. Mirrors the HandleVehicleAction ownership guard (af0d970).
    auto attackerPlayer = m_playerManager ? m_playerManager->GetPlayer(clientId) : nullptr;
    if (!attackerPlayer || !attackerPlayer->IsAlive()) {
        Logger::Warn("[GameServer::HandleWeaponFire] Rejecting fire from clientId=%u: attacker not alive/unknown", clientId);
        Logger::Trace("[GameServer::HandleWeaponFire] Exit (attacker invalid)");
        return;
    }

    // GUARD: the victim must resolve to a real, live player. An attacker-supplied victimId
    // that doesn't exist or is already dead is a spoofed/garbage hit - drop it. (Also avoids
    // the former double GetPlayer() lookup / possible null deref.)
    auto victimPlayer = m_playerManager ? m_playerManager->GetPlayer(victimId) : nullptr;
    if (!victimPlayer || !victimPlayer->IsAlive()) {
        Logger::Warn("[GameServer::HandleWeaponFire] Rejecting fire from clientId=%u: victim %u invalid/dead", clientId, victimId);
        Logger::Trace("[GameServer::HandleWeaponFire] Exit (victim invalid)");
        return;
    }
    float distance = origin.Distance(victimPlayer->GetPosition());

    auto* weaponDef = m_weaponDatabase ? m_weaponDatabase->GetWeapon(weaponId) : nullptr;
    if (!weaponDef) {
        Logger::Warn("[GameServer::HandleWeaponFire] Weapon '%s' not found in database, using default damage", weaponId.c_str());
    }

    // GUARD (server-side rate-of-fire): the weapon's fireRate (RPM) bounds how often a client can
    // register a damaging shot. Reject hits arriving faster than the weapon physically fires (with
    // a 20% jitter tolerance). The source is fully server-authoritative over firing (ROWeapon
    // GetFireInterval + ConsumeAmmo); this is a PARTIAL of that finding - the full per-player
    // Weapon-instance ammo/magazine/reload wiring is a tracked follow-up. Without it a client could
    // claim hits at any cadence (the Weapon class is orphaned from this path).
    if (weaponDef && weaponDef->stats.fireRate > 0.0f) {
        const uint64_t nowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        const double minIntervalMs = 60000.0 / weaponDef->stats.fireRate;  // RPM -> ms per shot
        auto it = m_lastWeaponFireMs.find(clientId);
        if (it != m_lastWeaponFireMs.end() &&
            static_cast<double>(nowMs - it->second) < minIntervalMs * 0.8) {
            Logger::Warn("[GameServer::HandleWeaponFire] Rejecting too-fast shot from clientId=%u ('%s' %.0f RPM -> >=%.0fms/shot, got %llums)",
                         clientId, weaponId.c_str(), weaponDef->stats.fireRate, minIntervalMs,
                         static_cast<unsigned long long>(nowMs - it->second));
            Logger::Trace("[GameServer::HandleWeaponFire] Exit (rate-of-fire violation)");
            return;
        }
        m_lastWeaponFireMs[clientId] = nowMs;
    }

    float baseDmg = weaponDef ? m_weaponDatabase->CalculateDamage(
        weaponId, distance, hitZone == HitZone::Head,
        hitZone == HitZone::LeftArm || hitZone == HitZone::RightArm ||
        hitZone == HitZone::LeftLeg || hitZone == HitZone::RightLeg) : 50.0f;

    Logger::Debug("[GameServer::HandleWeaponFire] Calculated damage=%.1f, distance=%.1f", baseDmg, distance);

    DamageEvent event;
    event.attackerId = clientId;
    event.victimId = victimId;
    event.source = DamageSource::Bullet;
    event.weaponId = weaponId;
    event.hitZone = hitZone;
    event.baseDamage = baseDmg;
    event.distance = distance;
    event.hitPosition = origin;
    event.hitDirection = direction;

    m_damageSystem->ApplyDamage(event);
    Logger::Trace("[GameServer::HandleWeaponFire] Exit");
}
