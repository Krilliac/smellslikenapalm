// src/Game/SpawnSystem.h
// RS2V spawn system — squad spawning, tunnel spawning, deployment waves

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include "Math/Vector3.h"

class GameServer;

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

    explicit SpawnSystem(GameServer* server,
                         AccessContextResolver accessContextResolver = {},
                         SquadLeaderEligibilityResolver
                             squadLeaderEligibilityResolver = {});
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

    // Spawn a player at a chosen location
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
    AccessContextResolver m_accessContextResolver;
    SquadLeaderEligibilityResolver m_squadLeaderEligibilityResolver;
};
