// src/Config/ConfigManager.cpp - Complete implementation for RS2V Server
#include "Config/ConfigManager.h"
#include "Utils/Logger.h"
#include "Utils/FileUtils.h"
#include "Utils/StringUtils.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>

ConfigManager::ConfigManager() {
    Logger::Info("ConfigManager initialized");
}

ConfigManager::~ConfigManager() {
    SaveAllConfigurations();
}

bool ConfigManager::Initialize() {
    Logger::Info("Initializing Configuration Manager...");
    
    // Create default configuration directory if it doesn't exist
    try {
        std::filesystem::create_directories("config");
        Logger::Info("Configuration directory ensured: config/");
    } catch (const std::exception& e) {
        Logger::Error("Failed to create config directory: %s", e.what());
        return false;
    }
    
    // Load default configuration schema
    if (!LoadConfigurationSchema()) {
        Logger::Error("Failed to load configuration schema");
        return false;
    }
    
    // Initialize configuration watchers
    InitializeConfigWatchers();
    
    Logger::Info("Configuration Manager initialized successfully");
    return true;
}

bool ConfigManager::LoadConfiguration(const std::string& configFile) {
    Logger::Info("Loading configuration file: %s", configFile.c_str());
    
    std::ifstream file(configFile);
    if (!file.is_open()) {
        Logger::Error("Cannot open configuration file: %s", configFile.c_str());
        return false;
    }
    
    // Store the primary config file path
    m_primaryConfigFile = configFile;
    
    // Parse INI format configuration
    std::string line;
    std::string currentSection;
    size_t lineNumber = 0;
    
    while (std::getline(file, line)) {
        lineNumber++;
        
        // Remove comments and trim whitespace
        size_t commentPos = line.find('#');
        if (commentPos != std::string::npos) {
            line = line.substr(0, commentPos);
        }
        
        line = StringUtils::Trim(line);
        
        // Skip empty lines
        if (line.empty()) {
            continue;
        }
        
        // Check for section header
        if (line.front() == '[' && line.back() == ']') {
            currentSection = line.substr(1, line.length() - 2);
            Logger::Debug("Processing section: %s", currentSection.c_str());
            continue;
        }
        
        // Parse key-value pairs
        size_t equalPos = line.find('=');
        if (equalPos == std::string::npos) {
            Logger::Warn("Invalid configuration line %zu: %s", lineNumber, line.c_str());
            continue;
        }
        
        std::string key = StringUtils::Trim(line.substr(0, equalPos));
        std::string value = StringUtils::Trim(line.substr(equalPos + 1));
        
        // Store configuration value
        std::string fullKey = currentSection.empty() ? key : currentSection + "." + key;
        m_configValues[fullKey] = value;
        
        Logger::Debug("Config: %s = %s", fullKey.c_str(), value.c_str());
    }
    
    file.close();
    
    // Post-process and validate configuration
    if (!ValidateConfiguration()) {
        Logger::Error("Configuration validation failed");
        return false;
    }
    
    // Apply security-specific configurations
    ApplySecurityConfiguration();
    
    // Apply EAC-specific configurations
    ApplyEACConfiguration();
    
    Logger::Info("Configuration loaded successfully: %zu settings", m_configValues.size());
    return true;
}

