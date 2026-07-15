// src/Game/SpawnSystem.h
// RS2V spawn system — squad spawning, tunnel spawning, deployment waves

#pragma once

#include <cstdint>
#include <memory>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include "Math/Vector3.h"

class GameServer;
class Player;
class PlayerManager;

enum class SpawnType : uint8_t {
    BaseSpawn,          // Team base spawn (always available)
    SquadLeader,        // Spawn on squad leader (if alive and not in combat)
    Tunnel,             // VC/NVA tunnel spawn (if controlled)
    Helicopter,         // US spawn from helicopter insertion
    ForwardBase,        // FOB / forward operating base
    DeploymentWave      // Timed wave respawn
};

struct SpawnLocation {
    uint32_t id = 0;
    SpawnType type;
    std::string name;
    Vector3 position;
    Vector3 rotation;
    uint32_t teamId = 0;
    bool isActive = true;
    bool isDestroyed = false;

    // Inclusive objective-phase bounds for Territory maps. A negative bound
    // is unbounded. These constraints are ignored by non-Territory modes.
    int minTerritoryPhase = -1;
    int maxTerritoryPhase = -1;

    // Canonical static PackageMap reference to the cooked
    // ROVolumePlayerStartGroup used by the retail spawn-selection map. Network
    // code applies the frozen artifact's map-object offset on the wire. Zero
    // means unavailable/unmapped.
    uint32_t retailSpawnVolumeRef = 0;

    bool HasTerritoryPhaseBounds() const noexcept {
        return minTerritoryPhase >= 0 || maxTerritoryPhase >= 0;
    }

    bool IsAvailableInTerritoryPhase(int phase) const noexcept {
        if (phase < 0) return false;
        if (minTerritoryPhase >= 0 && phase < minTerritoryPhase) return false;
        if (maxTerritoryPhase >= 0 && phase > maxTerritoryPhase) return false;
        return true;
    }

    // Squad leader specific
    uint32_t squadLeaderId = 0;

    // Tunnel specific
    uint32_t objectiveId = 0;       // Associated objective
    int tunnelHealth = 100;         // Can be destroyed

    // Timing
    float spawnCooldown = 0.0f;     // Seconds until spawnable again
};

struct SpawnAccessContext {
    uint32_t playerTeam = 0;
    bool territoryActive = false;
    uint32_t attackingTeam = 0;
    uint32_t defendingTeam = 0;
    int territoryPhase = -1;
};

class SpawnSystem {
public:
    using AccessContextResolver =
        std::function<std::optional<SpawnAccessContext>(uint32_t playerId)>;
    using SquadLeaderEligibilityResolver =
        std::function<bool(uint32_t playerId, const SpawnLocation& location)>;

    // Immutable value token for the gap between authorizing a spawn and
    // publishing the corresponding owning actor graph. Copies and moves retain
    // the exact player identity, access state, selected location, and one
    // precomputed spawn transform captured by PreparePlayerSpawn. Copies are
    // safe to retain, but the authoritative Dead -> Alive transition makes
    // the operation one-shot: every replay fails lifecycle validation.
    class PreparedPlayerSpawn {
    public:
        PreparedPlayerSpawn(const PreparedPlayerSpawn&) = default;
        PreparedPlayerSpawn(PreparedPlayerSpawn&&) noexcept = default;
        PreparedPlayerSpawn& operator=(const PreparedPlayerSpawn&) = default;
        PreparedPlayerSpawn& operator=(PreparedPlayerSpawn&&) noexcept = default;
        ~PreparedPlayerSpawn() = default;

        uint32_t GetPlayerId() const noexcept { return m_playerId; }
        uint32_t GetSpawnLocationId() const noexcept {
            return m_spawnLocationId;
        }
        const Vector3& GetPosition() const noexcept { return m_position; }
        const Vector3& GetRotation() const noexcept { return m_rotation; }
        SpawnType GetSpawnType() const noexcept { return m_location.type; }

    private:
        friend class SpawnSystem;

        PreparedPlayerSpawn(
            const SpawnSystem* owner, uint32_t playerId,
            uint32_t spawnLocationId, std::weak_ptr<Player> playerIdentity,
            SpawnAccessContext access, SpawnLocation location,
            Vector3 position, uint64_t playerLifecycleGeneration) noexcept;

        const SpawnSystem* m_owner = nullptr;
        uint32_t m_playerId = 0;
        uint32_t m_spawnLocationId = 0;
        std::weak_ptr<Player> m_playerIdentity;
        SpawnAccessContext m_access;
        SpawnLocation m_location;
        Vector3 m_position;
        Vector3 m_rotation;
        uint64_t m_playerLifecycleGeneration = 0;
    };

