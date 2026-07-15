#include "Game/BotManager.h"
#include "Game/TeamMapping.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <utility>

namespace {

constexpr float kTimeEpsilon = 0.00001f;
// Bots deliberately converge on exact authored objective coordinates. Treat
// only genuinely co-located/equidistant targets as a cohort; ordinary spatial
// differences must continue to select the strict nearest threat.
constexpr float kTargetDistanceTieEpsilon = 0.01f;

bool IsPlayableTeam(std::uint8_t teamId) {
    return teamId == BotManager::kTeamOne || teamId == BotManager::kTeamTwo;
}

constexpr bool IsValidExternalDamageAuthorization(
    ExternalBotDamageAuthorization authorization) noexcept {
    switch (authorization) {
    case ExternalBotDamageAuthorization::Hostile:
    case ExternalBotDamageAuthorization::FriendlyFireBlocked:
    case ExternalBotDamageAuthorization::FriendlyFireAuthorized:
    case ExternalBotDamageAuthorization::SelfDamageAuthorized:
        return true;
    default:
        return false;
    }
}

} // namespace

BotManager::BotManager(const BotManagerConfig& config) {
    if (!Configure(config)) {
        Configure(BotManagerConfig{});
    }
}

bool BotManager::Configure(const BotManagerConfig& config) {
    if (!IsValidConfig(config)) {
        return false;
    }

    BotNavigation navigationCandidate;
    if (!navigationCandidate.Configure(config.navigation) ||
        !navigationCandidate.ReplaceGraph(config.navigationGraph)) {
        return false;
    }

    navigation_ = std::move(navigationCandidate);
    config_ = config;
    if (accumulator_ >= config_.fixedStepSeconds) {
        accumulator_ = std::fmod(accumulator_, config_.fixedStepSeconds);
    }

    for (auto& entry : bots_) {
        BotAgent& bot = entry.second;
        bot.snapshot.maxHealth = config_.maxHealth;
        bot.snapshot.captureWeight = config_.captureWeight;
        if (bot.snapshot.lifecycle == BotLifecycle::Alive) {
            bot.snapshot.health = std::min(bot.snapshot.health, config_.maxHealth);
        } else {
            bot.snapshot.health = 0.0f;
        }
        bot.route = BotRoute{};
        bot.routeCursor = 0;
        bot.routeObjectiveId = 0;
        bot.routeUsesTransit = false;
        bot.nextCombatShotAt = simulationTime_;
        SyncRoster(bot);
    }

    externalHumans_.clear();
    externalHumansFresh_ = false;
    humanCombatEvents_.clear();
    pendingHumanCombat_.clear();

    ReconcileFill();
    return true;
}

void BotManager::SetEligibleSpawns(const std::vector<BotSpawnSnapshot>& spawns) {
    spawns_.clear();
    spawns_.reserve(spawns.size());
    for (const BotSpawnSnapshot& spawn : spawns) {
        if (spawn.id == 0 || !IsPlayableTeam(spawn.teamId) || !spawn.active ||
            spawn.destroyed || !std::isfinite(spawn.cooldownRemaining) ||
            spawn.cooldownRemaining > 0.0f || !IsFinite(spawn.position) ||
            !navigation_.IsPositionAllowed(spawn.position)) {
            continue;
        }
        spawns_.push_back(spawn);
    }

    std::sort(spawns_.begin(), spawns_.end(),
              [](const BotSpawnSnapshot& left, const BotSpawnSnapshot& right) {
                  if (left.teamId != right.teamId) {
                      return left.teamId < right.teamId;
                  }
                  return left.id < right.id;
              });
    spawns_.erase(std::unique(spawns_.begin(), spawns_.end(),
                              [](const BotSpawnSnapshot& left,
                                 const BotSpawnSnapshot& right) {
                                  return left.teamId == right.teamId && left.id == right.id;
                              }),
                   spawns_.end());

    std::vector<std::uint32_t> queuedIds;
    for (const auto& entry : bots_) {
        if (entry.second.snapshot.lifecycle == BotLifecycle::RespawnQueued) {
            queuedIds.push_back(entry.first);
        }
    }
    for (std::uint32_t id : queuedIds) {
        const auto it = bots_.find(id);
        if (it != bots_.end()) {
            AttemptRespawn(it->second);
        }
    }
}

void BotManager::SetActiveObjectives(
    const std::vector<BotObjectiveSnapshot>& objectives) {
    objectives_.clear();
    objectives_.reserve(objectives.size());
    for (const BotObjectiveSnapshot& objective : objectives) {
        if (objective.id == 0 || !objective.active || !objective.enabled ||
            !IsFinite(objective.position) ||
            !navigation_.IsPositionAllowed(objective.position) ||
            !std::isfinite(objective.captureRadius) || objective.captureRadius <= 0.0f) {
            continue;
        }
        objectives_.push_back(objective);
    }

    std::sort(objectives_.begin(), objectives_.end(),
              [](const BotObjectiveSnapshot& left, const BotObjectiveSnapshot& right) {
                  return left.id < right.id;
              });
    objectives_.erase(std::unique(objectives_.begin(), objectives_.end(),
                                  [](const BotObjectiveSnapshot& left,
                                     const BotObjectiveSnapshot& right) {
                                      return left.id == right.id;
                                  }),
                       objectives_.end());

    for (auto& entry : bots_) {
        entry.second.route = BotRoute{};
        entry.second.routeCursor = 0;
        entry.second.routeObjectiveId = 0;
        entry.second.routeUsesTransit = false;
    }
}

void BotManager::SetHumanTeamCounts(std::size_t teamOneHumans,
                                    std::size_t teamTwoHumans) {
    humanTeamCounts_[kTeamOne] = teamOneHumans;
    humanTeamCounts_[kTeamTwo] = teamTwoHumans;
    ReconcileFill();
}

