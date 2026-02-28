// src/Config/GameConfig.cpp

#include "Config/GameConfig.h"
#include "Config/INIParser.h"
#include "Utils/FileUtils.h"
#include "Utils/StringUtils.h"
#include "Utils/Logger.h"
#include <fstream>

GameConfig::GameConfig(const ServerConfig& cfg)
  : m_cfg(cfg)
{
    Logger::Trace("[GameConfig::GameConfig] Entry - constructing from ServerConfig");
    Logger::Info("[GameConfig::GameConfig] GameConfig initialized");
    Logger::Trace("[GameConfig::GameConfig] Exit");
}

// Returns list of maps from maps.ini
std::vector<std::string> GameConfig::GetMapRotation() const {
    Logger::Trace("[GameConfig::GetMapRotation] Entry");
    auto path = m_cfg.GetMapRotationFile();
    Logger::Debug("[GameConfig::GetMapRotation] Map rotation file path: '%s'", path.c_str());
    std::vector<std::string> maps;
    // Parse maps from the rotation config file
    auto mgr = m_cfg.GetManager();
    if (mgr) {
        std::string rotationStr = mgr->GetString("MapRotation.maps", "");
        Logger::Debug("[GameConfig::GetMapRotation] MapRotation.maps raw string: '%s'", rotationStr.c_str());
        if (!rotationStr.empty()) {
            maps = StringUtils::Split(rotationStr, ',');
            for (auto& m : maps) m = StringUtils::Trim(m);
            Logger::Debug("[GameConfig::GetMapRotation] Parsed %zu maps from rotation string", maps.size());
            for (size_t i = 0; i < maps.size(); ++i) {
                Logger::Trace("[GameConfig::GetMapRotation] Map[%zu]: '%s'", i, maps[i].c_str());
            }
        } else {
            Logger::Debug("[GameConfig::GetMapRotation] MapRotation.maps is empty, will use default");
        }
    } else {
        Logger::Warn("[GameConfig::GetMapRotation] ConfigManager is null, cannot read map rotation");
    }
    if (maps.empty()) {
        Logger::Debug("[GameConfig::GetMapRotation] No maps found, using default 'VNTE-CuChi'");
        maps.push_back("VNTE-CuChi");
    }
    Logger::Trace("[GameConfig::GetMapRotation] Exit - returning %zu maps", maps.size());
    return maps;
}

// Returns list of modes from config
std::vector<std::string> GameConfig::GetGameModes() const {
    Logger::Trace("[GameConfig::GetGameModes] Entry");
    auto mgr = m_cfg.GetManager();
    std::vector<std::string> modes;
    if (mgr) {
        std::string modesStr = mgr->GetString("GameModes.available", "Conquest,Elimination");
        Logger::Debug("[GameConfig::GetGameModes] GameModes.available raw string: '%s'", modesStr.c_str());
        modes = StringUtils::Split(modesStr, ',');
        for (auto& m : modes) m = StringUtils::Trim(m);
        Logger::Debug("[GameConfig::GetGameModes] Parsed %zu game modes", modes.size());
        for (size_t i = 0; i < modes.size(); ++i) {
            Logger::Trace("[GameConfig::GetGameModes] Mode[%zu]: '%s'", i, modes[i].c_str());
        }
    } else {
        Logger::Warn("[GameConfig::GetGameModes] ConfigManager is null, cannot read game modes");
    }
    if (modes.empty()) {
        Logger::Debug("[GameConfig::GetGameModes] No modes found, using default 'Conquest'");
        modes.push_back("Conquest");
    }
    Logger::Trace("[GameConfig::GetGameModes] Exit - returning %zu modes", modes.size());
    return modes;
}

// Gameplay rule getters
bool GameConfig::IsFriendlyFire() const {
    Logger::Trace("[GameConfig::IsFriendlyFire] Entry");
    bool result = m_cfg.GetManager()->GetBool("Gameplay.friendly_fire", false);
    Logger::Trace("[GameConfig::IsFriendlyFire] Exit - returning %s", result ? "true" : "false");
    return result;
}

