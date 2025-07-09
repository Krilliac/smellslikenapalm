// src/Game/RoundManager.cpp – Implementation for RoundManager

#include "Game/RoundManager.h"
#include "Game/GameServer.h"
#include "Game/GameState.h"
#include "Config/GameConfig.h"
#include "Utils/Logger.h"

RoundManager::RoundManager(GameServer* server)
    : m_server(server)
{
    m_gameState = m_server->GetGameState();
    auto settings = m_server->GetGameConfig()->GetGameSettings();
    m_preparationDuration = std::chrono::seconds(settings.preparationTime);
    m_roundDuration       = std::chrono::seconds(settings.roundTimeLimit);
    m_postRoundDuration   = std::chrono::seconds(10);  // fixed summary time
}

RoundManager::~RoundManager() = default;

void RoundManager::Initialize()
{
    Logger::Info("RoundManager initialized");
    m_roundNumber = 0;
    StartPreparation();
}

void RoundManager::Shutdown()
{
    Logger::Info("RoundManager shutdown");
}

void RoundManager::StartPreparation()
{
    TransitionToPhase(RoundPhase::Preparation);
}

void RoundManager::StartRound()
{
    TransitionToPhase(RoundPhase::Active);
}

void RoundManager::EndRound()
{
    TransitionToPhase(RoundPhase::PostRound);
}

void RoundManager::Update()
{
    auto now = std::chrono::steady_clock::now();
    if (now >= m_phaseEndTime) {
        switch (m_phase) {
            case RoundPhase::Preparation:
                StartRound();
                break;
            case RoundPhase::Active:
                OnTimeExpired();
                break;
            case RoundPhase::PostRound:
                StartPreparation();
                break;
        }
    }
}

void RoundManager::OnObjectiveCaptured(uint32_t objectiveId, uint32_t teamId)
{
    // If score limit reached, end round early
    if (m_gameState->CheckWinCondition()) {
        EndRound();
    }
}

void RoundManager::OnTimeExpired()
{
    Logger::Info("RoundManager: Round time expired");
    EndRound();
}

void RoundManager::SetOnRoundStart(std::function<void()> cb)
{
    m_onRoundStart = std::move(cb);
}

void RoundManager::SetOnRoundEnd(std::function<void()> cb)
{
    m_onRoundEnd = std::move(cb);
}

RoundPhase RoundManager::GetCurrentPhase() const
{
    return m_phase;
}

uint32_t RoundManager::GetRoundNumber() const
{
    return m_roundNumber;
}

void RoundManager::TransitionToPhase(RoundPhase newPhase)
{
    m_phase = newPhase;
    auto now = std::chrono::steady_clock::now();

    switch (m_phase) {
        case RoundPhase::Preparation:
            m_phaseEndTime = now + m_preparationDuration;
            Logger::Info("Round %u: Preparation phase started", m_roundNumber + 1);
            m_gameState->SetPhase(GamePhase::Preparation);
            break;
        case RoundPhase::Active:
            m_roundNumber++;
            m_phaseEndTime = now + m_roundDuration;
            Logger::Info("Round %u: Active phase started", m_roundNumber);
            m_gameState->StartRound();
            if (m_onRoundStart) m_onRoundStart();
            break;
        case RoundPhase::PostRound:
            m_phaseEndTime = now + m_postRoundDuration;
            Logger::Info("Round %u: Post-round phase started", m_roundNumber);
            m_gameState->EndRound();
            if (m_onRoundEnd) m_onRoundEnd();
            break;
    }

    BroadcastPhaseMessage();
}

void RoundManager::BroadcastPhaseMessage() const
{
    std::string msg;
    switch (m_phase) {
        case RoundPhase::Preparation:
            msg = "Next round starting in " + std::to_string(m_preparationDuration.count()) + " seconds.";
            break;
        case RoundPhase::Active:
            msg = "Round " + std::to_string(m_roundNumber) + " is now live for " + std::to_string(m_roundDuration.count()) + " seconds!";
            break;
        case RoundPhase::PostRound:
            msg = "Round " + std::to_string(m_roundNumber) + " ended. Showing results for " + std::to_string(m_postRoundDuration.count()) + " seconds.";
            break;
    }
    m_server->BroadcastChatMessage("[RoundManager] " + msg);
}