bool BotManager::SetHumanTeamCount(std::uint8_t teamId, std::size_t humans) {
    if (!IsPlayableTeam(teamId)) {
        return false;
    }
    humanTeamCounts_[teamId] = humans;
    ReconcileFill();
    return true;
}

bool BotManager::SetFillTargetPerTeam(std::size_t target) {
    if (target > config_.maxBotsPerTeam) {
        return false;
    }
    config_.fillTargetPerTeam = target;
    ReconcileFill();
    return true;
}

void BotManager::ReconcileFill() {
    for (std::uint8_t teamId : {kTeamOne, kTeamTwo}) {
        const std::size_t humans = humanTeamCounts_[teamId];
        const std::size_t unboundedDesired =
            humans >= config_.fillTargetPerTeam ? 0 : config_.fillTargetPerTeam - humans;
        const std::size_t desired = std::min(unboundedDesired, config_.maxBotsPerTeam);
        std::size_t current = CountBots(teamId);

        while (current < desired) {
            if (!CreateBot(teamId)) {
                break;
            }
            ++current;
        }

        if (current > desired) {
            std::vector<std::uint32_t> teamBotIds;
            for (const auto& entry : bots_) {
                if (entry.second.snapshot.teamId == teamId) {
                    teamBotIds.push_back(entry.first);
                }
            }
            std::sort(teamBotIds.begin(), teamBotIds.end(), std::greater<std::uint32_t>());
            const std::size_t removeCount = current - desired;
            for (std::size_t i = 0; i < removeCount; ++i) {
                RemoveBot(teamBotIds[i]);
            }
        }
    }
}

bool BotManager::SetExternalHumanCombatants(
    const std::vector<BotExternalHumanSnapshot>& humans) {
    // Starting a new authoritative frame invalidates any unconsumed prior
    // adapter work. A healthy GameServer consumes and resolves every event in
    // the same tick; late resolutions fail closed instead of carrying across
    // participant snapshots.
    externalHumans_.clear();
    externalHumansFresh_ = false;
    humanCombatEvents_.clear();
    pendingHumanCombat_.clear();

    if (humans.size() > kMaxExternalHumans) {
        return false;
    }

    std::map<ParticipantId, bool> unique;
    for (const BotExternalHumanSnapshot& human : humans) {
        if (!human.id.IsHuman() || !IsPlayableTeam(human.teamId) ||
            !IsFinite(human.position) ||
            !unique.emplace(human.id, true).second) {
            return false;
        }
    }

    externalHumans_ = humans;
    std::sort(externalHumans_.begin(), externalHumans_.end(),
              [](const BotExternalHumanSnapshot& left,
                 const BotExternalHumanSnapshot& right) {
                  return left.id < right.id;
              });
    externalHumansFresh_ = true;
    return true;
}

bool BotManager::SetTeamRespawnAllowed(std::uint8_t teamId, bool allowed) {
    if (!IsPlayableTeam(teamId)) {
        return false;
    }
    teamRespawnAllowed_[teamId] = allowed;

    if (allowed) {
        std::vector<std::uint32_t> queuedIds;
        for (const auto& entry : bots_) {
            if (entry.second.snapshot.teamId == teamId &&
                entry.second.snapshot.lifecycle == BotLifecycle::RespawnQueued) {
                queuedIds.push_back(entry.first);
            }
        }
        for (std::uint32_t id : queuedIds) {
            const auto it = bots_.find(id);
            if (it != bots_.end()) {
                AttemptRespawn(it->second);
            }
        }
    }
    return true;
}

bool BotManager::SetTerritoryAttackingTeam(std::uint8_t teamId) {
    if (teamId != 0 && !IsPlayableTeam(teamId)) {
        return false;
    }
    if (territoryAttackingTeam_ == teamId) {
        return true;
    }

    territoryAttackingTeam_ = teamId;
    // A role swap changes the objective policy even when the active objective
    // ids happen to be unchanged. Invalidate every route atomically; the caller
    // normally follows this with a round redeploy, while direct users still get
    // a correct route on their next fixed step.
    for (auto& entry : bots_) {
        BotAgent& bot = entry.second;
        bot.route = BotRoute{};
        bot.routeCursor = 0;
        bot.routeObjectiveId = 0;
        bot.routeUsesTransit = false;
    }
    return true;
}

std::uint8_t BotManager::RemapTerritorySpawnTeam(
    std::uint8_t authoredTeamId, std::uint8_t attackingTeamId) {
    const std::uint8_t defendingTeamId = attackingTeamId == kTeamOne
        ? kTeamTwo : (attackingTeamId == kTeamTwo ? kTeamOne : 0);
    return static_cast<std::uint8_t>(TeamMapping::ResolveTerritoryRoleTeam(
        authoredTeamId, attackingTeamId, defendingTeamId));
}

void BotManager::RedeployForRound() {
    RedeployAll(/*resetAccumulator=*/true);
}

void BotManager::RedeployForTerritoryPhase() {
    RedeployAll(/*resetAccumulator=*/false);
}

