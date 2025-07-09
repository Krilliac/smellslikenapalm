// src/Game/GameMode.h – Header for GameMode

#pragma once

#include <chrono>
#include <string>
#include "Config/GameConfig.h"
#include "Network/PacketData.h"

class GameServer;

class GameMode {
public:
    enum class Phase { Preparation, Active, PostRound };

    GameMode(GameServer* server, const GameModeDefinition& def);
    ~GameMode();

    void OnStart();
    void OnEnd();
    void Update();
    void HandlePlayerAction(uint32_t playerId, const std::string& action, const PacketData& data);
    bool ReloadConfiguration(const GameModeDefinition& def);

    const std::string& GetName() const;
    Phase GetPhase() const;
    uint64_t GetTickCount() const;

private:
    GameServer* m_server;
    GameModeDefinition m_def;
    Phase m_phase{Phase::Preparation};
    uint64_t m_tickCount{0};
    std::chrono::steady_clock::time_point m_phaseEndTime;

    void AdvancePhase();
    void BroadcastPhase() const;
    void HandleRespawns();
    void HandleObjectiveCapture(uint32_t playerId, uint32_t objectiveId);

    void LogGameModeSettings() const;
};