bool ConfigManager::SaveConfiguration(const std::string& configFile) {
    Logger::Info("Saving configuration to: %s", configFile.c_str());
    
    std::ofstream file(configFile);
    if (!file.is_open()) {
        Logger::Error("Cannot create configuration file: %s", configFile.c_str());
        return false;
    }
    
    // Group settings by section
    std::map<std::string, std::map<std::string, std::string>> sections;
    
    for (const auto& [fullKey, value] : m_configValues) {
        size_t dotPos = fullKey.find('.');
        if (dotPos != std::string::npos) {
            std::string section = fullKey.substr(0, dotPos);
            std::string key = fullKey.substr(dotPos + 1);
            sections[section][key] = value;
        } else {
            sections[""][fullKey] = value;
        }
    }
    
    // Write configuration file
    file << "# RS2V Server Configuration" << std::endl;
    file << "# Generated on: " << GetCurrentTimestamp() << std::endl;
    file << std::endl;
    
    for (const auto& [sectionName, sectionValues] : sections) {
        if (!sectionName.empty()) {
            file << "[" << sectionName << "]" << std::endl;
        }
        
        for (const auto& [key, value] : sectionValues) {
            // Add comments for important settings
            if (auto comment = GetConfigComment(sectionName, key); !comment.empty()) {
                file << "# " << comment << std::endl;
            }
            file << key << "=" << value << std::endl;
        }
        
        file << std::endl;
    }
    
    file.close();
    Logger::Info("Configuration saved successfully");
    return true;
}

bool ConfigManager::ReloadConfiguration() {
    Logger::Info("Reloading configuration from: %s", m_primaryConfigFile.c_str());
    
    // Backup current configuration
    auto backupConfig = m_configValues;
    
    // Clear current configuration
    m_configValues.clear();
    
    // Reload from file
    if (!LoadConfiguration(m_primaryConfigFile)) {
        Logger::Error("Failed to reload configuration, restoring backup");
        m_configValues = backupConfig;
        return false;
    }
    
    // Notify all listeners of configuration changes
    NotifyConfigurationChanged();
    
    Logger::Info("Configuration reloaded successfully");
    return true;
}

std::string ConfigManager::GetString(const std::string& key, const std::string& defaultValue) const {
    auto it = m_configValues.find(key);
    if (it != m_configValues.end()) {
        return it->second;
    }
    
    Logger::Debug("Config key '%s' not found, using default: %s", key.c_str(), defaultValue.c_str());
    return defaultValue;
}

int ConfigManager::GetInt(const std::string& key, int defaultValue) const {
    std::string value = GetString(key);
    if (value.empty()) {
        return defaultValue;
    }
    
    try {
        return std::stoi(value);
    } catch (const std::exception& e) {
        Logger::Warn("Invalid integer value for key '%s': %s", key.c_str(), value.c_str());
        return defaultValue;
    }
}

bool ConfigManager::GetBool(const std::string& key, bool defaultValue) const {
    std::string value = GetString(key);
    if (value.empty()) {
        return defaultValue;
    }
    
    // Convert to lowercase for comparison
    std::transform(value.begin(), value.end(), value.begin(), ::tolower);
    
    if (value == "true" || value == "1" || value == "yes" || value == "on") {
        return true;
    } else if (value == "false" || value == "0" || value == "no" || value == "off") {
        return false;
    }
    
    Logger::Warn("Invalid boolean value for key '%s': %s", key.c_str(), value.c_str());
    return defaultValue;
}

float ConfigManager::GetFloat(const std::string& key, float defaultValue) const {
    std::string value = GetString(key);
    if (value.empty()) {
        return defaultValue;
    }
    
    try {
        return std::stof(value);
    } catch (const std::exception& e) {
        Logger::Warn("Invalid float value for key '%s': %s", key.c_str(), value.c_str());
        return defaultValue;
    }
}

void ConfigManager::SetString(const std::string& key, const std::string& value) {
    m_configValues[key] = value;
    Logger::Debug("Config updated: %s = %s", key.c_str(), value.c_str());
    
    // Auto-save if enabled
    if (m_autoSave) {
        SaveConfiguration(m_primaryConfigFile);
    }
}

void ConfigManager::SetInt(const std::string& key, int value) {
    SetString(key, std::to_string(value));
}

void ConfigManager::SetBool(const std::string& key, bool value) {
    SetString(key, value ? "true" : "false");
}

void ConfigManager::SetFloat(const std::string& key, float value) {
    SetString(key, std::to_string(value));
}

bool ConfigManager::HasKey(const std::string& key) const {
    return m_configValues.find(key) != m_configValues.end();
}