void BotManager::RedeployAll(bool resetAccumulator) {
    // A round/phase deployment boundary is not a death. Reset every participant
    // first so a respawn callback cannot observe half of the previous deployment
    // still alive.
    std::vector<std::uint32_t> botIds;
    botIds.reserve(bots_.size());
    for (auto& entry : bots_) {
        BotAgent& bot = entry.second;
        bot.snapshot.lifecycle = BotLifecycle::RespawnQueued;
        bot.snapshot.position = Vector3{};
        bot.snapshot.health = 0.0f;
        bot.snapshot.objectiveId = 0;
        bot.route = BotRoute{};
        bot.routeCursor = 0;
        bot.routeObjectiveId = 0;
        bot.routeUsesTransit = false;
        bot.nextCombatShotAt = simulationTime_;
        bot.respawnAt = simulationTime_;
        SyncRoster(bot);
        botIds.push_back(entry.first);
    }
    if (resetAccumulator) {
        accumulator_ = 0.0f;
    }

    // AttemptRespawn owns the team gate and spawn validation. Keeping this as a
    // second pass also guarantees callbacks see a coherent all-queued roster.
    for (std::uint32_t id : botIds) {
        const auto it = bots_.find(id);
        if (it != bots_.end()) {
            AttemptRespawn(it->second);
        }
    }
}

bool BotManager::Update(float deltaSeconds) {
    if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0f ||
        !std::isfinite(accumulator_ + deltaSeconds)) {
        return false;
    }

    accumulator_ += deltaSeconds;
    std::size_t steps = 0;
    while (accumulator_ + kTimeEpsilon >= config_.fixedStepSeconds &&
           steps < config_.maxCatchUpSteps) {
        accumulator_ = std::max(0.0f, accumulator_ - config_.fixedStepSeconds);
        StepFixed();
        ++steps;
    }

    if (steps == config_.maxCatchUpSteps && accumulator_ >= config_.fixedStepSeconds) {
        accumulator_ = std::fmod(accumulator_, config_.fixedStepSeconds);
    }
    // External participants are a one-Update snapshot. GameServer must submit
    // a complete fresh view before the next call, even when this call did not
    // accumulate a fixed step.
    externalHumans_.clear();
    externalHumansFresh_ = false;
    return true;
}

bool BotManager::ApplyCombatEvent(const BotCombatEvent& event) {
    if (!event.attackerId.IsBot() || !event.victimId.IsBot() ||
        event.attackerId == event.victimId || !std::isfinite(event.damage) ||
        event.damage <= 0.0f) {
        return false;
    }

    const auto attackerIt = bots_.find(event.attackerId.value);
    const auto victimIt = bots_.find(event.victimId.value);
    if (attackerIt == bots_.end() || victimIt == bots_.end() ||
        attackerIt->second.snapshot.lifecycle != BotLifecycle::Alive ||
        victimIt->second.snapshot.lifecycle != BotLifecycle::Alive ||
        attackerIt->second.snapshot.teamId == victimIt->second.snapshot.teamId) {
        return false;
    }

    return ApplyValidatedDamage(victimIt->second, event.attackerId, event.damage);
}

bool BotManager::ApplyExternalDamageToBot(
    const ParticipantId& victimId, const ParticipantId& attackerId, float damage,
    ExternalBotDamageAuthorization authorization) {
    if (!victimId.IsBot() || (!attackerId.IsHuman() && !attackerId.IsBot()) ||
        !std::isfinite(damage) || damage <= 0.0f ||
        !IsValidExternalDamageAuthorization(authorization)) {
        return false;
    }
    if (authorization == ExternalBotDamageAuthorization::FriendlyFireBlocked) {
        return false;
    }

    const auto victimIt = bots_.find(victimId.value);
    if (victimIt == bots_.end() ||
        victimIt->second.snapshot.lifecycle != BotLifecycle::Alive) {
        return false;
    }

    const bool isSelfDamage = attackerId == victimId;
    if (attackerId.IsBot()) {
        const auto attackerIt = bots_.find(attackerId.value);
        if (attackerIt == bots_.end() ||
            attackerIt->second.snapshot.lifecycle != BotLifecycle::Alive) {
            return false;
        }

        const bool isFriendly =
            attackerIt->second.snapshot.teamId == victimIt->second.snapshot.teamId;
        if (isSelfDamage) {
            if (authorization !=
                ExternalBotDamageAuthorization::SelfDamageAuthorized) {
                return false;
            }
        } else if (isFriendly) {
            if (authorization !=
                ExternalBotDamageAuthorization::FriendlyFireAuthorized) {
                return false;
            }
        } else {
            if (authorization != ExternalBotDamageAuthorization::Hostile) {
                return false;
            }
            // Preserve the existing bot-vs-bot validation and event contract.
            return ApplyCombatEvent(BotCombatEvent{attackerId, victimId, damage});
        }
    } else {
        // Human existence, liveness, and team relationship are owned by the
        // authoritative caller. SelfDamageAuthorized can never describe a
        // human-to-bot hit; the other two values are explicit caller results.
        if (authorization ==
            ExternalBotDamageAuthorization::SelfDamageAuthorized) {
            return false;
        }
    }

    return ApplyValidatedDamage(victimIt->second, attackerId, damage);
}

bool BotManager::ApplyValidatedDamage(BotAgent& victim,
                                      const ParticipantId& attackerId,
                                      float damage) {
    combatEvents_.push_back(BotCombatEvent{attackerId, victim.snapshot.id, damage});
    victim.snapshot.health = std::max(0.0f, victim.snapshot.health - damage);
    if (victim.snapshot.health <= 0.0f) {
        return KillBot(victim.snapshot.id, attackerId, true);
    }
    SyncRoster(victim);
    return true;
}