int GameConfig::GetRespawnDelay() const {
    Logger::Trace("[GameConfig::GetRespawnDelay] Entry");
    int result = m_cfg.GetManager()->GetInt("Gameplay.respawn_delay_s", 5);
    Logger::Trace("[GameConfig::GetRespawnDelay] Exit - returning %d", result);
    return result;
}

bool GameConfig::IsRoundTimerEnabled() const {
    Logger::Trace("[GameConfig::IsRoundTimerEnabled] Entry");
    bool result = m_cfg.GetManager()->GetBool("Gameplay.enable_round_timer", true);
    Logger::Trace("[GameConfig::IsRoundTimerEnabled] Exit - returning %s", result ? "true" : "false");
    return result;
}

int GameConfig::GetRoundTimeLimit() const {
    Logger::Trace("[GameConfig::GetRoundTimeLimit] Entry");
    int result = m_cfg.GetManager()->GetInt("Gameplay.round_time_limit_s", 900);
    Logger::Trace("[GameConfig::GetRoundTimeLimit] Exit - returning %d", result);
    return result;
}

bool GameConfig::IsScoreLimitEnabled() const {
    Logger::Trace("[GameConfig::IsScoreLimitEnabled] Entry");
    bool result = m_cfg.GetManager()->GetBool("Gameplay.enable_score_limit", true);
    Logger::Trace("[GameConfig::IsScoreLimitEnabled] Exit - returning %s", result ? "true" : "false");
    return result;
}

int GameConfig::GetScoreLimit() const {
    Logger::Trace("[GameConfig::GetScoreLimit] Entry");
    int result = m_cfg.GetManager()->GetInt("Gameplay.score_limit", 1000);
    Logger::Trace("[GameConfig::GetScoreLimit] Exit - returning %d", result);
    return result;
}

bool GameConfig::IsVehicleSpawningEnabled() const {
    Logger::Trace("[GameConfig::IsVehicleSpawningEnabled] Entry");
    bool result = m_cfg.GetManager()->GetBool("Gameplay.enable_vehicles", true);
    Logger::Trace("[GameConfig::IsVehicleSpawningEnabled] Exit - returning %s", result ? "true" : "false");
    return result;
}

// Paths to dedicated data files
std::string GameConfig::GetMapsIniPath() const {
    Logger::Trace("[GameConfig::GetMapsIniPath] Entry");
    auto result = m_cfg.GetMapRotationFile();
    Logger::Trace("[GameConfig::GetMapsIniPath] Exit - returning '%s'", result.c_str());
    return result;
}

std::string GameConfig::GetModesIniPath() const {
    Logger::Trace("[GameConfig::GetModesIniPath] Entry");
    auto result = m_cfg.GetGameModesFile();
    Logger::Trace("[GameConfig::GetModesIniPath] Exit - returning '%s'", result.c_str());
    return result;
}

std::string GameConfig::GetMapsAssetDirectory() const {
    Logger::Trace("[GameConfig::GetMapsAssetDirectory] Entry");
    auto result = m_cfg.GetDataDirectory() + "/" + m_cfg.GetMapsDataPath();
    Logger::Trace("[GameConfig::GetMapsAssetDirectory] Exit - returning '%s'", result.c_str());
    return result;
}

std::string GameConfig::GetTeamsIniPath() const {
    Logger::Trace("[GameConfig::GetTeamsIniPath] Entry");
    auto result = m_cfg.GetManager()->GetString("DataPaths.teams_path", "config/teams.ini");
    Logger::Trace("[GameConfig::GetTeamsIniPath] Exit - returning '%s'", result.c_str());
    return result;
}

std::string GameConfig::GetWeaponsIniPath() const {
    Logger::Trace("[GameConfig::GetWeaponsIniPath] Entry");
    auto result = m_cfg.GetManager()->GetString("DataPaths.weapons_path", "config/weapons.ini");
    Logger::Trace("[GameConfig::GetWeaponsIniPath] Exit - returning '%s'", result.c_str());
    return result;
}