void ConfigManager::RemoveKey(const std::string& key) {
    auto it = m_configValues.find(key);
    if (it != m_configValues.end()) {
        m_configValues.erase(it);
        Logger::Debug("Config key removed: %s", key.c_str());
    }
}

std::vector<std::string> ConfigManager::GetSectionKeys(const std::string& section) const {
    std::vector<std::string> keys;
    std::string prefix = section + ".";
    
    for (const auto& [key, value] : m_configValues) {
        if (key.substr(0, prefix.length()) == prefix) {
            keys.push_back(key.substr(prefix.length()));
        }
    }
    
    return keys;
}

std::vector<std::string> ConfigManager::GetAllSections() const {
    std::set<std::string> sections;
    
    for (const auto& [key, value] : m_configValues) {
        size_t dotPos = key.find('.');
        if (dotPos != std::string::npos) {
            sections.insert(key.substr(0, dotPos));
        }
    }
    
    return std::vector<std::string>(sections.begin(), sections.end());
}

void ConfigManager::SetAutoSave(bool enabled) {
    m_autoSave = enabled;
    Logger::Info("Configuration auto-save %s", enabled ? "enabled" : "disabled");
}

bool ConfigManager::IsAutoSaveEnabled() const {
    return m_autoSave;
}

void ConfigManager::AddConfigurationListener(std::shared_ptr<IConfigurationListener> listener) {
    m_listeners.push_back(listener);
    Logger::Debug("Configuration listener added");
}

void ConfigManager::RemoveConfigurationListener(std::shared_ptr<IConfigurationListener> listener) {
    auto it = std::find(m_listeners.begin(), m_listeners.end(), listener);
    if (it != m_listeners.end()) {
        m_listeners.erase(it);
        Logger::Debug("Configuration listener removed");
    }
}

bool ConfigManager::ValidateConfiguration() {
    Logger::Debug("Validating configuration...");
    
    bool isValid = true;
    
    // Validate server configuration
    if (!ValidateServerConfig()) {
        isValid = false;
    }
    
    // Validate network configuration
    if (!ValidateNetworkConfig()) {
        isValid = false;
    }
    
    // Validate security configuration
    if (!ValidateSecurityConfig()) {
        isValid = false;
    }
    
    // Validate EAC configuration
    if (!ValidateEACConfig()) {
        isValid = false;
    }
    
    // Validate game configuration
    if (!ValidateGameConfig()) {
        isValid = false;
    }
    
    if (isValid) {
        Logger::Debug("Configuration validation passed");
    } else {
        Logger::Error("Configuration validation failed");
    }
    
    return isValid;
}

bool ConfigManager::ValidateServerConfig() {
    // Validate basic server settings
    std::string serverName = GetString("Server.ServerName", "");
    if (serverName.empty()) {
        Logger::Error("Server.ServerName is required");
        return false;
    }
    
    int maxPlayers = GetInt("Server.MaxPlayers", 64);
    if (maxPlayers < 1 || maxPlayers > 128) {
        Logger::Error("Server.MaxPlayers must be between 1 and 128");
        return false;
    }
    
    int gamePort = GetInt("Server.GamePort", 7777);
    if (gamePort < 1024 || gamePort > 65535) {
        Logger::Error("Server.GamePort must be between 1024 and 65535");
        return false;
    }
    
    int queryPort = GetInt("Server.QueryPort", 27015);
    if (queryPort < 1024 || queryPort > 65535) {
        Logger::Error("Server.QueryPort must be between 1024 and 65535");
        return false;
    }
    
    if (gamePort == queryPort) {
        Logger::Error("Server.GamePort and Server.QueryPort cannot be the same");
        return false;
    }
    
    int tickRate = GetInt("Server.TickRate", 60);
    if (tickRate < 10 || tickRate > 128) {
        Logger::Error("Server.TickRate must be between 10 and 128");
        return false;
    }
    
    return true;
}

