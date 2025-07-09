// src/Config/GameConfig.cpp - Complete implementation for RS2V Server game configuration
#include "Config/GameConfig.h"
#include "Utils/Logger.h"
#include "Utils/StringUtils.h"
#include "Utils/FileUtils.h"
#include <algorithm>
#include <fstream>
#include <filesystem>

GameConfig::GameConfig() {
    Logger::Info("GameConfig initialized");
    InitializeDefaultGameSettings();
}

GameConfig::~GameConfig() = default;

bool GameConfig::Initialize(std::shared_ptr<ConfigManager> configManager) {
    Logger::Info("Initializing Game Configuration...");
    
    m_configManager = configManager;
    
    if (!m_configManager) {
        Logger::Error("ConfigManager is null");
        return false;
    }
    
    // Load game configuration from config manager
    if (!LoadGameConfiguration()) {
        Logger::Error("Failed to load game configuration");
        return false;
    }
    
    // Load map definitions
    if (!LoadMapDefinitions()) {
        Logger::Error("Failed to load map definitions");
        return false;
    }
    
    // Load game mode definitions
    if (!LoadGameModeDefinitions()) {
        Logger::Error("Failed to load game mode definitions");
        return false;
    }
    
    // Load weapon configurations
    if (!LoadWeaponConfigurations()) {
        Logger::Error("Failed to load weapon configurations");
        return false;
    }
    
    // Load team configurations
    if (!LoadTeamConfigurations()) {
        Logger::Error("Failed to load team configurations");
        return false;
    }
    
    // Validate loaded configuration
    if (!ValidateGameConfiguration()) {
        Logger::Error("Game configuration validation failed");
        return false;
    }
    
    Logger::Info("Game Configuration initialized successfully");
    LogGameConfigurationSummary();
    
    return true;
}

bool GameConfig::LoadGameConfiguration() {
    Logger::Debug("Loading game configuration from ConfigManager...");
    
    // Basic game settings
    m_gameSettings.mapName = m_configManager->GetString("Game.MapName", "VNTE-CuChi");
    m_gameSettings.gameMode = m_configManager->GetString("Game.GameMode", "Territories");
    m_gameSettings.maxPlayers = m_configManager->GetInt("Game.MaxPlayers", 64);
    m_gameSettings.roundTimeLimit = m_configManager->GetInt("Game.RoundTimeLimit", 1800); // 30 minutes
    m_gameSettings.preparationTime = m_configManager->GetInt("Game.PreparationTime", 60);
    m_gameSettings.respawnDelay = m_configManager->GetFloat("Game.RespawnDelay", 10.0f);
    
    // Team settings
    m_gameSettings.allowTeamSwitch = m_configManager->GetBool("Game.AllowTeamSwitch", true);
    m_gameSettings.autoBalanceTeams = m_configManager->GetBool("Game.AutoBalanceTeams", true);
    m_gameSettings.teamBalanceTolerance = m_configManager->GetInt("Game.TeamBalanceTolerance", 2);
    
    // Combat settings
    m_gameSettings.friendlyFireScale = m_configManager->GetFloat("Game.FriendlyFireScale", 0.5f);
    m_gameSettings.allowTK = m_configManager->GetBool("Game.AllowTeamKilling", false);
    m_gameSettings.tkPunishmentThreshold = m_configManager->GetInt("Game.TKPunishmentThreshold", 3);
    m_gameSettings.tkKickThreshold = m_configManager->GetInt("Game.TKKickThreshold", 5);
    
    // Weapon settings
    m_gameSettings.weaponRespawnTime = m_configManager->GetFloat("Game.WeaponRespawnTime", 30.0f);
    m_gameSettings.allowWeaponPickup = m_configManager->GetBool("Game.AllowWeaponPickup", true);
    m_gameSettings.unlimitedAmmo = m_configManager->GetBool("Game.UnlimitedAmmo", false);
    m_gameSettings.realismMode = m_configManager->GetBool("Game.RealismMode", true);
    
    // Vehicle settings
    m_gameSettings.enableVehicles = m_configManager->GetBool("Game.EnableVehicles", true);
    m_gameSettings.vehicleRespawnTime = m_configManager->GetFloat("Game.VehicleRespawnTime", 120.0f);
    m_gameSettings.vehicleDamageScale = m_configManager->GetFloat("Game.VehicleDamageScale", 1.0f);
    
    // Objective settings
    m_gameSettings.captureTime = m_configManager->GetFloat("Game.CaptureTime", 30.0f);
    m_gameSettings.neutralizationTime = m_configManager->GetFloat("Game.NeutralizationTime", 15.0f);
    m_gameSettings.objectiveInfluenceRadius = m_configManager->GetFloat("Game.ObjectiveInfluenceRadius", 50.0f);
    
    // Advanced settings
    m_gameSettings.allowSpectators = m_configManager->GetBool("Game.AllowSpectators", true);
    m_gameSettings.maxSpectators = m_configManager->GetInt("Game.MaxSpectators", 8);
    m_gameSettings.enableVoiceChat = m_configManager->GetBool("Game.EnableVoiceChat", true);
    m_gameSettings.enableTextChat = m_configManager->GetBool("Game.EnableTextChat", true);
    m_gameSettings.allowChatDuringDeath = m_configManager->GetBool("Game.AllowChatDuringDeath", false);
    
    // Vietnam-specific settings
    m_gameSettings.enableTunnels = m_configManager->GetBool("Game.EnableTunnels", true);
    m_gameSettings.enableTraps = m_configManager->GetBool("Game.EnableTraps", true);
    m_gameSettings.enableAmbientEffects = m_configManager->GetBool("Game.EnableAmbientEffects", true);
    m_gameSettings.napalmEnabled = m_configManager->GetBool("Game.NapalmEnabled", true);
    m_gameSettings.helicopterSupport = m_configManager->GetBool("Game.HelicopterSupport", true);
    
    // Commander settings
    m_gameSettings.enableCommander = m_configManager->GetBool("Game.EnableCommander", true);
    m_gameSettings.commanderVoteTime = m_configManager->GetInt("Game.CommanderVoteTime", 60);
    m_gameSettings.allowCommanderMutiny = m_configManager->GetBool("Game.AllowCommanderMutiny", true);
    
    Logger::Debug("Game configuration loaded successfully");
    return true;
}