bool BotManager::KillBot(const ParticipantId& botId, const ParticipantId& killerId,
                         bool scheduleRespawn) {
    if (!botId.IsBot()) {
        return false;
    }
    const auto it = bots_.find(botId.value);
    if (it == bots_.end() || it->second.snapshot.lifecycle != BotLifecycle::Alive) {
        return false;
    }

    BotAgent& bot = it->second;
    bot.snapshot.health = 0.0f;
    ++bot.snapshot.deathSequence;
    bot.snapshot.objectiveId = 0;
    bot.route = BotRoute{};
    bot.routeCursor = 0;
    bot.routeObjectiveId = 0;
    bot.routeUsesTransit = false;
    if (scheduleRespawn) {
        bot.snapshot.lifecycle = BotLifecycle::RespawnQueued;
        bot.respawnAt = simulationTime_ + config_.respawnDelaySeconds;
    } else {
        bot.snapshot.lifecycle = BotLifecycle::Dead;
    }
    SyncRoster(bot);

    const BotDeathEvent deathEvent{bot.snapshot.id, killerId, bot.snapshot.teamId,
                                   bot.snapshot.deathSequence};
    deathEvents_.push_back(deathEvent);
    const DeathCallback callback = deathCallback_;
    const DeathBatchCallback batchCallback = deathBatchCallback_;
    if (callback) {
        callback(deathEvent);
    }
    if (batchCallback) {
        batchCallback(std::vector<BotDeathEvent>{deathEvent});
    }
    return true;
}

bool BotManager::QueueRespawn(const ParticipantId& botId) {
    if (!botId.IsBot()) {
        return false;
    }
    const auto it = bots_.find(botId.value);
    if (it == bots_.end() || it->second.snapshot.lifecycle != BotLifecycle::Dead) {
        return false;
    }

    BotAgent& bot = it->second;
    bot.snapshot.lifecycle = BotLifecycle::RespawnQueued;
    bot.respawnAt = simulationTime_ + config_.respawnDelaySeconds;
    SyncRoster(bot);
    return true;
}

const BotSnapshot* BotManager::FindBot(const ParticipantId& botId) const {
    if (!botId.IsBot()) {
        return nullptr;
    }
    const auto it = bots_.find(botId.value);
    return it == bots_.end() ? nullptr : &it->second.snapshot;
}

std::vector<BotSnapshot> BotManager::GetBots() const {
    std::vector<BotSnapshot> result;
    result.reserve(bots_.size());
    for (const auto& entry : bots_) {
        result.push_back(entry.second.snapshot);
    }
    return result;
}

std::size_t BotManager::CountBots(std::uint8_t teamId) const {
    if (!IsPlayableTeam(teamId)) {
        return 0;
    }
    std::size_t count = 0;
    for (const auto& entry : bots_) {
        if (entry.second.snapshot.teamId == teamId) {
            ++count;
        }
    }
    return count;
}

std::size_t BotManager::CountAliveBots(std::uint8_t teamId) const {
    if (!IsPlayableTeam(teamId)) {
        return 0;
    }
    std::size_t count = 0;
    for (const auto& entry : bots_) {
        if (entry.second.snapshot.teamId == teamId &&
            entry.second.snapshot.lifecycle == BotLifecycle::Alive) {
            ++count;
        }
    }
    return count;
}

std::vector<ObjectiveOccupantSnapshot> BotManager::GetObjectiveOccupants() const {
    std::vector<ObjectiveOccupantSnapshot> occupants;
    for (const auto& botEntry : bots_) {
        const BotSnapshot& bot = botEntry.second.snapshot;
        if (bot.lifecycle != BotLifecycle::Alive || bot.captureWeight <= 0.0f) {
            continue;
        }
        for (const BotObjectiveSnapshot& objective : objectives_) {
            if (Distance2D(bot.position, objective.position) <= objective.captureRadius) {
                occupants.push_back(ObjectiveOccupantSnapshot{
                    objective.id, bot.id, bot.teamId, bot.captureWeight});
            }
        }
    }
    return occupants;
}

float BotManager::GetObjectiveCaptureWeight(std::uint32_t objectiveId,
                                            std::uint8_t teamId) const {
    if (!IsPlayableTeam(teamId) || objectiveId == 0) {
        return 0.0f;
    }
    float weight = 0.0f;
    for (const ObjectiveOccupantSnapshot& occupant : GetObjectiveOccupants()) {
        if (occupant.objectiveId == objectiveId && occupant.teamId == teamId) {
            weight += occupant.captureWeight;
        }
    }
    return weight;
}

std::vector<BotCombatEvent> BotManager::ConsumeCombatEvents() {
    std::vector<BotCombatEvent> events;
    events.swap(combatEvents_);
    return events;
}

std::vector<BotHumanCombatEvent> BotManager::ConsumeHumanCombatEvents() {
    std::vector<BotHumanCombatEvent> events;
    events.swap(humanCombatEvents_);
    return events;
}

bool BotManager::IsPendingHumanCombatEvent(
    const BotHumanCombatEvent& event) const {
    const auto pending = pendingHumanCombat_.find(event.shotSequence);
    if (pending == pendingHumanCombat_.end()) return false;
    const BotHumanCombatEvent& expected = pending->second;
    return event.generation != 0 &&
        event.generation == expected.generation &&
        event.shotSequence != 0 &&
        event.attackerId == expected.attackerId &&
        event.victimId == expected.victimId &&
        event.attackerTeamId == expected.attackerTeamId &&
        event.origin.x == expected.origin.x &&
        event.origin.y == expected.origin.y &&
        event.origin.z == expected.origin.z &&
        event.impact.x == expected.impact.x &&
        event.impact.y == expected.impact.y &&
        event.impact.z == expected.impact.z &&
        event.distanceUu == expected.distanceUu &&
        event.damage == expected.damage &&
        event.attackerId.IsBot() && event.victimId.IsHuman();
}

