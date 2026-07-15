// src/Game/SupremacyMode.h
// RS2V Supremacy game mode - connected-objective tug of war.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

class GameServer;

class SupremacyMode {
public:
    static constexpr uint32_t kSouthTeamId = 1;
    static constexpr uint32_t kNorthTeamId = 2;
    static constexpr int32_t kDefaultScoreTarget = 500;

    enum class Phase : uint8_t {
        WarmUp,
        Preparation,
        Active,
        // Reserved ordinal. Retail Supremacy remains Active when a team runs
        // out of reinforcements; elimination is tracked independently.
        SuddenDeath,
        PostRound,
        Finished
    };

    explicit SupremacyMode(GameServer* server);
    ~SupremacyMode();

    void Initialize();
    void Shutdown();

    void StartRound();
    void EndRound();
    void Update(float deltaSeconds);

    // Events
    void OnObjectiveCaptured(uint32_t objectiveId, uint32_t capturingTeam);
    void OnTicketsDepleted(uint32_t teamId);
    void OnPlayerKilled(uint32_t killerId, uint32_t victimId);
    void OnTeamEliminated(uint32_t eliminatedTeamId);

    // State queries
    Phase GetPhase() const { return m_phase; }
    bool CanCaptureObjectives() const { return m_phase == Phase::Active; }
    float GetRoundDuration() const { return m_roundTime; }
    float GetRoundTimeRemaining() const;
    float GetPhaseDuration() const;
    float GetPhaseTimeRemaining() const;
    int32_t GetScore() const { return m_score; }
    int32_t GetScoreTarget() const { return m_scoreTarget; }
    // Server-team id (1=South/US, 2=North/NVA), or 0 for no winner/draw.
    uint32_t GetWinningTeam() const { return m_winningTeam; }

    // Compatibility view of the old two-pool model. The values are derived
    // from the signed score and always total GetScoreTarget(): at the default
    // target a tied round reports 250 points per team.
    float GetTeamPoints(uint32_t teamId) const;
    float GetPointBarProgress() const;  // 0.0 = North wins, 0.5 = tie, 1.0 = South wins

    int GetTeamObjectiveValue(uint32_t teamId) const;
    int GetSouthConnectedObjectiveValue() const;
    int GetNorthConnectedObjectiveValue() const;
    bool UsesSupplyLines() const;
    bool IsObjectiveLinked(uint32_t objectiveId, uint32_t teamId) const;
    bool HasObjective(uint32_t objectiveId) const;
    uint32_t GetObjectiveControllingTeam(uint32_t objectiveId) const;
    int GetObjectivePointValue(uint32_t objectiveId) const;
    std::vector<uint32_t> GetObjectiveLinks(uint32_t objectiveId) const;
    std::optional<uint32_t> GetTeamHQ(uint32_t teamId) const;

    // Configuration
    void SetScoreTarget(int32_t target);
    void SetScoringInterval(float seconds);

    // Legacy configuration aliases. Starting points represented one half of
    // the old fixed-size pool, so it maps to a signed target of 2 * points.
    void SetStartingPoints(float points);
    void SetPointDrainInterval(float seconds);
    void SetRoundTime(float seconds);

    // Objective supply chain. Configure initial ownership and point value for
    // every map objective, then provide adjacency lists and each team's HQ.
    // When both teams have a valid HQ, a controlled objective scores only while
    // connected to its own HQ through objectives controlled by the same team.
    // Maps without both HQs use retail's fallback and score every controlled
    // objective without supply-line connectivity.
    void ClearObjectives();
    void SetObjectiveMetadata(uint32_t objectiveId,
                              uint32_t controllingTeam,
                              int pointValue);
    void SetObjectiveLinks(const std::map<uint32_t, std::vector<uint32_t>>& links);
    void SetTeamHQ(uint32_t teamId, uint32_t objectiveId);

private:
    GameServer* m_server;
    Phase m_phase = Phase::WarmUp;

    // Retail Supremacy uses one signed score: positive favors South, negative
    // favors North. Every scoring interval adds SouthConnectedValue minus
    // NorthConnectedValue, clamped to +/- m_scoreTarget.
    int32_t m_score = 0;
    int32_t m_scoreTarget = kDefaultScoreTarget;
    uint32_t m_winningTeam = 0;
    float m_scoringInterval = 5.0f;
    double m_scoreTickTimer = 0.0;

    // Timing
    float m_roundTime = 1200.0f;
    float m_phaseTimer = 0.0f;
    float m_preparationTime = 30.0f;
    float m_postRoundTime = 15.0f;

    struct ObjectiveNode {
        uint32_t id = 0;
        int pointValue = 1;
        uint32_t initialControllingTeam = 0;
        uint32_t controllingTeam = 0;
        std::vector<uint32_t> linkedTo;
    };
    std::map<uint32_t, ObjectiveNode> m_objectiveGraph;
    std::optional<uint32_t> m_southHQ;
    std::optional<uint32_t> m_northHQ;
    std::array<bool, 2> m_ticketsDepleted{};

    void SetPhase(Phase newPhase);
    void ProcessScoreFlow(float deltaSeconds);
    int64_t CalculateLinkedObjectiveValue(uint32_t teamId) const;
    bool HasPathFromHQ(uint32_t objectiveId, uint32_t teamId) const;
    void ResetObjectivesToInitialOwners();
    bool IsTeamTicketsDepleted(uint32_t teamId) const;
    bool TeamHasLivingParticipant(uint32_t teamId) const;
    void CheckReinforcementElimination();
    void CheckWinConditions();
    uint32_t DetermineWinner() const;
    void FinishRoundWithWinner(uint32_t winningTeam);
    void BroadcastPhaseChange() const;
};
