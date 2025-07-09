// src/Config/GameConfig.cpp

#include "Config/GameConfig.h"
#include "Utils/FileUtils.h"
#include "Utils/StringUtils.h"
#include <fstream>

GameConfig::GameConfig(const ServerConfig& cfg)
  : m_cfg(cfg)
{}

// Returns list of maps from maps.ini
std::vector<std::string> GameConfig::GetMapRotation() const {
    auto path = m_cfg.GetMapRotationFile();
    return INIParser(path).GetSectionKeys("rotation");
}

// Returns list of modes from game_modes.ini
std::vector<std::string> GameConfig::GetGameModes() const {
    auto path = m_cfg.GetGameModesFile();
    return INIParser(path).GetAllSections();
}

// Gameplay rule getters
bool GameConfig::IsFriendlyFire() const {
    return m_cfg.GetManager()->GetBool("Gameplay.friendly_fire", false);
}

int GameConfig::GetRespawnDelay() const {
    return m_cfg.GetManager()->GetInt("Gameplay.respawn_delay_s", 5);
}

bool GameConfig::IsRoundTimerEnabled() const {
    return m_cfg.GetManager()->GetBool("Gameplay.enable_round_timer", true);
}

int GameConfig::GetRoundTimeLimit() const {
    return m_cfg.GetManager()->GetInt("Gameplay.round_time_limit_s", 900);
}

bool GameConfig::IsScoreLimitEnabled() const {
    return m_cfg.GetManager()->GetBool("Gameplay.enable_score_limit", true);
}

int GameConfig::GetScoreLimit() const {
    return m_cfg.GetManager()->GetInt("Gameplay.score_limit", 1000);
}

bool GameConfig::IsVehicleSpawningEnabled() const {
    return m_cfg.GetManager()->GetBool("Gameplay.enable_vehicles", true);
}

// Paths to dedicated data files
std::string GameConfig::GetMapsIniPath() const {
    return m_cfg.GetMapRotationFile();
}

std::string GameConfig::GetModesIniPath() const {
    return m_cfg.GetGameModesFile();
}

std::string GameConfig::GetMapsAssetDirectory() const {
    return m_cfg.GetDataDirectory() + "/" + m_cfg.GetMapsDataPath();
}

std::string GameConfig::GetTeamsIniPath() const {
    return m_cfg.GetManager()->GetString("DataPaths.teams_path", "config/teams.ini");
}

std::string GameConfig::GetWeaponsIniPath() const {
    return m_cfg.GetManager()->GetString("DataPaths.weapons_path", "config/weapons.ini");
}