bool BotManager::ResolveHumanCombatEvent(
    const BotHumanCombatEvent& event, BotHumanCombatOutcome outcome) {
    switch (outcome) {
        case BotHumanCombatOutcome::Rejected:
        case BotHumanCombatOutcome::DamageApplied:
        case BotHumanCombatOutcome::TargetKilled:
            break;
        default:
            return false;
    }

    if (!IsPendingHumanCombatEvent(event)) return false;
    pendingHumanCombat_.erase(event.shotSequence);
    if (outcome != BotHumanCombatOutcome::TargetKilled) {
        return true;
    }

    const auto attacker = bots_.find(event.attackerId.value);
    if (attacker == bots_.end() ||
        attacker->second.snapshot.teamId != event.attackerTeamId) {
        return false;
    }
    BotSnapshot& snapshot = attacker->second.snapshot;
    if (snapshot.kills < std::numeric_limits<std::uint32_t>::max()) {
        ++snapshot.kills;
    }
    if (snapshot.score < std::numeric_limits<std::uint32_t>::max()) {
        ++snapshot.score;
    }
    return true;
}

std::vector<BotDeathEvent> BotManager::ConsumeDeathEvents() {
    std::vector<BotDeathEvent> events;
    events.swap(deathEvents_);
    return events;
}

std::vector<BotRespawnEvent> BotManager::ConsumeRespawnEvents() {
    std::vector<BotRespawnEvent> events;
    events.swap(respawnEvents_);
    return events;
}

std::vector<BotRemovalEvent> BotManager::ConsumeRemovalEvents() {
    std::vector<BotRemovalEvent> events;
    events.swap(removalEvents_);
    return events;
}

bool BotManager::CreateBot(std::uint8_t teamId) {
    if (!IsPlayableTeam(teamId) || nextBotId_ == 0) {
        return false;
    }

    const std::uint32_t rawBotId = nextBotId_++;
    BotAgent bot;
    bot.snapshot.id = ParticipantId::Bot(rawBotId);
    bot.snapshot.name = "BOT-T" + std::to_string(teamId) + "-" +
                        std::to_string(rawBotId);
    bot.snapshot.teamId = teamId;
    bot.snapshot.lifecycle = BotLifecycle::RespawnQueued;
    bot.snapshot.maxHealth = config_.maxHealth;
    bot.snapshot.captureWeight = config_.captureWeight;
    bot.respawnAt = simulationTime_;

    auto inserted = bots_.emplace(rawBotId, std::move(bot));
    SyncRoster(inserted.first->second);
    AttemptRespawn(inserted.first->second);
    return true;
}

void BotManager::RemoveBot(std::uint32_t rawBotId) {
    const auto found = bots_.find(rawBotId);
    if (found == bots_.end()) return;

    const BotSnapshot& snapshot = found->second.snapshot;
    removalEvents_.push_back(BotRemovalEvent{
        snapshot.id, snapshot.teamId,
        snapshot.lifecycle == BotLifecycle::Alive});
    roster_.Remove(snapshot.id);
    bots_.erase(found);
}

void BotManager::StepFixed() {
    simulationTime_ += config_.fixedStepSeconds;

    std::vector<std::uint32_t> botIds;
    botIds.reserve(bots_.size());
    for (const auto& entry : bots_) {
        botIds.push_back(entry.first);
    }

    for (std::uint32_t id : botIds) {
        const auto it = bots_.find(id);
        if (it != bots_.end() &&
            it->second.snapshot.lifecycle == BotLifecycle::RespawnQueued) {
            AttemptRespawn(it->second);
        }
    }

    for (std::uint32_t id : botIds) {
        const auto it = bots_.find(id);
        if (it != bots_.end() && it->second.snapshot.lifecycle == BotLifecycle::Alive) {
            MoveBot(it->second, config_.fixedStepSeconds);
        }
    }

    RunBuiltInCombat();

    if (combatResolver_) {
        const std::vector<BotCombatEvent> proposed =
            combatResolver_(GetBots(), config_.fixedStepSeconds);
        for (const BotCombatEvent& event : proposed) {
            ApplyCombatEvent(event);
        }
    }
}