bool GameConfig::LoadMapDefinitions() {
    Logger::Debug("Loading map definitions...");
    
    std::string mapConfigFile = "config/maps.ini";
    
    if (!std::filesystem::exists(mapConfigFile)) {
        Logger::Warn("Map configuration file not found: %s, using defaults", mapConfigFile.c_str());
        LoadDefaultMapDefinitions();
        return true;
    }
    
    std::ifstream file(mapConfigFile);
    if (!file.is_open()) {
        Logger::Error("Failed to open map configuration file: %s", mapConfigFile.c_str());
        return false;
    }
    
    std::string line;
    std::string currentMapName;
    MapDefinition currentMap;
    
    while (std::getline(file, line)) {
        line = StringUtils::Trim(line);
        
        // Skip comments and empty lines
        if (line.empty() || line[0] == '#') {
            continue;
        }
        
        // Map section header
        if (line.front() == '[' && line.back() == ']') {
            // Save previous map if valid
            if (!currentMapName.empty() && IsValidMapDefinition(currentMap)) {
                m_mapDefinitions[currentMapName] = currentMap;
                Logger::Debug("Loaded map definition: %s", currentMapName.c_str());
            }
            
            // Start new map
            currentMapName = line.substr(1, line.length() - 2);
            currentMap = MapDefinition();
            currentMap.name = currentMapName;
            continue;
        }
        
        // Key-value pairs
        size_t equalPos = line.find('=');
        if (equalPos != std::string::npos) {
            std::string key = StringUtils::Trim(line.substr(0, equalPos));
            std::string value = StringUtils::Trim(line.substr(equalPos + 1));
            
            ParseMapProperty(currentMap, key, value);
        }
    }
    
    // Save last map
    if (!currentMapName.empty() && IsValidMapDefinition(currentMap)) {
        m_mapDefinitions[currentMapName] = currentMap;
        Logger::Debug("Loaded map definition: %s", currentMapName.c_str());
    }
    
    file.close();
    
    Logger::Info("Loaded %zu map definitions", m_mapDefinitions.size());
    return true;
}

bool GameConfig::LoadGameModeDefinitions() {
    Logger::Debug("Loading game mode definitions...");
    
    std::string gameModeConfigFile = "config/game_modes.ini";
    
    if (!std::filesystem::exists(gameModeConfigFile)) {
        Logger::Warn("Game mode configuration file not found: %s, using defaults", gameModeConfigFile.c_str());
        LoadDefaultGameModeDefinitions();
        return true;
    }
    
    std::ifstream file(gameModeConfigFile);
    if (!file.is_open()) {
        Logger::Error("Failed to open game mode configuration file: %s", gameModeConfigFile.c_str());
        return false;
    }
    
    std::string line;
    std::string currentModeName;
    GameModeDefinition currentMode;
    
    while (std::getline(file, line)) {
        line = StringUtils::Trim(line);
        
        if (line.empty() || line[0] == '#') {
            continue;
        }
        
        // Game mode section header
        if (line.front() == '[' && line.back() == ']') {
            // Save previous mode if valid
            if (!currentModeName.empty() && IsValidGameModeDefinition(currentMode)) {
                m_gameModeDefinitions[currentModeName] = currentMode;
                Logger::Debug("Loaded game mode definition: %s", currentModeName.c_str());
            }
            
            // Start new mode
            currentModeName = line.substr(1, line.length() - 2);
            currentMode = GameModeDefinition();
            currentMode.name = currentModeName;
            continue;
        }
        
        // Key-value pairs
        size_t equalPos = line.find('=');
        if (equalPos != std::string::npos) {
            std::string key = StringUtils::Trim(line.substr(0, equalPos));
            std::string value = StringUtils::Trim(line.substr(equalPos + 1));
            
            ParseGameModeProperty(currentMode, key, value);
        }
    }
    
    // Save last mode
    if (!currentModeName.empty() && IsValidGameModeDefinition(currentMode)) {
        m_gameModeDefinitions[currentModeName] = currentMode;
        Logger::Debug("Loaded game mode definition: %s", currentModeName.c_str());
    }
    
    file.close();
    
    Logger::Info("Loaded %zu game mode definitions", m_gameModeDefinitions.size());
    return true;
}

