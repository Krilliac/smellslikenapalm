#pragma once

#include "Game/BotNavigation.h"
#include "Game/ParticipantRoster.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

enum class BotLifecycle : std::uint8_t {
    Alive = 0,
    Dead = 1,
    RespawnQueued = 2,
};

struct BotManagerConfig {
    std::size_t fillTargetPerTeam = 0;
    std::size_t maxBotsPerTeam = 64;
    float fixedStepSeconds = 0.1f;
    float moveSpeed = 5.0f;
    // Optional fast traversal for unusually long, direct-fallback routes (for
    // example, an offshore insertion leg). All three values must be zero to
    // disable the feature. A newly built direct route qualifies only when its
    // initial length exceeds transitRouteDistanceThreshold. Once qualified,
    // transitMoveSpeed is used until transitApproachDistance remains; crossing
    // that boundary consumes the rest of the fixed step at ordinary moveSpeed
    // so the fast leg can never carry a bot through its near-objective approach.
    float transitMoveSpeed = 0.0f;
    float transitRouteDistanceThreshold = 0.0f;
    float transitApproachDistance = 0.0f;
    float respawnDelaySeconds = 5.0f;
    float maxHealth = 100.0f;
    float captureWeight = 1.0f;
    float objectiveArrivalTolerance = 0.1f;
    std::size_t maxCatchUpSteps = 8;
    bool allowDirectRouteFallback = true;
    // Optional deterministic rifle combat. All three values must be zero to
    // disable it, or finite and positive to enable it. Range is expressed in
    // Unreal units to match bot/world positions; damage is the unscaled amount
    // submitted to CombatAuthority for the selected participant.
    float combatRangeUu = 0.0f;
    float combatDamage = 0.0f;
    float combatRoundsPerMinute = 0.0f;
    // Nonzero world/map incarnation copied into every server-issued human
    // combat event. Bot ids and shot sequences restart with a new BotManager;
    // this token prevents a delayed event from aliasing a new map's pending
    // shot with otherwise identical fields.
    std::uint64_t humanCombatGeneration = 1;
    BotNavigationConfig navigation;
    BotNavigationGraph navigationGraph;
};

struct BotSpawnSnapshot {
    std::uint32_t id = 0;
    std::uint8_t teamId = 0;
    Vector3 position;
    bool active = true;
    bool destroyed = false;
    float cooldownRemaining = 0.0f;
};

struct BotObjectiveSnapshot {
    std::uint32_t id = 0;
    Vector3 position;
    float captureRadius = 1.0f;
    bool active = true;
    bool enabled = true;
};

struct BotSnapshot {
    ParticipantId id;
    std::string name;
    std::uint8_t teamId = 0;
    BotLifecycle lifecycle = BotLifecycle::Dead;
    Vector3 position;
    float health = 0.0f;
    float maxHealth = 0.0f;
    float captureWeight = 0.0f;
    std::uint32_t objectiveId = 0;
    std::uint32_t deathSequence = 0;
    std::uint32_t kills = 0;
    std::uint32_t score = 0;
};

// One frame's authoritative human view supplied by GameServer. Human health
// remains owned by CombatAuthority/PlayerManager; BotManager uses this only for
// deterministic perception and target selection.
struct BotExternalHumanSnapshot {
    ParticipantId id;
    std::uint8_t teamId = 0;
    Vector3 position;
    bool alive = false;
};

struct BotHumanCombatEvent {
    std::uint64_t generation = 0;
    std::uint64_t shotSequence = 0;
    ParticipantId attackerId;
    ParticipantId victimId;
    std::uint8_t attackerTeamId = 0;
    Vector3 origin;
    Vector3 impact;
    float distanceUu = 0.0f;
    float damage = 0.0f;
};

enum class BotHumanCombatOutcome : std::uint8_t {
    Rejected = 0,
    DamageApplied = 1,
    TargetKilled = 2,
};

struct ObjectiveOccupantSnapshot {
    std::uint32_t objectiveId = 0;
    ParticipantId participantId;
    std::uint8_t teamId = 0;
    float captureWeight = 0.0f;
};

struct BotCombatEvent {
    ParticipantId attackerId;
    ParticipantId victimId;
    float damage = 0.0f;
};

// Relationship decision made by the authoritative caller before applying
// damage originating outside BotManager's bot-vs-bot combat resolver. Human
// team/liveness state is intentionally owned by GameServer, so there is no
// permissive default: callers must explicitly classify every external hit.
enum class ExternalBotDamageAuthorization : std::uint8_t {
    Hostile = 0,
    FriendlyFireBlocked = 1,
    FriendlyFireAuthorized = 2,
    SelfDamageAuthorized = 3,
};

