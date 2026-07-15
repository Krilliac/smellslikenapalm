// src/Game/ObjectiveSystem.h
// RS2V capture point / objective system with progressive territory control

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
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

    // Retail ROGameReplicationInfo objective identity/state. ObjIndex is the
    // client's fixed 16-element GRI array slot; ObjRepIndex identifies the
    // cooked ROObjective actor already present in the loaded map. 255 means the
    // zone is server-only/unmapped (for example, a generic fallback template).
    uint8_t clientSlot = 0xFF;
    uint8_t cookedRepIndex = 0xFF;
    bool enabled = true;                 // GRI ObjectiveStatus enabled bit (0x10)
    bool connectedToBase = false;        // GRI ObjectiveConnectedToBase[slot]
    uint8_t cappingTeam = 0xFF;          // Retail team index 0/1; 255 = no capper

    // Cooked ROObjective lockdown metadata. The three times are the authored
    // LockDownTime16/32/64 values selected by retail for <=16, <=32, and <=64
    // players. They are distinct from ROGameInfoTerritories.LockdownTime, the
    // global duration of an early-win countdown. Unknown metadata must never
    // inherit the class-default enable bit implicitly: TerritoryMode treats it
    // as ineligible until a package-grounded map row supplies all four values.
    bool lockdownMetadataKnown = false;
    bool lockdownEnabled = false;
    std::array<int32_t, 3> lockdownTimeSecondsByPlayerBand{{0, 0, 0}};

    // Supremacy map topology. Links are encoded by the retail client's fixed
    // objective slots so authored data remains stable even if server ids are
    // regenerated while loading a map.
    uint8_t supremacyPointValue = 1;
    uint8_t supremacyHomeTeam = 0;        // 0 = not an HQ, 1/2 = team HQ
    uint16_t supremacyAdjacentSlots = 0;  // bit N links to client slot N

    // Players currently in the zone
    std::vector<uint32_t> attackerIds;
    std::vector<uint32_t> defenderIds;

    // Headless bot capture contribution, indexed by server team id (1/2).
    // Humans remain represented by the id vectors above. Keeping the two
    // sources separate lets retail HUD replication add bot strength without
    // inventing client-visible player ids for headless participants.
    std::array<float, 3> botCaptureWeightByTeam{{0.0f, 0.0f, 0.0f}};

    // Scoring
    uint32_t capturePoints = 200;       // Points awarded for capture
    uint32_t ticketPenalty = 30;        // Tickets lost by defenders on capture

    // Visual/gameplay flags
    bool hasTunnel = false;             // VC/NVA tunnel spawn at this objective
    Vector3 tunnelPosition;
};

using ObjectiveCapturedCallback = std::function<void(uint32_t objectiveId, uint32_t capturingTeam, uint32_t previousTeam)>;
using ObjectiveBotCaptureWeightProvider =
    std::function<float(uint32_t objectiveId, uint32_t serverTeamId)>;

class ObjectiveSystem {
public:
    // RO gameplay coordinates use 50 Unreal units per displayed meter. Map
    // objective radii are authored in meters, while pawn positions arrive in
    // Unreal units from ServerMove.
    static constexpr float kUnrealUnitsPerMeter = 50.0f;

    explicit ObjectiveSystem(GameServer* server);
    ~ObjectiveSystem();

    void Initialize();
    void Shutdown();

    // Objective management
    uint32_t AddObjective(const CaptureZone& zone);
    void RemoveObjective(uint32_t objectiveId);
    void Clear();   // Remove all objectives and reset territory ordering
    CaptureZone* GetObjective(uint32_t id);
    const CaptureZone* GetObjective(uint32_t id) const;
    std::vector<const CaptureZone*> GetAllObjectives() const;
    std::vector<const CaptureZone*> GetActiveObjectives() const;

    // Territory mode: set linear capture order
    void SetTerritoryOrder(const std::vector<uint32_t>& objectiveIds);
    // Reset ownership/progress/activation to the first objective phase for a
    // new Territory round. All objectives begin owned by the defending team.
    void ResetTerritoryForRound(uint32_t defendingTeam);
    // Restore the ownership authored when each non-Territory objective was
    // registered. Supremacy/Skirmish call this at every round start so a prior
    // round cannot leak captured owners or partial HUD state into preparation.
    void ResetObjectivesToInitialOwners();
    void ActivateNextTerritory(uint32_t capturingTeamDirection);
    const CaptureZone* GetCurrentTerritoryObjective() const;

