// src/Config/GameConfig.cpp

#include "Config/GameConfig.h"
#include "Config/INIParser.h"
#include "Utils/FileUtils.h"
#include "Utils/StringUtils.h"
#include "Utils/Logger.h"
#include <fstream>

GameConfig::GameConfig(const ServerConfig& cfg)
  : m_cfg(cfg)
{}

// Returns list of maps from maps.ini
std::vector<std::string> GameConfig::GetMapRotation() const {
    auto path = m_cfg.GetMapRotationFile();
    std::vector<std::string> maps;
    // Parse maps from the rotation config file
    auto mgr = m_cfg.GetManager();
    if (mgr) {
        std::string rotationStr = mgr->GetString("MapRotation.maps", "");
        if (!rotationStr.empty()) {
            maps = StringUtils::Split(rotationStr, ',');
            for (auto& m : maps) m = StringUtils::Trim(m);
        }
    }
    if (maps.empty()) {
        maps.push_back("VNTE-CuChi");
    }
    return maps;
}

// Returns list of modes from config
std::vector<std::string> GameConfig::GetGameModes() const {
    auto mgr = m_cfg.GetManager();
    std::vector<std::string> modes;
    if (mgr) {
        std::string modesStr = mgr->GetString("GameModes.available", "Conquest,Elimination");
        modes = StringUtils::Split(modesStr, ',');
        for (auto& m : modes) m = StringUtils::Trim(m);
    }
    if (modes.empty()) {
        modes.push_back("Conquest");
    }
    return modes;
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

GameSettings GameConfig::GetGameSettings() const {
    GameSettings gs;
    auto mgr = m_cfg.GetManager();
    gs.mapName        = mgr->GetString("Game.initial_map", "VNTE-CuChi");
    gs.gameMode       = mgr->GetString("Game.game_mode", "Conquest");
    gs.friendlyFire   = IsFriendlyFire();
    gs.respawnDelay   = GetRespawnDelay();
    gs.roundTimeLimit = GetRoundTimeLimit();
    gs.scoreLimit     = GetScoreLimit();
    return gs;
}

std::optional<GameModeDefinition> GameConfig::GetGameModeDefinition(const std::string& modeName) const {
    auto mgr = m_cfg.GetManager();
    std::string prefix = "GameMode." + modeName + ".";

    // Check if mode exists in config
    if (!mgr->HasKey(prefix + "name") && !mgr->HasKey(prefix + "display_name")) {
        // Return a default definition for well-known modes
        if (modeName == "Conquest" || modeName == "Elimination" || modeName == "CTF") {
            GameModeDefinition def;
            def.name           = modeName;
            def.displayName    = modeName;
            def.roundTimeLimit = GetRoundTimeLimit();
            def.scoreLimit     = GetScoreLimit();
            def.respawnDelay   = GetRespawnDelay();
            def.friendlyFire   = IsFriendlyFire();
            def.vehiclesEnabled = IsVehicleSpawningEnabled();
            return def;
        }
        return std::nullopt;
    }

    GameModeDefinition def;
    def.name           = modeName;
    def.displayName    = mgr->GetString(prefix + "display_name", modeName);
    def.roundTimeLimit = mgr->GetInt(prefix + "round_time_limit", GetRoundTimeLimit());
    def.scoreLimit     = mgr->GetInt(prefix + "score_limit", GetScoreLimit());
    def.respawnDelay   = mgr->GetInt(prefix + "respawn_delay", GetRespawnDelay());
    def.friendlyFire   = mgr->GetBool(prefix + "friendly_fire", IsFriendlyFire());
    def.vehiclesEnabled = mgr->GetBool(prefix + "vehicles_enabled", IsVehicleSpawningEnabled());
    return def;
}
