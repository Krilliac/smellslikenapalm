// src/Game/RoundManager.h – Header for RoundManager

#pragma once

#include <chrono>
#include <functional>
#include <vector>

class GameServer;
class GameState;

enum class RoundPhase {
    Preparation,
    Active,
    PostRound
};

class RoundManager {
public:
    explicit RoundManager(GameServer* server);
    ~RoundManager();

    // Initialize and start round cycle
    void Initialize();
    void Shutdown();

    // Begin preparation phase for next round
    void StartPreparation();
    // Begin active gameplay phase
    void StartRound();
    // Begin post-round summary phase
    void EndRound();

    // Tick update, called each server loop
    void Update();

    // Handlers for external triggers
    void OnObjectiveCaptured(uint32_t objectiveId, uint32_t teamId);
    void OnTimeExpired();

    // Set callbacks
    void SetOnRoundStart(std::function<void()> cb);
    void SetOnRoundEnd  (std::function<void()> cb);

    RoundPhase GetCurrentPhase() const;
    uint32_t    GetRoundNumber() const;

private:
    GameServer* m_server;
    GameState*  m_gameState;

    RoundPhase  m_phase{RoundPhase::Preparation};
    uint32_t    m_roundNumber{0};

    std::chrono::steady_clock::time_point m_phaseEndTime;
    std::chrono::seconds                  m_preparationDuration;
    std::chrono::seconds                  m_roundDuration;
    std::chrono::seconds                  m_postRoundDuration;

    std::function<void()> m_onRoundStart;
    std::function<void()> m_onRoundEnd;

    void TransitionToPhase(RoundPhase newPhase);
    void BroadcastPhaseMessage() const;
};