bool GameConfig::LoadWeaponConfigurations() {
    Logger::Debug("Loading weapon configurations...");
    
    std::string weaponConfigFile = "config/weapons.ini";
    
    if (!std::filesystem::exists(weaponConfigFile)) {
        Logger::Warn("Weapon configuration file not found: %s, using defaults", weaponConfigFile.c_str());
        LoadDefaultWeaponConfigurations();
        return true;
    }
    
    std::ifstream file(weaponConfigFile);
    if (!file.is_open()) {
        Logger::Error("Failed to open weapon configuration file: %s", weaponConfigFile.c_str());
        return false;
    }
    
    std::string line;
    std::string currentWeaponName;
    WeaponDefinition currentWeapon;
    
    while (std::getline(file, line)) {
        line = StringUtils::Trim(line);
        
        if (line.empty() || line[0] == '#') {
            continue;
        }
        
        // Weapon section header
        if (line.front() == '[' && line.back() == ']') {
            // Save previous weapon if valid
            if (!currentWeaponName.empty() && IsValidWeaponDefinition(currentWeapon)) {
                m_weaponDefinitions[currentWeaponName] = currentWeapon;
                Logger::Debug("Loaded weapon definition: %s", currentWeaponName.c_str());
            }
            
            // Start new weapon
            currentWeaponName = line.substr(1, line.length() - 2);
            currentWeapon = WeaponDefinition();
            currentWeapon.name = currentWeaponName;
            continue;
        }
        
        // Key-value pairs
        size_t equalPos = line.find('=');
        if (equalPos != std::string::npos) {
            std::string key = StringUtils::Trim(line.substr(0, equalPos));
            std::string value = StringUtils::Trim(line.substr(equalPos + 1));
            
            ParseWeaponProperty(currentWeapon, key, value);
        }
    }
    
    // Save last weapon
    if (!currentWeaponName.empty() && IsValidWeaponDefinition(currentWeapon)) {
        m_weaponDefinitions[currentWeaponName] = currentWeapon;
        Logger::Debug("Loaded weapon definition: %s", currentWeaponName.c_str());
    }
    
    file.close();
    
    Logger::Info("Loaded %zu weapon definitions", m_weaponDefinitions.size());
    return true;
}

bool GameConfig::LoadTeamConfigurations() {
    Logger::Debug("Loading team configurations...");
    
    std::string teamConfigFile = "config/teams.ini";
    
    if (!std::filesystem::exists(teamConfigFile)) {
        Logger::Warn("Team configuration file not found: %s, using defaults", teamConfigFile.c_str());
        LoadDefaultTeamConfigurations();
        return true;
    }
    
    std::ifstream file(teamConfigFile);
    if (!file.is_open()) {
        Logger::Error("Failed to open team configuration file: %s", teamConfigFile.c_str());
        return false;
    }
    
    std::string line;
    std::string currentTeamName;
    TeamDefinition currentTeam;
    
    while (std::getline(file, line)) {
        line = StringUtils::Trim(line);
        
        if (line.empty() || line[0] == '#') {
            continue;
        }
        
        // Team section header
        if (line.front() == '[' && line.back() == ']') {
            // Save previous team if valid
            if (!currentTeamName.empty() && IsValidTeamDefinition(currentTeam)) {
                m_teamDefinitions[currentTeamName] = currentTeam;
                Logger::Debug("Loaded team definition: %s", currentTeamName.c_str());
            }
            
            // Start new team
            currentTeamName = line.substr(1, line.length() - 2);
            currentTeam = TeamDefinition();
            currentTeam.name = currentTeamName;
            continue;
        }
        
        // Key-value pairs
        size_t equalPos = line.find('=');
        if (equalPos != std::string::npos) {
            std::string key = StringUtils::Trim(line.substr(0, equalPos));
            std::string value = StringUtils::Trim(line.substr(equalPos + 1));
            
            ParseTeamProperty(currentTeam, key, value);
        }
    }
    
    // Save last team
    if (!currentTeamName.empty() && IsValidTeamDefinition(currentTeam)) {
        m_teamDefinitions[currentTeamName] = currentTeam;
        Logger::Debug("Loaded team definition: %s", currentTeamName.c_str());
    }
    
    file.close();
    
    Logger::Info("Loaded %zu team definitions", m_teamDefinitions.size());
    return true;
}