void BotManager::RunBuiltInCombat() {
    const bool enabled = config_.combatRangeUu > 0.0f &&
                         config_.combatDamage > 0.0f &&
                         config_.combatRoundsPerMinute > 0.0f;
    if (!enabled) return;

    struct TargetCandidate {
        ParticipantId id;
        Vector3 position;
        float distance = 0.0f;
    };

    // Target selection and firing eligibility belong to one immutable volley.
    // Applying damage while walking bots_ gives lower raw bot ids combat
    // initiative: a lethal team-one shot can remove a ready team-two attacker
    // before its turn in the same fixed step. Plan every ready shot first, then
    // resolve bot damage after the complete start-of-volley view was consumed.
    std::vector<BotCombatEvent> plannedBotHits;
    plannedBotHits.reserve(bots_.size());

    const float cadenceSeconds = 60.0f / config_.combatRoundsPerMinute;
    std::array<std::size_t, 3> readyAttackerOrdinals{{0, 0, 0}};
    for (auto& [rawAttackerId, attacker] : bots_) {
        (void)rawAttackerId;
        if (attacker.snapshot.lifecycle != BotLifecycle::Alive ||
            simulationTime_ + kTimeEpsilon < attacker.nextCombatShotAt) {
            continue;
        }

        std::vector<TargetCandidate> candidates;
        candidates.reserve(bots_.size() + externalHumans_.size());
        float nearestDistance = config_.combatRangeUu;
        const auto consider = [&](const ParticipantId& candidateId,
                                  const Vector3& candidatePosition) {
            const float distance =
                attacker.snapshot.position.Distance(candidatePosition);
            if (!std::isfinite(distance) ||
                distance > config_.combatRangeUu) {
                return;
            }
            nearestDistance = std::min(nearestDistance, distance);
            candidates.push_back(TargetCandidate{
                candidateId, candidatePosition, distance});
        };

        for (const auto& [rawCandidateId, candidate] : bots_) {
            (void)rawCandidateId;
            if (candidate.snapshot.lifecycle != BotLifecycle::Alive ||
                candidate.snapshot.teamId == attacker.snapshot.teamId) {
                continue;
            }
            consider(candidate.snapshot.id, candidate.snapshot.position);
        }
        if (externalHumansFresh_) {
            for (const BotExternalHumanSnapshot& human : externalHumans_) {
                if (!human.alive ||
                    human.teamId == attacker.snapshot.teamId) {
                    continue;
                }
                consider(human.id, human.position);
            }
        }

        if (candidates.empty()) continue;

        candidates.erase(
            std::remove_if(candidates.begin(), candidates.end(),
                           [nearestDistance](const TargetCandidate& candidate) {
                               return candidate.distance - nearestDistance >
                                   kTargetDistanceTieEpsilon;
                           }),
            candidates.end());
        if (candidates.empty()) continue;

        std::sort(candidates.begin(), candidates.end(),
                  [](const TargetCandidate& left,
                     const TargetCandidate& right) {
                      return left.id < right.id;
                  });
        // Use this frame's team-local ready-attacker ordinal, not raw bot ids.
        // Human fill churn makes bot ids sparse (for example 1..7,17); raw-id
        // modulo would then double-target one victim and leave another ignored.
        const std::size_t attackerOrdinal =
            readyAttackerOrdinals[attacker.snapshot.teamId]++;
        std::size_t targetIndex = attackerOrdinal % candidates.size();
        const bool coordinatedTerritoryAssault =
            attacker.snapshot.teamId == territoryAttackingTeam_ &&
            std::all_of(candidates.begin(), candidates.end(),
                        [](const TargetCandidate& candidate) {
                            return candidate.id.IsBot();
                        });
        if (coordinatedTerritoryAssault) {
            // Group assault fire into one full-health damage budget before
            // rolling to the next nearest-cohort target. When the ready squad
            // is smaller than that budget, the whole squad remains committed
            // across volleys. This changes neither per-shot damage nor capture
            // strength; defenders retain one-target-per-ready-ordinal spread.
            const double fullHealthRounds = std::ceil(
                static_cast<double>(config_.maxHealth) /
                static_cast<double>(config_.combatDamage));
            const double sizeLimit = static_cast<double>(
                std::numeric_limits<std::size_t>::max());
            const std::size_t roundsPerTarget = fullHealthRounds >= sizeLimit
                ? std::numeric_limits<std::size_t>::max()
                : std::max<std::size_t>(
                      1, static_cast<std::size_t>(fullHealthRounds));
            targetIndex =
                (attackerOrdinal / roundsPerTarget) % candidates.size();
        }
        const TargetCandidate& target =
            candidates[targetIndex];

        bool fired = false;
        if (target.id.IsBot()) {
            plannedBotHits.push_back(BotCombatEvent{
                attacker.snapshot.id, target.id, config_.combatDamage});
            fired = true;
        } else if (target.id.IsHuman() && nextHumanShotSequence_ != 0) {
            BotHumanCombatEvent event;
            event.generation = config_.humanCombatGeneration;
            event.shotSequence = nextHumanShotSequence_++;
            event.attackerId = attacker.snapshot.id;
            event.victimId = target.id;
            event.attackerTeamId = attacker.snapshot.teamId;
            event.origin = attacker.snapshot.position;
            event.impact = target.position;
            event.distanceUu = target.distance;
            event.damage = config_.combatDamage;
            const auto inserted =
                pendingHumanCombat_.emplace(event.shotSequence, event);
            if (inserted.second) {
                humanCombatEvents_.push_back(event);
                fired = true;
            }
        }

        if (fired) {
            attacker.nextCombatShotAt = simulationTime_ + cadenceSeconds;
        }
    }

    struct VolleyDamage {
        double totalDamage = 0.0;
        float startingHealth = 0.0f;
        ParticipantId creditedAttacker;
        bool lethalContributorChosen = false;
    };
    std::map<std::uint32_t, VolleyDamage> damageByVictim;
    for (const BotCombatEvent& event : plannedBotHits) {
        // Every plan was validated against the immutable alive roster above.
        // Retain one combat event per fired round (including overkill), while
        // collapsing health/death mutation to one transaction per victim.
        const auto victim = bots_.find(event.victimId.value);
        if (victim == bots_.end() ||
            victim->second.snapshot.lifecycle != BotLifecycle::Alive) {
            continue;
        }
        combatEvents_.push_back(event);

        auto [damageIt, inserted] = damageByVictim.try_emplace(event.victimId.value);
        VolleyDamage& damage = damageIt->second;
        if (inserted) {
            damage.startingHealth = victim->second.snapshot.health;
            damage.creditedAttacker = event.attackerId;
        }
        damage.totalDamage += static_cast<double>(event.damage);
        if (!damage.lethalContributorChosen &&
            damage.totalDamage >= static_cast<double>(damage.startingHealth)) {
            // Simultaneous overkill has no unique physical final hit. Preserve
            // deterministic attacker-order credit without allowing that order
            // to suppress any other planned shot.
            damage.creditedAttacker = event.attackerId;
            damage.lethalContributorChosen = true;
        }
    }

    // Commit every lethal participant state before publishing any synchronous
    // death callback. Game-mode callbacks query the unified live roster to
    // resolve sudden-death/mutual-elimination outcomes; notifying after the
    // first victim while another same-volley victim is still Alive turns a
    // draw into an id-order-dependent win. KillBot still owns the individual
    // death transaction/event, but its callback is replayed only after the
    // complete volley state is visible.
    const std::size_t firstVolleyDeathEvent = deathEvents_.size();
    DeathCallback deferredDeathCallback = std::move(deathCallback_);
    DeathBatchCallback deferredDeathBatchCallback =
        std::move(deathBatchCallback_);
    for (const auto& [rawVictimId, damage] : damageByVictim) {
        const auto victim = bots_.find(rawVictimId);
        if (victim == bots_.end() ||
            victim->second.snapshot.lifecycle != BotLifecycle::Alive) {
            continue;
        }

        BotAgent& bot = victim->second;
        const double remaining = std::max(
            0.0, static_cast<double>(bot.snapshot.health) - damage.totalDamage);
        if (remaining <= 0.0) {
            KillBot(bot.snapshot.id, damage.creditedAttacker, true);
        } else {
            bot.snapshot.health = static_cast<float>(remaining);
            SyncRoster(bot);
        }
    }

    deathCallback_ = std::move(deferredDeathCallback);
    deathBatchCallback_ = std::move(deferredDeathBatchCallback);
    if ((deathCallback_ || deathBatchCallback_) &&
        deathEvents_.size() > firstVolleyDeathEvent) {
        const std::vector<BotDeathEvent> volleyDeaths(
            deathEvents_.begin() +
                static_cast<std::ptrdiff_t>(firstVolleyDeathEvent),
            deathEvents_.end());
        // Snapshot the callback as well as the events. A callback is public
        // extension code and may consume event queues or replace itself; neither
        // action may invalidate this volley's remaining notifications.
        const DeathCallback callback = deathCallback_;
        const DeathBatchCallback batchCallback = deathBatchCallback_;
        if (callback) {
            for (const BotDeathEvent& death : volleyDeaths) {
                callback(death);
            }
        }
        if (batchCallback) {
            batchCallback(volleyDeaths);
        }
    }
}