GameSettings GameConfig::GetGameSettings() const {
    Logger::Trace("[GameConfig::GetGameSettings] Entry");
    GameSettings gs;
    auto mgr = m_cfg.GetManager();
    gs.mapName        = mgr->GetString("Game.initial_map", "VNTE-CuChi");
    gs.gameMode       = mgr->GetString("Game.game_mode", "Conquest");
    gs.friendlyFire   = IsFriendlyFire();
    gs.respawnDelay   = GetRespawnDelay();
    gs.roundTimeLimit = GetRoundTimeLimit();
    gs.scoreLimit     = GetScoreLimit();
    Logger::Debug("[GameConfig::GetGameSettings] Assembled GameSettings: mapName='%s', gameMode='%s', friendlyFire=%s, respawnDelay=%d, roundTimeLimit=%d, scoreLimit=%d",
                  gs.mapName.c_str(), gs.gameMode.c_str(), gs.friendlyFire ? "true" : "false",
                  gs.respawnDelay, gs.roundTimeLimit, gs.scoreLimit);
    Logger::Trace("[GameConfig::GetGameSettings] Exit");
    return gs;
}

std::optional<GameModeDefinition> GameConfig::GetGameModeDefinition(const std::string& modeName) const {
    Logger::Trace("[GameConfig::GetGameModeDefinition] Entry - modeName='%s'", modeName.c_str());
    auto mgr = m_cfg.GetManager();
    std::string prefix = "GameMode." + modeName + ".";
    Logger::Debug("[GameConfig::GetGameModeDefinition] Config prefix: '%s'", prefix.c_str());

    // Check if mode exists in config
    if (!mgr->HasKey(prefix + "name") && !mgr->HasKey(prefix + "display_name")) {
        Logger::Debug("[GameConfig::GetGameModeDefinition] Mode '%s' not found in config (no '%sname' or '%sdisplay_name')", modeName.c_str(), prefix.c_str(), prefix.c_str());
        // Return a default definition for well-known modes
        if (modeName == "Conquest" || modeName == "Elimination" || modeName == "CTF") {
            Logger::Debug("[GameConfig::GetGameModeDefinition] '%s' is a well-known mode, creating default definition", modeName.c_str());
            GameModeDefinition def;
            def.name           = modeName;
            def.displayName    = modeName;
            def.roundTimeLimit = GetRoundTimeLimit();
            def.scoreLimit     = GetScoreLimit();
            def.respawnDelay   = GetRespawnDelay();
            def.friendlyFire   = IsFriendlyFire();
            def.vehiclesEnabled = IsVehicleSpawningEnabled();
            Logger::Info("[GameConfig::GetGameModeDefinition] Created default definition for well-known mode '%s'", modeName.c_str());
            Logger::Trace("[GameConfig::GetGameModeDefinition] Exit - returning default definition");
            return def;
        }
        Logger::Warn("[GameConfig::GetGameModeDefinition] Unknown mode '%s' with no config entry, returning nullopt", modeName.c_str());
        Logger::Trace("[GameConfig::GetGameModeDefinition] Exit - returning nullopt");
        return std::nullopt;
    }

    Logger::Debug("[GameConfig::GetGameModeDefinition] Mode '%s' found in config, reading properties", modeName.c_str());
    GameModeDefinition def;
    def.name           = modeName;
    def.displayName    = mgr->GetString(prefix + "display_name", modeName);
    def.roundTimeLimit = mgr->GetInt(prefix + "round_time_limit", GetRoundTimeLimit());
    def.scoreLimit     = mgr->GetInt(prefix + "score_limit", GetScoreLimit());
    def.respawnDelay   = mgr->GetInt(prefix + "respawn_delay", GetRespawnDelay());
    def.friendlyFire   = mgr->GetBool(prefix + "friendly_fire", IsFriendlyFire());
    def.vehiclesEnabled = mgr->GetBool(prefix + "vehicles_enabled", IsVehicleSpawningEnabled());
    Logger::Debug("[GameConfig::GetGameModeDefinition] Loaded mode '%s': displayName='%s', roundTimeLimit=%d, scoreLimit=%d, respawnDelay=%d, friendlyFire=%s, vehicles=%s",
                  def.name.c_str(), def.displayName.c_str(), def.roundTimeLimit, def.scoreLimit,
                  def.respawnDelay, def.friendlyFire ? "true" : "false", def.vehiclesEnabled ? "true" : "false");
    Logger::Trace("[GameConfig::GetGameModeDefinition] Exit - returning loaded definition");
    return def;
}