bool GameConfig::ValidateGameConfiguration() {
    Logger::Debug("Validating game configuration...");
    
    bool isValid = true;
    
    // Validate basic settings
    if (m_gameSettings.maxPlayers < 2 || m_gameSettings.maxPlayers > 128) {
        Logger::Error("Invalid max players: %d (must be 2-128)", m_gameSettings.maxPlayers);
        isValid = false;
    }
    
    if (m_gameSettings.roundTimeLimit < 60 || m_gameSettings.roundTimeLimit > 7200) {
        Logger::Error("Invalid round time limit: %d (must be 60-7200 seconds)", m_gameSettings.roundTimeLimit);
        isValid = false;
    }
    
    if (m_gameSettings.friendlyFireScale < 0.0f || m_gameSettings.friendlyFireScale > 2.0f) {
        Logger::Error("Invalid friendly fire scale: %.2f (must be 0.0-2.0)", m_gameSettings.friendlyFireScale);
        isValid = false;
    }
    
    // Validate map exists
    if (m_mapDefinitions.find(m_gameSettings.mapName) == m_mapDefinitions.end()) {
        Logger::Error("Map not found: %s", m_gameSettings.mapName.c_str());
        isValid = false;
    }
    
    // Validate game mode exists
    if (m_gameModeDefinitions.find(m_gameSettings.gameMode) == m_gameModeDefinitions.end()) {
        Logger::Error("Game mode not found: %s", m_gameSettings.gameMode.c_str());
        isValid = false;
    }
    
    // Validate map and game mode compatibility
    if (!IsMapGameModeCompatible(m_gameSettings.mapName, m_gameSettings.gameMode)) {
        Logger::Error("Map %s is not compatible with game mode %s", 
                     m_gameSettings.mapName.c_str(), m_gameSettings.gameMode.c_str());
        isValid = false;
    }
    
    // Validate team balance settings
    if (m_gameSettings.teamBalanceTolerance < 0 || m_gameSettings.teamBalanceTolerance > 10) {
        Logger::Error("Invalid team balance tolerance: %d (must be 0-10)", m_gameSettings.teamBalanceTolerance);
        isValid = false;
    }
    
    // Validate TK settings
    if (m_gameSettings.tkPunishmentThreshold > m_gameSettings.tkKickThreshold) {
        Logger::Error("TK punishment threshold (%d) cannot be greater than kick threshold (%d)",
                     m_gameSettings.tkPunishmentThreshold, m_gameSettings.tkKickThreshold);
        isValid = false;
    }
    
    if (isValid) {
        Logger::Info("Game configuration validation passed");
    } else {
        Logger::Error("Game configuration validation failed");
    }
    
    return isValid;
}

void GameConfig::ParseMapProperty(MapDefinition& map, const std::string& key, const std::string& value) {
    if (key == "displayName") {
        map.displayName = value;
    } else if (key == "description") {
        map.description = value;
    } else if (key == "gameMode") {
        map.supportedGameModes.push_back(value);
    } else if (key == "maxPlayers") {
        map.maxPlayers = std::stoi(value);
    } else if (key == "minPlayers") {
        map.minPlayers = std::stoi(value);
    } else if (key == "environment") {
        map.environment = value;
    } else if (key == "timeOfDay") {
        map.timeOfDay = value;
    } else if (key == "weather") {
        map.weather = value;
    } else if (key == "sizeCategory") {
        map.sizeCategory = value;
    } else if (key == "enableTunnels") {
        map.enableTunnels = StringUtils::ToBool(value);
    } else if (key == "enableVehicles") {
        map.enableVehicles = StringUtils::ToBool(value);
    } else if (key == "enableAirSupport") {
        map.enableAirSupport = StringUtils::ToBool(value);
    } else if (key == "objectiveCount") {
        map.objectiveCount = std::stoi(value);
    } else if (key == "usTeamSpawns") {
        map.usTeamSpawns = std::stoi(value);
    } else if (key == "nvaTeamSpawns") {
        map.nvaTeamSpawns = std::stoi(value);
    } else if (key == "filePath") {
        map.filePath = value;
    } else {
        Logger::Debug("Unknown map property: %s", key.c_str());
    }
}

void GameConfig::ParseGameModeProperty(GameModeDefinition& mode, const std::string& key, const std::string& value) {
    if (key == "displayName") {
        mode.displayName = value;
    } else if (key == "description") {
        mode.description = value;
    } else if (key == "category") {
        mode.category = value;
    } else if (key == "maxPlayers") {
        mode.maxPlayers = std::stoi(value);
    } else if (key == "minPlayers") {
        mode.minPlayers = std::stoi(value);
    } else if (key == "roundTimeLimit") {
        mode.roundTimeLimit = std::stoi(value);
    } else if (key == "preparationTime") {
        mode.preparationTime = std::stoi(value);
    } else if (key == "respawnType") {
        mode.respawnType = value;
    } else if (key == "objectiveType") {
        mode.objectiveType = value;
    } else if (key == "teamCount") {
        mode.teamCount = std::stoi(value);
    } else if (key == "allowSpectators") {
        mode.allowSpectators = StringUtils::ToBool(value);
    } else if (key == "enableCommander") {
        mode.enableCommander = StringUtils::ToBool(value);
    } else if (key == "friendlyFire") {
        mode.friendlyFire = StringUtils::ToBool(value);
    } else if (key == "autoBalance") {
        mode.autoBalance = StringUtils::ToBool(value);
    } else if (key == "scoringSystem") {
        mode.scoringSystem = value;
    } else {
        Logger::Debug("Unknown game mode property: %s", key.c_str());
    }
}