bool BotManager::AttemptRespawn(BotAgent& bot) {
    if (bot.snapshot.lifecycle != BotLifecycle::RespawnQueued ||
        !teamRespawnAllowed_[bot.snapshot.teamId] ||
        simulationTime_ + kTimeEpsilon < bot.respawnAt) {
        return false;
    }

    const BotSpawnSnapshot* spawn = SelectSpawn(bot);
    if (spawn == nullptr) {
        return false;
    }

    const bool initialSpawn = !bot.hasSpawned;
    bot.snapshot.lifecycle = BotLifecycle::Alive;
    bot.snapshot.position = spawn->position;
    bot.snapshot.health = bot.snapshot.maxHealth;
    bot.snapshot.objectiveId = 0;
    bot.route = BotRoute{};
    bot.routeCursor = 0;
    bot.routeObjectiveId = 0;
    bot.routeUsesTransit = false;
    bot.nextCombatShotAt = simulationTime_;
    bot.hasSpawned = true;
    ++bot.spawnSequence;
    SyncRoster(bot);

    const BotRespawnEvent respawnEvent{bot.snapshot.id, bot.snapshot.teamId, spawn->id,
                                       spawn->position, initialSpawn};
    respawnEvents_.push_back(respawnEvent);
    if (respawnCallback_) {
        respawnCallback_(respawnEvent);
    }
    return true;
}

const BotSpawnSnapshot* BotManager::SelectSpawn(const BotAgent& bot) const {
    std::size_t teamSpawnCount = 0;
    for (const BotSpawnSnapshot& spawn : spawns_) {
        if (spawn.teamId == bot.snapshot.teamId) {
            ++teamSpawnCount;
        }
    }
    if (teamSpawnCount == 0) {
        return nullptr;
    }

    const std::uint64_t stableIndex =
        (static_cast<std::uint64_t>(GetTeamLocalOrdinal(bot)) + bot.spawnSequence) %
        teamSpawnCount;
    std::size_t teamIndex = 0;
    for (const BotSpawnSnapshot& spawn : spawns_) {
        if (spawn.teamId != bot.snapshot.teamId) {
            continue;
        }
        if (teamIndex == stableIndex) {
            return &spawn;
        }
        ++teamIndex;
    }
    return nullptr;
}

const BotObjectiveSnapshot* BotManager::SelectObjective(const BotAgent& bot) const {
    if (objectives_.empty()) {
        return nullptr;
    }
    if (bot.snapshot.teamId == territoryAttackingTeam_) {
        // Territory assault squads take grouped objectives serially. Splitting
        // both teams by the same modulo produces an exact 4v4/4v4 equilibrium
        // on phases such as Resort Villa/Farm and can never make progress.
        return &objectives_.front();
    }

    // Defenders cover simultaneous objectives evenly. Use a current team-local
    // ordinal instead of the raw participant id so normal human-fill removal
    // and refill (which leaves sparse bot ids) cannot skew every defender onto
    // one branch.
    const std::size_t index = GetTeamLocalOrdinal(bot) % objectives_.size();
    return &objectives_[index];
}

std::size_t BotManager::GetTeamLocalOrdinal(const BotAgent& bot) const {
    std::size_t ordinal = 0;
    for (const auto& entry : bots_) {
        const BotAgent& candidate = entry.second;
        if (candidate.snapshot.teamId != bot.snapshot.teamId) {
            continue;
        }
        if (candidate.snapshot.id == bot.snapshot.id) {
            return ordinal;
        }
        ++ordinal;
    }
    // BotAgent references passed by this class always belong to bots_. Keep a
    // deterministic fail-closed value if that ownership invariant is violated.
    return 0;
}

