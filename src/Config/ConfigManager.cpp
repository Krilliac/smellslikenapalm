// src/Config/ConfigManager.cpp

#include "Config/ConfigManager.h"
#include "Utils/Logger.h"
#include "Utils/FileUtils.h"
#include "Utils/StringUtils.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <set>
#include <map>
#include <vector>

ConfigManager::ConfigManager() {
    Logger::Info("ConfigManager initialized");
}

ConfigManager::~ConfigManager() {
    SaveAllConfigurations();
}

bool ConfigManager::Initialize() {
    Logger::Info("Initializing Configuration Manager...");
    
    // Ensure config directory exists
    try {
        std::filesystem::create_directories("config");
        Logger::Info("Configuration directory ensured: config/");
    } catch (const std::exception& e) {
        Logger::Error("Failed to create config directory: %s", e.what());
        return false;
    }
    
    // Load unified server.ini
    const std::string mainConfig = "config/server.ini";
    if (!LoadConfiguration(mainConfig)) {
        Logger::Error("Failed to load main configuration: %s", mainConfig.c_str());
        return false;
    }
    
    // Initialize file-watchers (placeholder)
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
    
    m_primaryConfigFile = configFile;
    m_configValues.clear();

    std::string line;
    std::string currentSection;
    size_t lineNumber = 0;

    while (std::getline(file, line)) {
        ++lineNumber;
        // Strip comments
        if (auto pos = line.find('#'); pos != std::string::npos) {
            line.erase(pos);
        }
        line = StringUtils::Trim(line);
        if (line.empty()) continue;

        // Section header
        if (line.front() == '[' && line.back() == ']') {
            currentSection = line.substr(1, line.size() - 2);
            Logger::Debug("Section: %s", currentSection.c_str());
            continue;
        }

        // Parse key=value
        auto eq = line.find('=');
        if (eq == std::string::npos) {
            Logger::Warn("Invalid line %zu: %s", lineNumber, line.c_str());
            continue;
        }
        std::string key   = StringUtils::Trim(line.substr(0, eq));
        std::string value = StringUtils::Trim(line.substr(eq + 1));

        std::string fullKey = currentSection.empty() ? key : (currentSection + "." + key);
        m_configValues[fullKey] = value;
        Logger::Debug("Loaded %s = %s", fullKey.c_str(), value.c_str());
    }
    file.close();

    if (!ValidateConfiguration()) {
        Logger::Error("Configuration validation failed");
        return false;
    }

    ApplySecurityConfiguration();
    ApplyEACConfiguration();

    Logger::Info("Configuration loaded: %zu entries", m_configValues.size());
    return true;
}

bool ConfigManager::SaveConfiguration(const std::string& configFile) {
    Logger::Info("Saving configuration to: %s", configFile.c_str());
    std::ofstream file(configFile);
    if (!file.is_open()) {
        Logger::Error("Cannot open file for write: %s", configFile.c_str());
        return false;
    }

    // Group by section
    std::map<std::string, std::map<std::string, std::string>> sections;
    for (auto& [fullKey, val] : m_configValues) {
        if (auto dot = fullKey.find('.'); dot != std::string::npos) {
            auto sect = fullKey.substr(0, dot);
            auto key  = fullKey.substr(dot + 1);
            sections[sect][key] = val;
        } else {
            sections[""][fullKey] = val;
        }
    }

    file << "# RS2V Server Configuration\n";
    file << "# Generated on: " << GetCurrentTimestamp() << "\n\n";

    for (auto& [sect, kvs] : sections) {
        if (!sect.empty()) {
            file << "[" << sect << "]\n";
        }
        for (auto& [key, val] : kvs) {
            if (auto comment = GetConfigComment(sect, key); !comment.empty()) {
                file << "# " << comment << "\n";
            }
            file << key << "=" << val << "\n";
        }
        file << "\n";
    }

    file.close();
    Logger::Info("Configuration saved");
    return true;
}

bool ConfigManager::ReloadConfiguration() {
    Logger::Info("Reloading configuration: %s", m_primaryConfigFile.c_str());
    auto backup = m_configValues;
    m_configValues.clear();

    if (!LoadConfiguration(m_primaryConfigFile)) {
        Logger::Error("Reload failed, restoring previous configuration");
        m_configValues = std::move(backup);
        return false;
    }

    NotifyConfigurationChanged();
    Logger::Info("Configuration reloaded");
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
    auto s = GetString(key);
    if (s.empty()) return defaultValue;
    try {
        return std::stoi(s);
    } catch (...) {
        Logger::Warn("Invalid int for key '%s': %s", key.c_str(), s.c_str());
        return defaultValue;
    }
}

bool ConfigManager::GetBool(const std::string& key, bool defaultValue) const {
    auto s = StringUtils::ToLower(GetString(key));
    if (s.empty()) return defaultValue;
    if (s == "true" || s == "1" || s == "yes" || s == "on") return true;
    if (s == "false" || s == "0" || s == "no" || s == "off") return false;
    Logger::Warn("Invalid bool for key '%s': %s", key.c_str(), s.c_str());
    return defaultValue;
}

float ConfigManager::GetFloat(const std::string& key, float defaultValue) const {
    auto s = GetString(key);
    if (s.empty()) return defaultValue;
    try {
        return std::stof(s);
    } catch (...) {
        Logger::Warn("Invalid float for key '%s': %s", key.c_str(), s.c_str());
        return defaultValue;
    }
}

void ConfigManager::SetString(const std::string& key, const std::string& value) {
    m_configValues[key] = value;
    Logger::Debug("Config updated: %s = %s", key.c_str(), value.c_str());
    if (m_autoSave) SaveConfiguration(m_primaryConfigFile);
}