void GameConfig::ParseWeaponProperty(WeaponDefinition& weapon, const std::string& key, const std::string& value) {
    if (key == "displayName") {
        weapon.displayName = value;
    } else if (key == "category") {
        weapon.category = value;
    } else if (key == "damage") {
        weapon.damage = std::stof(value);
    } else if (key == "range") {
        weapon.range = std::stof(value);
    } else if (key == "accuracy") {
        weapon.accuracy = std::stof(value);
    } else if (key == "fireRate") {
        weapon.fireRate = std::stof(value);
    } else if (key == "reloadTime") {
        weapon.reloadTime = std::stof(value);
    } else if (key == "magazineSize") {
        weapon.magazineSize = std::stoi(value);
    } else if (key == "maxAmmo") {
        weapon.maxAmmo = std::stoi(value);
    } else if (key == "isAutomatic") {
        weapon.isAutomatic = StringUtils::ToBool(value);
    } else if (key == "penetration") {
        weapon.penetration = std::stof(value);
    } else if (key == "recoil") {
        weapon.recoil = std::stof(value);
    } else if (key == "weight") {
        weapon.weight = std::stof(value);
    } else if (key == "availableToTeam") {
        weapon.availableToTeams.push_back(value);
    } else if (key == "requiredClass") {
        weapon.requiredClass = value;
    } else if (key == "canPickup") {
        weapon.canPickup = StringUtils::ToBool(value);
    } else {
        Logger::Debug("Unknown weapon property: %s", key.c_str());
    }
}

void GameConfig::ParseTeamProperty(TeamDefinition& team, const std::string& key, const std::string& value) {
    if (key == "displayName") {
        team.displayName = value;
    } else if (key == "faction") {
        team.faction = value;
    } else if (key == "color") {
        team.color = value;
    } else if (key == "maxPlayers") {
        team.maxPlayers = std::stoi(value);
    } else if (key == "defaultClass") {
        team.defaultClass = value;
    } else if (key == "availableClass") {
        team.availableClasses.push_back(value);
    } else if (key == "spawnDelay") {
        team.spawnDelay = std::stof(value);
    } else if (key == "reinforcementWaves") {
        team.reinforcementWaves = StringUtils::ToBool(value);
    } else if (key == "waveSpawnTime") {
        team.waveSpawnTime = std::stof(value);
    } else if (key == "commanderAvailable") {
        team.commanderAvailable = StringUtils::ToBool(value);
    } else if (key == "uniqueWeapon") {
        team.uniqueWeapons.push_back(value);
    } else if (key == "teamAdvantage") {
        team.teamAdvantages.push_back(value);
    } else {
        Logger::Debug("Unknown team property: %s", key.c_str());
    }
}

bool GameConfig::IsValidMapDefinition(const MapDefinition& map) {
    return !map.name.empty() && 
           !map.displayName.empty() && 
           map.maxPlayers > 0 && 
           map.minPlayers > 0 && 
           map.maxPlayers >= map.minPlayers;
}

bool GameConfig::IsValidGameModeDefinition(const GameModeDefinition& mode) {
    return !mode.name.empty() && 
           !mode.displayName.empty() && 
           mode.maxPlayers > 0 && 
           mode.minPlayers > 0 && 
           mode.maxPlayers >= mode.minPlayers &&
           mode.teamCount >= 2;
}

bool GameConfig::IsValidWeaponDefinition(const WeaponDefinition& weapon) {
    return !weapon.name.empty() && 
           !weapon.displayName.empty() && 
           weapon.damage > 0.0f && 
           weapon.range > 0.0f &&
           weapon.magazineSize > 0;
}

bool GameConfig::IsValidTeamDefinition(const TeamDefinition& team) {
    return !team.name.empty() && 
           !team.displayName.empty() && 
           !team.faction.empty() &&
           team.maxPlayers > 0;
}

bool GameConfig::IsMapGameModeCompatible(const std::string& mapName, const std::string& gameMode) {
    auto mapIt = m_mapDefinitions.find(mapName);
    if (mapIt == m_mapDefinitions.end()) {
        return false;
    }
    
    const auto& supportedModes = mapIt->second.supportedGameModes;
    return std::find(supportedModes.begin(), supportedModes.end(), gameMode) != supportedModes.end();
}