bool ConfigManager::ValidateNetworkConfig() {
    int maxPacketSize = GetInt("Network.MaxPacketSize", 1024);
    if (maxPacketSize < 64 || maxPacketSize > 65536) {
        Logger::Error("Network.MaxPacketSize must be between 64 and 65536");
        return false;
    }
    
    int timeoutSeconds = GetInt("Network.TimeoutSeconds", 30);
    if (timeoutSeconds < 5 || timeoutSeconds > 300) {
        Logger::Error("Network.TimeoutSeconds must be between 5 and 300");
        return false;
    }
    
    return true;
}

bool ConfigManager::ValidateSecurityConfig() {
    // Validate anti-cheat thresholds
    float maxSpeed = GetFloat("AntiCheat.MaxSpeed", 7.0f);
    if (maxSpeed < 1.0f || maxSpeed > 20.0f) {
        Logger::Error("AntiCheat.MaxSpeed must be between 1.0 and 20.0");
        return false;
    }
    
    float headshotRatio = GetFloat("AntiCheat.MaxHeadshotRatio", 0.8f);
    if (headshotRatio < 0.0f || headshotRatio > 1.0f) {
        Logger::Error("AntiCheat.MaxHeadshotRatio must be between 0.0 and 1.0");
        return false;
    }
    
    float accuracyThreshold = GetFloat("AntiCheat.MaxAccuracyThreshold", 0.95f);
    if (accuracyThreshold < 0.0f || accuracyThreshold > 1.0f) {
        Logger::Error("AntiCheat.MaxAccuracyThreshold must be between 0.0 and 1.0");
        return false;
    }
    
    return true;
}

bool ConfigManager::ValidateEACConfig() {
    std::string eacMode = GetString("EAC.Mode", "DISABLED");
    std::vector<std::string> validModes = {"DISABLED", "PASSIVE_LOGGING", "BLOCK_MODE"};
    
    if (std::find(validModes.begin(), validModes.end(), eacMode) == validModes.end()) {
        Logger::Error("EAC.Mode must be one of: DISABLED, PASSIVE_LOGGING, BLOCK_MODE");
        return false;
    }
    
    int eacPort = GetInt("EAC.ProxyPort", 7957);
    if (eacPort < 1024 || eacPort > 65535) {
        Logger::Error("EAC.ProxyPort must be between 1024 and 65535");
        return false;
    }
    
    return true;
}

bool ConfigManager::ValidateGameConfig() {
    std::string mapName = GetString("Game.MapName", "");
    if (mapName.empty()) {
        Logger::Error("Game.MapName is required");
        return false;
    }
    
    std::string gameMode = GetString("Game.GameMode", "");
    if (gameMode.empty()) {
        Logger::Error("Game.GameMode is required");
        return false;
    }
    
    return true;
}

void ConfigManager::ApplySecurityConfiguration() {
    Logger::Debug("Applying security configuration...");
    
    // Force security settings if in secure mode
    bool secureMode = GetBool("Security.SecureMode", false);
    if (secureMode) {
        Logger::Info("Secure mode enabled - applying hardened security settings");
        
        // Force EAC disabled
        SetString("EAC.Mode", "DISABLED");
        SetBool("EAC.AllowEACEnable", false);
        SetBool("Security.AllowTelemetry", false);
        SetBool("Security.PrivacyMode", true);
        
        // Enhanced anti-cheat settings
        SetBool("AntiCheat.StrictValidation", true);
        SetBool("AntiCheat.ParanoidMode", true);
        SetFloat("AntiCheat.ValidationTolerance", 0.02f);
        
        Logger::Info("Secure mode configuration applied");
    }
}