void BotManager::MoveBot(BotAgent& bot, float stepSeconds) {
    const BotObjectiveSnapshot* objective = SelectObjective(bot);
    if (objective == nullptr) {
        bot.snapshot.objectiveId = 0;
        bot.route = BotRoute{};
        bot.routeCursor = 0;
        bot.routeObjectiveId = 0;
        bot.routeUsesTransit = false;
        SyncRoster(bot);
        return;
    }

    bot.snapshot.objectiveId = objective->id;
    if (Distance2D(bot.snapshot.position, objective->position) <=
        config_.objectiveArrivalTolerance) {
        bot.snapshot.position = objective->position;
        SyncRoster(bot);
        return;
    }

    if (bot.routeObjectiveId != objective->id || !bot.route.IsValid() ||
        bot.routeCursor >= bot.route.waypoints.size()) {
        bot.route = navigation_.BuildRoute(bot.snapshot.position, objective->position,
                                           config_.allowDirectRouteFallback);
        bot.routeCursor = 0;
        bot.routeObjectiveId = objective->id;
        const float initialRouteDistance =
            bot.snapshot.position.Distance(objective->position);
        bot.routeUsesTransit =
            config_.transitMoveSpeed > 0.0f && bot.route.usedDirectFallback &&
            std::isfinite(initialRouteDistance) &&
            initialRouteDistance > config_.transitRouteDistanceThreshold;
    }

    if (bot.route.IsValid()) {
        float remainingStepSeconds = std::max(0.0f, stepSeconds);
        if (bot.routeUsesTransit) {
            const float remainingRouteDistance =
                bot.snapshot.position.Distance(objective->position);
            if (std::isfinite(remainingRouteDistance) &&
                remainingRouteDistance > config_.transitApproachDistance) {
                // Stop the fast leg exactly at the configured boundary. If it
                // is crossed within this fixed step, spend only the remaining
                // time at infantry speed rather than overshooting the approach.
                const float distanceToTransitBoundary =
                    remainingRouteDistance - config_.transitApproachDistance;
                const float transitBudget =
                    config_.transitMoveSpeed * remainingStepSeconds;
                const float transitDistance =
                    std::min(distanceToTransitBoundary, transitBudget);
                bot.snapshot.position = navigation_.AdvanceAlongRoute(
                    bot.snapshot.position, bot.route, bot.routeCursor,
                    transitDistance);
                remainingStepSeconds = std::max(
                    0.0f,
                    remainingStepSeconds -
                        transitDistance / config_.transitMoveSpeed);
            }
        }

        if (remainingStepSeconds > kTimeEpsilon) {
            const float infantryDistance =
                config_.moveSpeed * remainingStepSeconds;
            bot.snapshot.position = navigation_.AdvanceAlongRoute(
                bot.snapshot.position, bot.route, bot.routeCursor,
                infantryDistance);
        }
        if (Distance2D(bot.snapshot.position, objective->position) <=
            config_.objectiveArrivalTolerance) {
            bot.snapshot.position = objective->position;
        }
    }
    SyncRoster(bot);
}

void BotManager::SyncRoster(const BotAgent& bot) {
    roster_.Upsert(ParticipantSnapshot{
        bot.snapshot.id,
        bot.snapshot.teamId,
        bot.snapshot.lifecycle == BotLifecycle::Alive,
        bot.snapshot.position,
        bot.snapshot.captureWeight,
    });
}

bool BotManager::IsValidConfig(const BotManagerConfig& config) {
    const bool transitDisabled = config.transitMoveSpeed == 0.0f &&
                                 config.transitRouteDistanceThreshold == 0.0f &&
                                 config.transitApproachDistance == 0.0f;
    const bool transitEnabled = std::isfinite(config.transitMoveSpeed) &&
                                config.transitMoveSpeed > config.moveSpeed &&
                                config.transitMoveSpeed > 0.0f &&
                                config.allowDirectRouteFallback &&
                                std::isfinite(
                                    config.transitRouteDistanceThreshold) &&
                                std::isfinite(config.transitApproachDistance) &&
                                config.transitApproachDistance >
                                    config.objectiveArrivalTolerance &&
                                config.transitRouteDistanceThreshold >
                                    config.transitApproachDistance &&
                                config.transitRouteDistanceThreshold <
                                    config.navigation.maxDirectRouteLength;
    const bool combatDisabled = config.combatRangeUu == 0.0f &&
                                config.combatDamage == 0.0f &&
                                config.combatRoundsPerMinute == 0.0f;
    const bool combatEnabled = std::isfinite(config.combatRangeUu) &&
                               config.combatRangeUu > 0.0f &&
                               std::isfinite(config.combatDamage) &&
                               config.combatDamage > 0.0f &&
                               std::isfinite(config.combatRoundsPerMinute) &&
                               config.combatRoundsPerMinute > 0.0f;
    return config.fillTargetPerTeam <= config.maxBotsPerTeam &&
           std::isfinite(config.fixedStepSeconds) && config.fixedStepSeconds > 0.0f &&
           std::isfinite(config.moveSpeed) && config.moveSpeed >= 0.0f &&
           (transitDisabled || transitEnabled) &&
           (combatDisabled || combatEnabled) &&
           config.humanCombatGeneration != 0 &&
           std::isfinite(config.respawnDelaySeconds) &&
           config.respawnDelaySeconds >= 0.0f && std::isfinite(config.maxHealth) &&
           config.maxHealth > 0.0f && std::isfinite(config.captureWeight) &&
           config.captureWeight >= 0.0f &&
           std::isfinite(config.objectiveArrivalTolerance) &&
           config.objectiveArrivalTolerance >= 0.0f && config.maxCatchUpSteps > 0;
}

bool BotManager::IsFinite(const Vector3& position) {
    return std::isfinite(position.x) && std::isfinite(position.y) &&
           std::isfinite(position.z);
}

float BotManager::Distance2D(const Vector3& left, const Vector3& right) {
    const float x = left.x - right.x;
    const float y = left.y - right.y;
    return std::sqrt(x * x + y * y);
}