struct BotDeathEvent {
    ParticipantId victimId;
    ParticipantId killerId;
    std::uint8_t teamId = 0;
    std::uint32_t deathSequence = 0;
};

struct BotRespawnEvent {
    ParticipantId botId;
    std::uint8_t teamId = 0;
    std::uint32_t spawnId = 0;
    Vector3 position;
    bool initialSpawn = false;
};

// Fill reconciliation removes an actor identity rather than killing it.  Keep
// that lifecycle distinct so GameServer can retire every viewer-local PRI/pawn
// pair without debiting tickets or manufacturing a combat death.
struct BotRemovalEvent {
    ParticipantId botId;
    std::uint8_t teamId = 0;
    bool wasAlive = false;
};

struct BotManagerTestAccess;

class BotManager {
public:
    static constexpr std::uint8_t kTeamOne = 1;
    static constexpr std::uint8_t kTeamTwo = 2;
    static constexpr std::size_t kMaxExternalHumans = 128;

    using CombatResolver =
        std::function<std::vector<BotCombatEvent>(const std::vector<BotSnapshot>&, float)>;
    using DeathCallback = std::function<void(const BotDeathEvent&)>;
    // Publishes one immutable simulation transaction. Built-in simultaneous
    // combat emits one vector after every victim state has committed; direct
    // and externally authorized deaths emit a one-element vector.
    using DeathBatchCallback =
        std::function<void(const std::vector<BotDeathEvent>&)>;
    using RespawnCallback = std::function<void(const BotRespawnEvent&)>;

    explicit BotManager(const BotManagerConfig& config = BotManagerConfig{});

    bool Configure(const BotManagerConfig& config);
    const BotManagerConfig& GetConfig() const { return config_; }

    void SetEligibleSpawns(const std::vector<BotSpawnSnapshot>& spawns);
    void SetActiveObjectives(const std::vector<BotObjectiveSnapshot>& objectives);

    void SetHumanTeamCounts(std::size_t teamOneHumans, std::size_t teamTwoHumans);
    bool SetHumanTeamCount(std::uint8_t teamId, std::size_t humans);
    // Updates the desired total participants per side and reconciles
    // immediately. Targets above the configured bot limit are rejected so the
    // public API preserves the same bound as Configure().
    bool SetFillTargetPerTeam(std::size_t target);
    void ReconcileFill();

    // Replaces the complete human combat view for exactly the next Update().
    // A malformed/duplicate/oversized view is rejected atomically and clears
    // any previous view so stale participants can never remain targetable.
    bool SetExternalHumanCombatants(
        const std::vector<BotExternalHumanSnapshot>& humans);

    bool SetTeamRespawnAllowed(std::uint8_t teamId, bool allowed);

    // Enables role-aware Territory tactics for one playable attacking team.
    // Passing 0 clears the context (used by non-Territory modes). Attackers
    // concentrate the first active objective and coordinate otherwise-equal
    // nearest-target volleys; defenders continue to spread across simultaneous
    // objectives. This changes movement/target choice only--capture weight and
    // ObjectiveSystem's true-tie semantics remain authoritative.
    bool SetTerritoryAttackingTeam(std::uint8_t teamId);

    // Cooked Territory spawn ownership describes attacker/defender roles for
    // round one (authored team 1 attacks). Resolve those roles to the actual
    // round's team without mutating SpawnSystem's retail source records.
    static std::uint8_t RemapTerritorySpawnTeam(
        std::uint8_t authoredTeamId, std::uint8_t attackingTeamId);

    // Begin a new round without manufacturing combat deaths. Every bot is
    // removed from the live roster, its route/health/objective state is reset,
    // and it is queued for an immediate round spawn. Teams whose current
    // respawn gate is open are repositioned using the current eligible spawn
    // set after every bot has been reset; closed teams remain queued until a
    // later SetTeamRespawnAllowed(..., true) or Update(). No death callback or
    // death/combat event is emitted, and stable participant/death ids survive.
    void RedeployForRound();

    // A newly activated Territory phase has a different authored deployment
    // view. Reposition every bot through the same non-death transaction used at
    // round start so living squads do not leave the prior objective together
    // and ignore the new attacker/defender spawns.
    void RedeployForTerritoryPhase();

    // Accumulates real time and advances deterministic fixed-size simulation
    // steps. Non-finite or negative time is rejected.
    bool Update(float deltaSeconds);

    bool ApplyCombatEvent(const BotCombatEvent& event);
    bool ApplyExternalDamageToBot(
        const ParticipantId& victimId, const ParticipantId& attackerId, float damage,
        ExternalBotDamageAuthorization authorization);
    bool KillBot(const ParticipantId& botId,
                 const ParticipantId& killerId = ParticipantId{},
                 bool scheduleRespawn = true);
    bool QueueRespawn(const ParticipantId& botId);