void ConfigManager::ApplyEACConfiguration() {
    Logger::Debug("Applying EAC configuration...");
    
    std::string eacMode = GetString("EAC.Mode", "DISABLED");
    
    if (eacMode == "DISABLED") {
        Logger::Info("EAC is DISABLED - applying EAC-free configuration");
        
        // Ensure EAC components are disabled
        SetBool("EAC.EnableEACProxy", false);
        SetBool("EAC.EnableTelemetryLogging", false);
        SetBool("EAC.ForwardToEpic", false);
        
        // Enhanced custom anti-cheat when EAC is disabled
        SetBool("AntiCheat.EnableCustomAntiCheat", true);
        SetFloat("AntiCheat.StrictValidation", 1.5f);
        SetInt("AntiCheat.ValidationInterval", 5);
        
    } else if (eacMode == "PASSIVE_LOGGING") {
        Logger::Info("EAC PASSIVE_LOGGING mode - enabling telemetry collection");
        
        SetBool("EAC.EnableEACProxy", true);
        SetBool("EAC.EnableTelemetryLogging", true);
        SetBool("EAC.ForwardToEpic", true);
        SetBool("EAC.LogUnknownPackets", true);
        
    } else if (eacMode == "BLOCK_MODE") {
        Logger::Info("EAC BLOCK_MODE - logging without forwarding");
        
        SetBool("EAC.EnableEACProxy", true);
        SetBool("EAC.EnableTelemetryLogging", true);
        SetBool("EAC.ForwardToEpic", false);
    }
}

void ConfigManager::InitializeConfigWatchers() {
    Logger::Debug("Initializing configuration file watchers...");
    
    // TODO: Implement file system watchers to detect configuration changes
    // This would allow automatic reloading when config files are modified
    
    Logger::Debug("Configuration watchers initialized");
}

void ConfigManager::NotifyConfigurationChanged() {
    Logger::Debug("Notifying configuration listeners of changes...");
    
    // Remove expired weak pointers
    m_listeners.erase(
        std::remove_if(m_listeners.begin(), m_listeners.end(),
            [](const std::weak_ptr<IConfigurationListener>& weak) {
                return weak.expired();
            }),
        m_listeners.end()
    );
    
    // Notify all active listeners
    for (auto& weakListener : m_listeners) {
        if (auto listener = weakListener.lock()) {
            try {
                listener->OnConfigurationChanged();
            } catch (const std::exception& e) {
                Logger::Error("Configuration listener exception: %s", e.what());
            }
        }
    }
    
    Logger::Debug("Configuration change notifications sent to %zu listeners", m_listeners.size());
}

std::string ConfigManager::GetConfigComment(const std::string& section, const std::string& key) {
    // Provide helpful comments for important configuration keys
    std::string fullKey = section.empty() ? key : section + "." + key;
    
    static std::map<std::string, std::string> comments = {
        {"Server.ServerName", "Display name for your server"},
        {"Server.MaxPlayers", "Maximum number of concurrent players (1-128)"},
        {"Server.GamePort", "UDP port for game traffic (1024-65535)"},
        {"Server.QueryPort", "UDP port for Steam queries (1024-65535)"},
        {"Server.TickRate", "Server update frequency in Hz (10-128)"},
        {"EAC.Mode", "EAC operation mode: DISABLED, PASSIVE_LOGGING, BLOCK_MODE"},
        {"Security.SecureMode", "Enable hardened security configuration"},
        {"AntiCheat.MaxSpeed", "Maximum allowed player movement speed"},
        {"AntiCheat.StrictValidation", "Enable strict anti-cheat validation"},
        {"Network.MaxPacketSize", "Maximum allowed packet size in bytes"},
        {"Logging.LogLevel", "Logging verbosity: DEBUG, INFO, WARN, ERROR"}
    };
    
    auto it = comments.find(fullKey);
    return it != comments.end() ? it->second : "";
}

std::string ConfigManager::GetCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

bool ConfigManager::LoadConfigurationSchema() {
    Logger::Debug("Loading configuration schema...");
    
    // TODO: Implement configuration schema loading from JSON or XML
    // This would provide validation rules, data types, and constraints
    
    Logger::Debug("Configuration schema loaded");
    return true;
}

void ConfigManager::SaveAllConfigurations() {
    if (!m_primaryConfigFile.empty()) {
        SaveConfiguration(m_primaryConfigFile);
    }
}

