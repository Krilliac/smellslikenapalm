// src/Game/SpawnSystem.h
// RS2V spawn system — squad spawning, tunnel spawning, deployment waves

#pragma once

#include <cstdint>
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

    // Squad leader specific
    uint32_t squadLeaderId = 0;

    // Tunnel specific
    uint32_t objectiveId = 0;       // Associated objective
    int tunnelHealth = 100;         // Can be destroyed

    // Timing
    float spawnCooldown = 0.0f;     // Seconds until spawnable again
};

class SpawnSystem {
public:
    explicit SpawnSystem(GameServer* server);
    ~SpawnSystem();

    void Initialize();
    void Shutdown();

    // Spawn location management
    uint32_t AddSpawnLocation(const SpawnLocation& loc);
    void RemoveSpawnLocation(uint32_t id);
    SpawnLocation* GetSpawnLocation(uint32_t id);
    std::vector<const SpawnLocation*> GetAvailableSpawns(uint32_t playerId) const;
    std::vector<const SpawnLocation*> GetTeamSpawns(uint32_t teamId) const;

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

    bool IsSquadLeaderInCombat(uint32_t leaderId) const;
    Vector3 GetSpawnOffset(const SpawnLocation& loc) const;
};
