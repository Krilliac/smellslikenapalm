// src/Game/ObjectiveSystem.h
// RS2V capture point / objective system with progressive territory control

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include "Math/Vector3.h"

class GameServer;

enum class ObjectiveType : uint8_t {
    CapturePoint,       // Standard capture zone
    Territory,          // Territory control (linear progression in Territory mode)
    DestroyTarget,      // Destructible objective
    HoldZone            // Area that must be held for time
};

enum class CaptureState : uint8_t {
    Neutral,            // No team controls
    Contested,          // Both teams present
    Capturing,          // One team capturing
    Controlled,         // Firmly controlled by a team
    Locked              // Cannot be captured (already passed in Territory mode)
};

struct CaptureZone {
    uint32_t id = 0;
    std::string name;
    ObjectiveType type = ObjectiveType::CapturePoint;
    Vector3 position;
    float captureRadius = 30.0f;        // meters
    uint32_t controllingTeam = 0;       // 0 = neutral
    CaptureState state = CaptureState::Neutral;

    // Capture progress: 0.0 = neutral, 1.0 = fully captured by attacking team
    float captureProgress = 0.0f;
    float captureSpeed = 0.10f;         // progress per second per player
    float decaySpeed = 0.05f;           // progress loss when no attackers present
    float contestDecaySpeed = 0.02f;    // slower decay when contested

    // Territory mode ordering
    int territoryOrder = 0;             // Order in the territory chain (0 = first)
    bool isActive = true;               // Can currently be captured

    // Players currently in the zone
    std::vector<uint32_t> attackerIds;
    std::vector<uint32_t> defenderIds;

    // Scoring
    uint32_t capturePoints = 200;       // Points awarded for capture
    uint32_t ticketPenalty = 30;        // Tickets lost by defenders on capture

    // Visual/gameplay flags
    bool hasTunnel = false;             // VC/NVA tunnel spawn at this objective
    Vector3 tunnelPosition;
};

using ObjectiveCapturedCallback = std::function<void(uint32_t objectiveId, uint32_t capturingTeam, uint32_t previousTeam)>;

class ObjectiveSystem {
public:
    explicit ObjectiveSystem(GameServer* server);
    ~ObjectiveSystem();

    void Initialize();
    void Shutdown();

    // Objective management
    uint32_t AddObjective(const CaptureZone& zone);
    void RemoveObjective(uint32_t objectiveId);
    CaptureZone* GetObjective(uint32_t id);
    const CaptureZone* GetObjective(uint32_t id) const;
    std::vector<const CaptureZone*> GetAllObjectives() const;
    std::vector<const CaptureZone*> GetActiveObjectives() const;

    // Territory mode: set linear capture order
    void SetTerritoryOrder(const std::vector<uint32_t>& objectiveIds);
    void ActivateNextTerritory(uint32_t capturingTeamDirection);
    const CaptureZone* GetCurrentTerritoryObjective() const;

    // Per-tick update: process captures, contests, decay
    void Update(float deltaSeconds);

    // Player zone tracking
    void OnPlayerEnterZone(uint32_t playerId, uint32_t objectiveId);
    void OnPlayerLeaveZone(uint32_t playerId, uint32_t objectiveId);
    void RefreshPlayerZones();  // Recalculate from player positions

    // Query
    uint32_t GetObjectiveCount() const;
    uint32_t GetTeamObjectiveCount(uint32_t teamId) const;
    bool AreAllObjectivesCapturedBy(uint32_t teamId) const;

    // Events
    void SetOnObjectiveCaptured(ObjectiveCapturedCallback cb);

    // Broadcast objective state to all clients
    void BroadcastObjectiveStates() const;

private:
    GameServer* m_server;
    std::map<uint32_t, CaptureZone> m_objectives;
    uint32_t m_nextObjectiveId = 1;
    ObjectiveCapturedCallback m_capturedCallback;

    // Territory mode state
    std::vector<uint32_t> m_territoryOrder;
    int m_currentTerritoryIndex = 0;

    void ProcessCapture(CaptureZone& zone, float deltaSeconds);
    void OnObjectiveCaptured(CaptureZone& zone, uint32_t newTeam);
    bool IsPlayerInZone(uint32_t playerId, const CaptureZone& zone) const;
};