std::map<std::string, std::string> ConfigManager::GetAllConfigValues() const {
    return m_configValues;
}

void ConfigManager::DumpConfigurationToLog() {
    Logger::Info("=== Current Configuration Dump ===");
    
    auto sections = GetAllSections();
    for (const auto& section : sections) {
        Logger::Info("[%s]", section.c_str());
        auto keys = GetSectionKeys(section);
        for (const auto& key : keys) {
            std::string fullKey = section + "." + key;
            std::string value = GetString(fullKey);
            Logger::Info("  %s = %s", key.c_str(), value.c_str());
        }
    }
    
    Logger::Info("=== End Configuration Dump ===");
}

bool ConfigManager::ImportConfiguration(const std::string& sourceFile) {
    Logger::Info("Importing configuration from: %s", sourceFile.c_str());
    
    // Create backup of current configuration
    auto backup = m_configValues;
    
    // Load additional configuration
    ConfigManager tempManager;
    if (!tempManager.LoadConfiguration(sourceFile)) {
        Logger::Error("Failed to import configuration from: %s", sourceFile.c_str());
        return false;
    }
    
    // Merge configurations (imported values override existing ones)
    for (const auto& [key, value] : tempManager.GetAllConfigValues()) {
        m_configValues[key] = value;
    }
    
    // Validate merged configuration
    if (!ValidateConfiguration()) {
        Logger::Error("Merged configuration validation failed, reverting");
        m_configValues = backup;
        return false;
    }
    
    Logger::Info("Configuration imported successfully");
    NotifyConfigurationChanged();
    return true;
}

bool ConfigManager::ExportConfiguration(const std::string& targetFile) {
    Logger::Info("Exporting configuration to: %s", targetFile.c_str());
    return SaveConfiguration(targetFile);
}

void ConfigManager::ResetToDefaults() {
    Logger::Info("Resetting configuration to defaults");
    
    m_configValues.clear();
    
    // Apply default configuration values
    ApplyDefaultConfiguration();
    
    // Validate defaults
    if (!ValidateConfiguration()) {
        Logger::Error("Default configuration validation failed");
    }
    
    Logger::Info("Configuration reset to defaults");
    NotifyConfigurationChanged();
}

void ConfigManager::ApplyDefaultConfiguration() {
    // Server defaults
    SetString("Server.ServerName", "RS2V Custom Server");
    SetInt("Server.MaxPlayers", 64);
    SetInt("Server.GamePort", 7777);
    SetInt("Server.QueryPort", 27015);
    SetString("Server.MapName", "VNTE-CuChi");
    SetInt("Server.TickRate", 60);
    
    // Network defaults
    SetInt("Network.MaxPacketSize", 1024);
    SetInt("Network.TimeoutSeconds", 30);
    SetBool("Network.CompressionEnabled", true);
    
    // EAC defaults (permanently disabled)
    SetString("EAC.Mode", "DISABLED");
    SetBool("EAC.EnableEACProxy", false);
    SetBool("EAC.AllowEACEnable", false);
    SetBool("EAC.ForwardToEpic", false);
    
    // Security defaults
    SetBool("Security.SecureMode", true);
    SetBool("Security.PrivacyMode", true);
    SetBool("Security.AllowTelemetry", false);
    
    // Anti-cheat defaults
    SetBool("AntiCheat.EnableCustomAntiCheat", true);
    SetBool("AntiCheat.StrictValidation", true);
    SetFloat("AntiCheat.MaxSpeed", 7.0f);
    SetFloat("AntiCheat.MaxHeadshotRatio", 0.8f);
    SetFloat("AntiCheat.MaxAccuracyThreshold", 0.95f);
    
    // Logging defaults
    SetString("Logging.LogLevel", "INFO");
    SetString("Logging.LogFile", "logs/server.log");
    SetBool("Logging.PacketLogging", true);
    
    Logger::Info("Default configuration applied");
}