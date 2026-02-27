// src/Config/GameConfig.h

#pragma once

#include <string>
#include <vector>
#include <optional>
#include <memory>
#include "Config/ServerConfig.h"
#include "Config/MapConfig.h"

class GameConfig {
public:
    explicit GameConfig(const ServerConfig& cfg);

    // Map rotation and modes
    std::vector<std::string> GetMapRotation() const;
    std::vector<std::string> GetGameModes() const;

    // Gameplay rules
    bool  IsFriendlyFire() const;
    int   GetRespawnDelay() const;
    bool  IsRoundTimerEnabled() const;
    int   GetRoundTimeLimit() const;
    bool  IsScoreLimitEnabled() const;
    int   GetScoreLimit() const;
    bool  IsVehicleSpawningEnabled() const;

    // Data file paths
    std::string GetMapsIniPath() const;
    std::string GetModesIniPath() const;
    std::string GetTeamsIniPath() const;
    std::string GetWeaponsIniPath() const;
    std::string GetMapsAssetDirectory() const;

    // Game settings (used by GameServer to set up initial state)
    GameSettings GetGameSettings() const;

    // Game mode definitions (loaded from config)
    std::optional<GameModeDefinition> GetGameModeDefinition(const std::string& modeName) const;

private:
    const ServerConfig& m_cfg;
};