void GameConfig::LoadDefaultMapDefinitions() {
    Logger::Debug("Loading default map definitions...");
    
    // VNTE-CuChi
    MapDefinition cuChi;
    cuChi.name = "VNTE-CuChi";
    cuChi.displayName = "Cu Chi";
    cuChi.description = "Underground tunnel complex with intense close-quarters combat";
    cuChi.supportedGameModes = {"Territories", "Supremacy", "Skirmish"};
    cuChi.maxPlayers = 64;
    cuChi.minPlayers = 16;
    cuChi.environment = "Jungle";
    cuChi.timeOfDay = "Day";
    cuChi.weather = "Clear";
    cuChi.sizeCategory = "Medium";
    cuChi.enableTunnels = true;
    cuChi.enableVehicles = false;
    cuChi.enableAirSupport = false;
    cuChi.objectiveCount = 5;
    cuChi.usTeamSpawns = 3;
    cuChi.nvaTeamSpawns = 4;
    m_mapDefinitions["VNTE-CuChi"] = cuChi;
    
    // VNTE-AnLao
    MapDefinition anLao;
    anLao.name = "VNTE-AnLao";
    anLao.displayName = "An Lao Valley";
    anLao.description = "Large scale battlefield with helicopter support";
    anLao.supportedGameModes = {"Territories", "Supremacy"};
    anLao.maxPlayers = 64;
    anLao.minPlayers = 20;
    anLao.environment = "Valley";
    anLao.timeOfDay = "Dawn";
    anLao.weather = "Foggy";
    anLao.sizeCategory = "Large";
    anLao.enableTunnels = false;
    anLao.enableVehicles = true;
    anLao.enableAirSupport = true;
    anLao.objectiveCount = 7;
    anLao.usTeamSpawns = 4;
    anLao.nvaTeamSpawns = 5;
    m_mapDefinitions["VNTE-AnLao"] = anLao;
    
    // VNTE-HueCity
    MapDefinition hueCity;
    hueCity.name = "VNTE-HueCity";
    hueCity.displayName = "Hue City";
    hueCity.description = "Urban warfare in the ancient imperial city";
    hueCity.supportedGameModes = {"Territories", "Supremacy", "Skirmish"};
    hueCity.maxPlayers = 64;
    hueCity.minPlayers = 16;
    hueCity.environment = "Urban";
    hueCity.timeOfDay = "Day";
    hueCity.weather = "Overcast";
    hueCity.sizeCategory = "Medium";
    hueCity.enableTunnels = false;
    hueCity.enableVehicles = true;
    hueCity.enableAirSupport = false;
    hueCity.objectiveCount = 6;
    hueCity.usTeamSpawns = 3;
    hueCity.nvaTeamSpawns = 4;
    m_mapDefinitions["VNTE-HueCity"] = hueCity;
    
    Logger::Info("Loaded %zu default map definitions", m_mapDefinitions.size());
}

void GameConfig::LoadDefaultGameModeDefinitions() {
    Logger::Debug("Loading default game mode definitions...");
    
    // Territories
    GameModeDefinition territories;
    territories.name = "Territories";
    territories.displayName = "Territories";
    territories.description = "Capture and hold territorial objectives";
    territories.category = "Objective";
    territories.maxPlayers = 64;
    territories.minPlayers = 16;
    territories.roundTimeLimit = 1800; // 30 minutes
    territories.preparationTime = 60;
    territories.respawnType = "Wave";
    territories.objectiveType = "Control";
    territories.teamCount = 2;
    territories.allowSpectators = true;
    territories.enableCommander = true;
    territories.friendlyFire = true;
    territories.autoBalance = true;
    territories.scoringSystem = "Territory";
    m_gameModeDefinitions["Territories"] = territories;
    
    // Supremacy
    GameModeDefinition supremacy;
    supremacy.name = "Supremacy";
    supremacy.displayName = "Supremacy";
    supremacy.description = "Large scale warfare with multiple objectives";
    supremacy.category = "Large Scale";
    supremacy.maxPlayers = 64;
    supremacy.minPlayers = 20;
    supremacy.roundTimeLimit = 2400; // 40 minutes
    supremacy.preparationTime = 90;
    supremacy.respawnType = "Wave";
    supremacy.objectiveType = "Sequential";
    supremacy.teamCount = 2;
    supremacy.allowSpectators = true;
    supremacy.enableCommander = true;
    supremacy.friendlyFire = true;
    supremacy.autoBalance = true;
    supremacy.scoringSystem = "Elimination";
    m_gameModeDefinitions["Supremacy"] = supremacy;
    
    // Skirmish
    GameModeDefinition skirmish;
    skirmish.name = "Skirmish";
    skirmish.displayName = "Skirmish";
    skirmish.description = "Fast-paced small unit combat";
    skirmish.category = "Small Scale";
    skirmish.maxPlayers = 32;
    skirmish.minPlayers = 8;
    skirmish.roundTimeLimit = 900; // 15 minutes
    skirmish.preparationTime = 30;
    skirmish.respawnType = "Individual";
    skirmish.objectiveType = "Elimination";
    skirmish.teamCount = 2;
    skirmish.allowSpectators = true;
    skirmish.enableCommander = false;
    skirmish.friendlyFire = true;
    skirmish.autoBalance = true;
    skirmish.scoringSystem = "Kill";
    m_gameModeDefinitions["Skirmish"] = skirmish;
    
    Logger::Info("Loaded %zu default game mode definitions", m_gameModeDefinitions.size());
}

void GameConfig::LoadDefaultWeaponConfigurations() {
    Logger::Debug("Loading default weapon configurations...");
    
    // M16A1
    WeaponDefinition m16a1;
    m16a1.name = "M16A1";
    m16a1.displayName = "M16A1 Assault Rifle";
    m16a1.category = "AssaultRifle";
    m16a1.damage = 65.0f;
    m16a1.range = 300.0f;
    m16a1.accuracy = 0.85f;
    m16a1.fireRate = 750.0f; // RPM
    m16a1.reloadTime = 3.2f;
    m16a1.magazineSize = 20;
    m16a1.maxAmmo = 200;
    m16a1.isAutomatic = true;
    m16a1.penetration = 0.7f;
    m16a1.recoil = 0.6f;
    m16a1.weight = 3.4f;
    m16a1.availableToTeams = {"US", "ARVN"};
    m16a1.requiredClass = "Rifleman";
    m16a1.canPickup = true;
    m_weaponDefinitions["M16A1"] = m16a1;
    
    // AK-47
    WeaponDefinition ak47;
    ak47.name = "AK47";
    ak47.displayName = "AK-47 Assault Rifle";
    ak47.category = "AssaultRifle";
    ak47.damage = 70.0f;
    ak47.range = 280.0f;
    ak47.accuracy = 0.78f;
    ak47.fireRate = 600.0f; // RPM
    ak47.reloadTime = 3.5f;
    ak47.magazineSize = 30;
    ak47.maxAmmo = 180;
    ak47.isAutomatic = true;
    ak47.penetration = 0.8f;
    ak47.recoil = 0.8f;
    ak47.weight = 4.3f;
    ak47.availableToTeams = {"NVA", "VC"};
    ak47.requiredClass = "Rifleman";
    ak47.canPickup = true;
    m_weaponDefinitions["AK47"] = ak47;
    
    Logger::Info("Loaded %zu default weapon definitions", m_weaponDefinitions.size());
}