    // Per-tick update: process captures, contests, decay. The two-delta form
    // preserves wall-time replication cadence while GameServer clamps capture
    // simulation at a native phase boundary.
    void Update(float deltaSeconds);
    void Update(float deltaSeconds, float captureDeltaSeconds);

    // Player zone tracking
    void OnPlayerEnterZone(uint32_t playerId, uint32_t objectiveId);
    void OnPlayerLeaveZone(uint32_t playerId, uint32_t objectiveId);
    void RefreshPlayerZones();  // Recalculate from player positions

    // Inject headless capture strength without coupling ObjectiveSystem to a
    // BotManager lifetime or header. Invalid/negative provider results are
    // ignored. Replacing/removing the provider clears all cached bot weights.
    void SetBotCaptureWeightProvider(ObjectiveBotCaptureWeightProvider provider);

    // Query
    // Gameplay objective count. Disabled cooked slots remain available through
    // GetAllObjectives for replication identity, but do not count here (notably
    // for Skirmish all-objective control).
    uint32_t GetObjectiveCount() const;
    uint32_t GetTeamObjectiveCount(uint32_t teamId) const;
    bool AreAllObjectivesCapturedBy(uint32_t teamId) const;
    float GetBotCaptureWeight(uint32_t objectiveId, uint32_t serverTeamId) const;
    static bool ContainsPoint2D(const CaptureZone& zone, const Vector3& point);

    // Clamp elapsed capture simulation to a timed native phase boundary.
    // Invalid/negative inputs produce zero so a malformed frame cannot add
    // capture progress or reverse decay.
    static float ClampCaptureDeltaToPhase(float deltaSeconds,
                                          float phaseTimeRemaining);

    // Advance a neutral objective using separate server-team capture strengths.
    // captureProgress belongs to cappingTeam until it reaches zero; an opposing
    // majority therefore neutralizes existing progress before building its own.
    // Returns the server team that completed capture, or 0 if none did.
    static uint32_t ProcessNeutralCapture(CaptureZone& zone,
                                          float team1CaptureWeight,
                                          float team2CaptureWeight,
                                          float deltaSeconds);

    // Events
    void SetOnObjectiveCaptured(ObjectiveCapturedCallback cb);

    // Broadcast objective state to all clients
    void BroadcastObjectiveStates() const;

private:
    struct PendingCaptureEvent {
        uint32_t objectiveId = 0;
        uint32_t capturingTeam = 0;
        uint32_t previousTeam = 0;
    };

    GameServer* m_server;
    std::map<uint32_t, CaptureZone> m_objectives;
    std::map<uint32_t, uint32_t> m_initialControllingTeams;
    uint32_t m_nextObjectiveId = 1;
    ObjectiveCapturedCallback m_capturedCallback;
    ObjectiveBotCaptureWeightProvider m_botCaptureWeightProvider;
    std::deque<PendingCaptureEvent> m_pendingCaptureEvents;
    uint64_t m_objectiveGeneration = 0;
    uint64_t m_botCaptureProviderGeneration = 0;
    bool m_updateInProgress = false;
    bool m_zoneRefreshInProgress = false;
    bool m_dispatchingCaptureEvents = false;

    // Territory mode state
    std::vector<uint32_t> m_territoryOrder;
    int m_currentTerritoryIndex = 0;
    uint32_t m_territoryAdvancingTeam = 0;
    float m_retailReplicationAccumulator = 0.0f;

    void ProcessCapture(CaptureZone& zone, float deltaSeconds);
    bool CanProcessCaptures() const;
    void OnObjectiveCaptured(CaptureZone& zone, uint32_t newTeam);
    void DispatchPendingCaptureEvents();
    uint32_t AllocateObjectiveId();
    bool IsPlayerInZone(uint32_t playerId, const CaptureZone& zone) const;
};