    explicit SpawnSystem(GameServer* server,
                         AccessContextResolver accessContextResolver = {},
                         SquadLeaderEligibilityResolver
                             squadLeaderEligibilityResolver = {},
                         // Explicit dependency-injection seam for detached
                         // subsystem hosts. Production leaves this null and
                         // resolves PlayerManager through GameServer.
                         PlayerManager* playerManagerOverride = nullptr);
    ~SpawnSystem();

    void Initialize();
    void Shutdown();

    // Spawn location management
    uint32_t AddSpawnLocation(const SpawnLocation& loc);
    void RemoveSpawnLocation(uint32_t id);
    SpawnLocation* GetSpawnLocation(uint32_t id);
    std::vector<const SpawnLocation*> GetAvailableSpawns(uint32_t playerId) const;
    std::vector<const SpawnLocation*> GetTeamSpawns(uint32_t teamId) const;
    // Exact read-only predicate used again by SpawnPlayer at commit time. A
    // deployment UI can retain an id, but a side/phase change invalidates it.
    bool CanPlayerSpawnAt(uint32_t playerId, uint32_t spawnLocationId) const;
    static bool IsSpawnEligibleForContext(
        const SpawnLocation& location,
        const SpawnAccessContext& context) noexcept;

    // Squad leader spawn availability
    void UpdateSquadLeaderSpawns();
    bool CanSpawnOnSquadLeader(uint32_t playerId) const;

    // Tunnel management
    uint32_t CreateTunnel(uint32_t teamId, const Vector3& position, uint32_t objectiveId = 0);
    void DestroyTunnel(uint32_t tunnelId);
    bool IsTunnelActive(uint32_t tunnelId) const;

    // Wave spawning
    void StartSpawnWave(uint32_t teamId);
    bool IsInSpawnWave(uint32_t teamId) const;
    float GetWaveTimeRemaining(uint32_t teamId) const;
    // Re-arm one active team's next deployment without changing the shared
    // cadence (used when Skirmish objective capture moves its retail schedule).
    void SetWaveTimeRemaining(uint32_t teamId, float seconds);

    // Prepare performs every fallible identity/access/location check and
    // chooses the random offset without mutating player, combat, or cooldown
    // state. Both phases require the authoritative player to remain Dead;
    // commit revalidates every snapshot before applying the transform and
    // ordinary PlayerManager spawn transition.
    std::optional<PreparedPlayerSpawn> PreparePlayerSpawn(
        uint32_t playerId, uint32_t spawnLocationId) const;
    bool CommitPreparedPlayerSpawn(const PreparedPlayerSpawn& prepared);

    // Compatibility wrapper for callers that do not need a publication gap.
    bool SpawnPlayer(uint32_t playerId, uint32_t spawnLocationId);
    bool SpawnPlayerAtDefault(uint32_t playerId);

    // Per-tick update
    void Update(float deltaSeconds);

    // Configuration
    void SetWaveInterval(float seconds);
    void SetSquadSpawnCooldown(float seconds);
    void SetTunnelSpawnCooldown(float seconds);

private:
    GameServer* m_server;
    std::map<uint32_t, SpawnLocation> m_spawnLocations;
    uint32_t m_nextSpawnId = 1;

    // Wave spawn state per team
    struct WaveState {
        bool active = false;
        float timer = 0.0f;
        float interval = 20.0f;         // Seconds between waves
        std::vector<uint32_t> pendingPlayers;
    };
    std::map<uint32_t, WaveState> m_waveStates;

    // Config
    float m_waveInterval = 20.0f;
    float m_squadSpawnCooldown = 5.0f;
    float m_tunnelSpawnCooldown = 10.0f;

    std::optional<SpawnAccessContext> ResolveAccessContext(
        uint32_t playerId) const;
    bool IsSpawnAvailableToPlayer(
        uint32_t playerId, const SpawnLocation& location,
        const SpawnAccessContext& context) const;
    bool IsSpecificSquadLeaderAvailable(
        uint32_t playerId, const SpawnLocation& location,
        const SpawnAccessContext& context) const;
    bool IsSquadLeaderInCombat(uint32_t leaderId) const;
    Vector3 GetSpawnOffset(const SpawnLocation& loc) const;
    PlayerManager* ResolvePlayerManager() const;
    AccessContextResolver m_accessContextResolver;
    SquadLeaderEligibilityResolver m_squadLeaderEligibilityResolver;
    PlayerManager* m_playerManagerOverride = nullptr;
};