void GameConfig::LoadDefaultTeamConfigurations() {
    Logger::Debug("Loading default team configurations...");
    
    // US Army
    TeamDefinition usArmy;
    usArmy.name = "US";
    usArmy.displayName = "US Army";
    usArmy.faction = "United States";
    usArmy.color = "Green";
    usArmy.maxPlayers = 32;
    usArmy.defaultClass = "Rifleman";
    usArmy.availableClasses = {"Rifleman", "Grenadier", "MachineGunner", "Sniper", "Engineer", "Medic", "RadioOperator"};
    usArmy.spawnDelay = 10.0f;
    usArmy.reinforcementWaves = true;
    usArmy.waveSpawnTime = 30.0f;
    usArmy.commanderAvailable = true;
    usArmy.uniqueWeapons = {"M16A1", "M60", "M14", "M79"};
    usArmy.teamAdvantages = {"Air Support", "Artillery", "Better Equipment"};
    m_teamDefinitions["US"] = usArmy;
    
    // North Vietnamese Army
    TeamDefinition nva;
    nva.name = "NVA";
    nva.displayName = "North Vietnamese Army";
    nva.faction = "North Vietnam";
    nva.color = "Red";
    nva.maxPlayers = 32;
    nva.defaultClass = "Rifleman";
    nva.availableClasses = {"Rifleman", "Grenadier", "MachineGunner", "Sniper", "Sapper", "Medic", "RadioOperator"};
    nva.spawnDelay = 8.0f;
    nva.reinforcementWaves = true;
    nva.waveSpawnTime = 25.0f;
    nva.commanderAvailable = true;
    nva.uniqueWeapons = {"AK47", "RPD", "SKS", "RPG7"};
    nva.teamAdvantages = {"Tunnels", "Traps", "Local Knowledge"};
    m_teamDefinitions["NVA"] = nva;
    
    Logger::Info("Loaded %zu default team definitions", m_teamDefinitions.size());
}

void GameConfig::InitializeDefaultGameSettings() {
    Logger::Debug("Initializing default game settings...");
    
    // Set all default values for game settings
    m_gameSettings.mapName = "VNTE-CuChi";
    m_gameSettings.gameMode = "Territories";
    m_gameSettings.maxPlayers = 64;
    m_gameSettings.roundTimeLimit = 1800;
    m_gameSettings.preparationTime = 60;
    m_gameSettings.respawnDelay = 10.0f;
    m_gameSettings.allowTeamSwitch = true;
    m_gameSettings.autoBalanceTeams = true;
    m_gameSettings.teamBalanceTolerance = 2;
    m_gameSettings.friendlyFireScale = 0.5f;
    m_gameSettings.allowTK = false;
    m_gameSettings.tkPunishmentThreshold = 3;
    m_gameSettings.tkKickThreshold = 5;
    m_gameSettings.weaponRespawnTime = 30.0f;
    m_gameSettings.allowWeaponPickup = true;
    m_gameSettings.unlimitedAmmo = false;
    m_gameSettings.realismMode = true;
    m_gameSettings.enableVehicles = true;
    m_gameSettings.vehicleRespawnTime = 120.0f;
    m_gameSettings.vehicleDamageScale = 1.0f;
    m_gameSettings.captureTime = 30.0f;
    m_gameSettings.neutralizationTime = 15.0f;
    m_gameSettings.objectiveInfluenceRadius = 50.0f;
    m_gameSettings.allowSpectators = true;
    m_gameSettings.maxSpectators = 8;
    m_gameSettings.enableVoiceChat = true;
    m_gameSettings.enableTextChat = true;
    m_gameSettings.allowChatDuringDeath = false;
    m_gameSettings.enableTunnels = true;
    m_gameSettings.enableTraps = true;
    m_gameSettings.enableAmbientEffects = true;
    m_gameSettings.napalmEnabled = true;
    m_gameSettings.helicopterSupport = true;
    m_gameSettings.enableCommander = true;
    m_gameSettings.commanderVoteTime = 60;
    m_gameSettings.allowCommanderMutiny = true;
    
    Logger::Debug("Default game settings initialized");
}