    const BotSnapshot* FindBot(const ParticipantId& botId) const;
    std::vector<BotSnapshot> GetBots() const;
    std::size_t CountBots(std::uint8_t teamId) const;
    std::size_t CountAliveBots(std::uint8_t teamId) const;

    std::vector<ObjectiveOccupantSnapshot> GetObjectiveOccupants() const;
    float GetObjectiveCaptureWeight(std::uint32_t objectiveId,
                                    std::uint8_t teamId) const;

    void SetCombatResolver(CombatResolver resolver) { combatResolver_ = std::move(resolver); }
    void SetDeathCallback(DeathCallback callback) { deathCallback_ = std::move(callback); }
    void SetDeathBatchCallback(DeathBatchCallback callback) {
        deathBatchCallback_ = std::move(callback);
    }
    void SetRespawnCallback(RespawnCallback callback) {
        respawnCallback_ = std::move(callback);
    }

    std::vector<BotCombatEvent> ConsumeCombatEvents();
    std::vector<BotHumanCombatEvent> ConsumeHumanCombatEvents();
    bool IsPendingHumanCombatEvent(
        const BotHumanCombatEvent& event) const;
    // Resolves a server-issued pending shot exactly once. Only TargetKilled
    // mutates bot statistics; forged, stale and duplicate resolutions fail.
    bool ResolveHumanCombatEvent(const BotHumanCombatEvent& event,
                                 BotHumanCombatOutcome outcome);
    std::vector<BotDeathEvent> ConsumeDeathEvents();
    std::vector<BotRespawnEvent> ConsumeRespawnEvents();
    std::vector<BotRemovalEvent> ConsumeRemovalEvents();

    BotNavigation& Navigation() { return navigation_; }
    const BotNavigation& Navigation() const { return navigation_; }
    const ParticipantRoster& Roster() const { return roster_; }

    double GetSimulationTime() const { return simulationTime_; }

private:
    friend struct BotManagerTestAccess;

    struct BotAgent {
        BotSnapshot snapshot;
        BotRoute route;
        std::size_t routeCursor = 0;
        std::uint32_t routeObjectiveId = 0;
        bool routeUsesTransit = false;
        std::uint32_t spawnSequence = 0;
        double respawnAt = 0.0;
        bool hasSpawned = false;
        double nextCombatShotAt = 0.0;
    };

    bool CreateBot(std::uint8_t teamId);
    void RemoveBot(std::uint32_t rawBotId);
    void RedeployAll(bool resetAccumulator);
    void StepFixed();
    bool AttemptRespawn(BotAgent& bot);
    const BotSpawnSnapshot* SelectSpawn(const BotAgent& bot) const;
    const BotObjectiveSnapshot* SelectObjective(const BotAgent& bot) const;
    std::size_t GetTeamLocalOrdinal(const BotAgent& bot) const;
    void MoveBot(BotAgent& bot, float stepSeconds);
    void RunBuiltInCombat();
    void SyncRoster(const BotAgent& bot);
    bool ApplyValidatedDamage(BotAgent& victim, const ParticipantId& attackerId,
                              float damage);

    static bool IsValidConfig(const BotManagerConfig& config);
    static bool IsFinite(const Vector3& position);
    static float Distance2D(const Vector3& left, const Vector3& right);

    BotManagerConfig config_;
    BotNavigation navigation_;
    ParticipantRoster roster_;
    std::map<std::uint32_t, BotAgent> bots_;
    std::vector<BotSpawnSnapshot> spawns_;
    std::vector<BotObjectiveSnapshot> objectives_;
    std::array<std::size_t, 3> humanTeamCounts_{{0, 0, 0}};
    std::array<bool, 3> teamRespawnAllowed_{{false, true, true}};
    std::uint8_t territoryAttackingTeam_ = 0;
    std::uint32_t nextBotId_ = 1;
    double simulationTime_ = 0.0;
    float accumulator_ = 0.0f;

    CombatResolver combatResolver_;
    DeathCallback deathCallback_;
    DeathBatchCallback deathBatchCallback_;
    RespawnCallback respawnCallback_;
    std::vector<BotCombatEvent> combatEvents_;
    std::vector<BotExternalHumanSnapshot> externalHumans_;
    bool externalHumansFresh_ = false;
    std::vector<BotHumanCombatEvent> humanCombatEvents_;
    std::map<std::uint64_t, BotHumanCombatEvent> pendingHumanCombat_;
    std::uint64_t nextHumanShotSequence_ = 1;
    std::vector<BotDeathEvent> deathEvents_;
    std::vector<BotRespawnEvent> respawnEvents_;
    std::vector<BotRemovalEvent> removalEvents_;
};