void ConfigManager::SetInt(const std::string& key, int v)    { SetString(key, std::to_string(v)); }
void ConfigManager::SetBool(const std::string& key, bool v)  { SetString(key, v ? "true" : "false"); }
void ConfigManager::SetFloat(const std::string& key, float v){ SetString(key, std::to_string(v)); }

bool ConfigManager::HasKey(const std::string& key) const {
    return m_configValues.find(key) != m_configValues.end();
}

void ConfigManager::RemoveKey(const std::string& key) {
    if (m_configValues.erase(key)) {
        Logger::Debug("Config key removed: %s", key.c_str());
    }
}

std::vector<std::string> ConfigManager::GetSectionKeys(const std::string& section) const {
    std::vector<std::string> keys;
    std::string prefix = section + ".";
    for (auto& [k, v] : m_configValues) {
        if (k.rfind(prefix, 0) == 0) {
            keys.push_back(k.substr(prefix.size()));
        }
    }
    return keys;
}

std::vector<std::string> ConfigManager::GetAllSections() const {
    std::set<std::string> secs;
    for (auto& [k, v] : m_configValues) {
        if (auto dot = k.find('.'); dot != std::string::npos) {
            secs.insert(k.substr(0, dot));
        }
    }
    return std::vector<std::string>(secs.begin(), secs.end());
}

void ConfigManager::SetAutoSave(bool enabled) {
    m_autoSave = enabled;
    Logger::Info("Auto-save %s", enabled ? "enabled" : "disabled");
}

bool ConfigManager::ValidateConfiguration() {
    bool ok = true;
    ok &= ValidateServerConfig();
    ok &= ValidateNetworkConfig();
    ok &= ValidateSecurityConfig();
    ok &= ValidateEACConfig();
    ok &= ValidateGameConfig();
    if (ok) Logger::Debug("All configuration sections valid");
    return ok;
}

bool ConfigManager::ValidateServerConfig() {
    if (GetString("General.server_name", "").empty()) {
        Logger::Error("General.server_name is required");
        return false;
    }
    int mp = GetInt("General.max_players", 64);
    if (mp < 1 || mp > 128) {
        Logger::Error("General.max_players must be 1–128");
        return false;
    }
    int port = GetInt("Network.port", 7777);
    if (port < 1024 || port > 65535) {
        Logger::Error("Network.port must be 1024–65535");
        return false;
    }
    int tr = GetInt("General.tick_rate", 60);
    if (tr < 10 || tr > 128) {
        Logger::Error("General.tick_rate must be 10–128");
        return false;
    }
    return true;
}

bool ConfigManager::ValidateNetworkConfig() {
    int pkt = GetInt("Network.max_packet_size", 1200);
    if (pkt < 64 || pkt > 65536) {
        Logger::Error("Network.max_packet_size must be 64–65536");
        return false;
    }
    return true;
}

bool ConfigManager::ValidateSecurityConfig() {
    if (GetBool("Security.fallback_custom_auth", false)) {
        auto f = GetString("Security.custom_auth_tokens_file", "");
        if (!Utils::FileExists(f)) {
            Logger::Error("Token file missing: %s", f.c_str());
            return false;
        }
    }
    return true;
}

bool ConfigManager::ValidateEACConfig() {
    static const std::vector<std::string> modes = {"off", "safe", "emulate"};
    auto m = GetString("Security.anti_cheat_mode", "off");
    if (std::find(modes.begin(), modes.end(), m) == modes.end()) {
        Logger::Error("Security.anti_cheat_mode must be off|safe|emulate");
        return false;
    }
    return true;
}

bool ConfigManager::ValidateGameConfig() {
    auto mf = GetString("General.map_rotation_file", "config/maps.ini");
    if (!Utils::FileExists(mf)) {
        Logger::Error("Map rotation file missing: %s", mf.c_str());
        return false;
    }
    return true;
}

void ConfigManager::ApplySecurityConfiguration() {
    Logger::Debug("Security configuration applied");
}

void ConfigManager::ApplyEACConfiguration() {
    auto mode = GetString("Security.anti_cheat_mode", "off");
    Logger::Info("AntiCheat mode: %s", mode.c_str());
}

void ConfigManager::InitializeConfigWatchers() {
    Logger::Debug("Config watchers not implemented");
}

void ConfigManager::NotifyConfigurationChanged() {
    Logger::Debug("Notifying %zu listeners", m_listeners.size());
    m_listeners.erase(
        std::remove_if(m_listeners.begin(), m_listeners.end(),
                       [](auto& w){ return w.expired(); }),
        m_listeners.end());
    for (auto& w : m_listeners) {
        if (auto l = w.lock()) {
            try { l->OnConfigurationChanged(); }
            catch (const std::exception& e) {
                Logger::Error("Listener exception: %s", e.what());
            }
        }
    }
}

std::string ConfigManager::GetConfigComment(const std::string& section,
                                            const std::string& key) {
    static const std::map<std::string, std::string> comments = {
        {"General.server_name", "Display name for your server"},
        {"General.max_players", "Max concurrent players (1-128)"},
        {"Network.port", "Port for game traffic (1024-65535)"},
        {"General.tick_rate", "Server tick rate (10-128)"},
        {"Security.anti_cheat_mode", "Anti-cheat mode: off|safe|emulate"},
        {"Logging.log_level", "Log verbosity: trace|debug|info|warn|error"}
    };
    std::string fk = section.empty() ? key : section + "." + key;
    auto it = comments.find(fk);
    return it != comments.end() ? it->second : "";
}

std::string ConfigManager::GetCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    std::ostringstream ss;
    ss << std::put_time(std::localtime(&t), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

bool ConfigManager::SaveAllConfigurations() {
    if (!m_primaryConfigFile.empty()) {
        return SaveConfiguration(m_primaryConfigFile);
    }
    return true;
}