void GameConfig::LogGameConfigurationSummary() {
    Logger::Info("=== Game Configuration Summary ===");
    Logger::Info("Map: %s", m_gameSettings.mapName.c_str());
    Logger::Info("Game Mode: %s", m_gameSettings.gameMode.c_str());
    Logger::Info("Max Players: %d", m_gameSettings.maxPlayers);
    Logger::Info("Round Time: %d seconds", m_gameSettings.roundTimeLimit);
    Logger::Info("Friendly Fire Scale: %.2f", m_gameSettings.friendlyFireScale);
    Logger::Info("Realism Mode: %s", m_gameSettings.realismMode ? "Enabled" : "Disabled");
    Logger::Info("Vehicles: %s", m_gameSettings.enableVehicles ? "Enabled" : "Disabled");
    Logger::Info("Tunnels: %s", m_gameSettings.enableTunnels ? "Enabled" : "Disabled");
    Logger::Info("Commander: %s", m_gameSettings.enableCommander ? "Enabled" : "Disabled");
    Logger::Info("Maps Loaded: %zu", m_mapDefinitions.size());
    Logger::Info("Game Modes Loaded: %zu", m_gameModeDefinitions.size());
    Logger::Info("Weapons Loaded: %zu", m_weaponDefinitions.size());
    Logger::Info("Teams Loaded: %zu", m_teamDefinitions.size());
    Logger::Info("================================");
}

// Getters for game configuration
const GameSettings& GameConfig::GetGameSettings() const {
    return m_gameSettings;
}

const MapDefinition* GameConfig::GetMapDefinition(const std::string& mapName) const {
    auto it = m_mapDefinitions.find(mapName);
    return it != m_mapDefinitions.end() ? &it->second : nullptr;
}

const GameModeDefinition* GameConfig::GetGameModeDefinition(const std::string& gameMode) const {
    auto it = m_gameModeDefinitions.find(gameMode);
    return it != m_gameModeDefinitions.end() ? &it->second : nullptr;
}

const WeaponDefinition* GameConfig::GetWeaponDefinition(const std::string& weaponName) const {
    auto it = m_weaponDefinitions.find(weaponName);
    return it != m_weaponDefinitions.end() ? &it->second : nullptr;
}

const TeamDefinition* GameConfig::GetTeamDefinition(const std::string& teamName) const {
    auto it = m_teamDefinitions.find(teamName);
    return it != m_teamDefinitions.end() ? &it->second : nullptr;
}

std::vector<std::string> GameConfig::GetAvailableMaps() const {
    std::vector<std::string> maps;
    for (const auto& [name, def] : m_mapDefinitions) {
        maps.push_back(name);
    }
    return maps;
}

std::vector<std::string> GameConfig::GetAvailableGameModes() const {
    std::vector<std::string> modes;
    for (const auto& [name, def] : m_gameModeDefinitions) {
        modes.push_back(name);
    }
    return modes;
}

std::vector<std::string> GameConfig::GetCompatibleGameModes(const std::string& mapName) const {
    auto mapDef = GetMapDefinition(mapName);
    return mapDef ? mapDef->supportedGameModes : std::vector<std::string>();
}

std::vector<std::string> GameConfig::GetAvailableWeapons(const std::string& teamName) const {
    std::vector<std::string> weapons;
    for (const auto& [name, def] : m_weaponDefinitions) {
        if (std::find(def.availableToTeams.begin(), def.availableToTeams.end(), teamName) != def.availableToTeams.end()) {
            weapons.push_back(name);
        }
    }
    return weapons;
}

bool GameConfig::SaveGameConfiguration() {
    if (!m_configManager) {
        Logger::Error("ConfigManager is null, cannot save game configuration");
        return false;
    }
    
    Logger::Info("Saving game configuration...");
    
    // Save all game settings back to config manager
    m_configManager->SetString("Game.MapName", m_gameSettings.mapName);
    m_configManager->SetString("Game.GameMode", m_gameSettings.gameMode);
    m_configManager->SetInt("Game.MaxPlayers", m_gameSettings.maxPlayers);
    m_configManager->SetInt("Game.RoundTimeLimit", m_gameSettings.roundTimeLimit);
    m_configManager->SetFloat("Game.FriendlyFireScale", m_gameSettings.friendlyFireScale);
    m_configManager->SetBool("Game.RealismMode", m_gameSettings.realismMode);
    m_configManager->SetBool("Game.EnableVehicles", m_gameSettings.enableVehicles);
    m_configManager->SetBool("Game.EnableTunnels", m_gameSettings.enableTunnels);
    m_configManager->SetBool("Game.EnableCommander", m_gameSettings.enableCommander);
    
    // Save to file if auto-save is enabled
    if (m_configManager->IsAutoSaveEnabled()) {
        return m_configManager->SaveConfiguration("config/server.ini");
    }
    
    Logger::Info("Game configuration saved successfully");
    return true;
}

bool GameConfig::ReloadGameConfiguration() {
    Logger::Info("Reloading game configuration...");
    
    // Reload from config manager
    if (!LoadGameConfiguration()) {
        Logger::Error("Failed to reload game configuration");
        return false;
    }
    
    // Re-validate
    if (!ValidateGameConfiguration()) {
        Logger::Error("Reloaded game configuration validation failed");
        return false;
    }
    
    Logger::Info("Game configuration reloaded successfully");
    LogGameConfigurationSummary();
    
    return true;
}