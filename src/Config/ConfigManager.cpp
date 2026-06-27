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
#include <thread>
#include <regex>
#include <cctype>

ConfigManager::ConfigManager() {
    Logger::Trace("[ConfigManager::ConfigManager] Entry - default constructor called");
    Logger::Info("ConfigManager initialized");
    Logger::Trace("[ConfigManager::ConfigManager] Exit");
}

ConfigManager::~ConfigManager() {
    Logger::Trace("[ConfigManager::~ConfigManager] Entry - destructor called, stopping file watcher and saving all configurations before teardown");
    StopFileWatcher();
    SaveAllConfigurations();
    Logger::Trace("[ConfigManager::~ConfigManager] Exit");
}

bool ConfigManager::Initialize() {
    Logger::Trace("[ConfigManager::Initialize] Entry");
    Logger::Info("Initializing Configuration Manager...");

    // Ensure config directory exists
    try {
        Logger::Debug("[ConfigManager::Initialize] Attempting to create config directory 'config/'");
        std::filesystem::create_directories("config");
        Logger::Info("Configuration directory ensured: config/");
    } catch (const std::exception& e) {
        Logger::Error("[ConfigManager::Initialize] Failed to create config directory: %s", e.what());
        Logger::Trace("[ConfigManager::Initialize] Exit - returning false due to directory creation failure");
        return false;
    }

    // Load unified server.ini
    const std::string mainConfig = "config/server.ini";
    Logger::Debug("[ConfigManager::Initialize] Main config file path resolved to: %s", mainConfig.c_str());
    if (!LoadConfiguration(mainConfig)) {
        Logger::Error("[ConfigManager::Initialize] Failed to load main configuration: %s", mainConfig.c_str());
        Logger::Trace("[ConfigManager::Initialize] Exit - returning false due to LoadConfiguration failure");
        return false;
    }

    // Initialize file-watchers (placeholder)
    Logger::Debug("[ConfigManager::Initialize] Proceeding to initialize config watchers");
    InitializeConfigWatchers();

    Logger::Info("Configuration Manager initialized successfully");
    Logger::Trace("[ConfigManager::Initialize] Exit - returning true");
    return true;
}

bool ConfigManager::LoadConfiguration(const std::string& configFile) {
    Logger::Trace("[ConfigManager::LoadConfiguration] Entry - configFile='%s'", configFile.c_str());
    Logger::Info("Loading configuration file: %s", configFile.c_str());

    std::ifstream file(configFile);
    if (!file.is_open()) {
        Logger::Error("[ConfigManager::LoadConfiguration] Cannot open configuration file: %s", configFile.c_str());
        Logger::Trace("[ConfigManager::LoadConfiguration] Exit - returning false, file not openable");
        return false;
    }

    Logger::Debug("[ConfigManager::LoadConfiguration] File opened successfully, setting primary config file and clearing existing values");
    m_primaryConfigFile = configFile;
    m_configValues.clear();

    std::string line;
    std::string currentSection;
    size_t lineNumber = 0;

    while (std::getline(file, line)) {
        ++lineNumber;
        // Strip comments. Both '#' and ';' begin a comment (the config files use
        // ';' as their primary comment char, including inline after a value), so
        // strip from the first occurrence of either. This also drops full-line
        // ';' comment banners cleanly instead of logging them as invalid lines.
        if (auto pos = line.find_first_of("#;"); pos != std::string::npos) {
            Logger::Trace("[ConfigManager::LoadConfiguration] Line %zu: stripping comment at position %zu", lineNumber, pos);
            line.erase(pos);
        }
        line = StringUtils::Trim(line);
        if (line.empty()) {
            Logger::Trace("[ConfigManager::LoadConfiguration] Line %zu: empty or comment-only, skipping", lineNumber);
            continue;
        }

        // Section header
        if (line.front() == '[' && line.back() == ']') {
            currentSection = line.substr(1, line.size() - 2);
            Logger::Debug("[ConfigManager::LoadConfiguration] Section header found at line %zu: '%s'", lineNumber, currentSection.c_str());
            continue;
        }

        // Parse key=value
        auto eq = line.find('=');
        if (eq == std::string::npos) {
            Logger::Warn("[ConfigManager::LoadConfiguration] Invalid line %zu (no '=' separator): '%s'", lineNumber, line.c_str());
            continue;
        }
        std::string key   = StringUtils::Trim(line.substr(0, eq));
        std::string value = StringUtils::Trim(line.substr(eq + 1));

        // Reject malformed lines with an empty key (e.g. "=value" or "  =value").
        // Storing an empty key would create a bogus "<section>." entry that can
        // never be looked up and pollutes section enumeration; skip + warn.
        if (key.empty()) {
            Logger::Warn("[ConfigManager::LoadConfiguration] Invalid line %zu (empty key before '='): '%s'", lineNumber, line.c_str());
            continue;
        }

        std::string fullKey = currentSection.empty() ? key : (currentSection + "." + key);
        m_configValues[fullKey] = value;
        Logger::Debug("[ConfigManager::LoadConfiguration] Loaded config entry: %s = '%s' (line %zu)", fullKey.c_str(), value.c_str(), lineNumber);
    }
    file.close();
    Logger::Debug("[ConfigManager::LoadConfiguration] File parsing complete, total raw entries: %zu. Proceeding to validation.", m_configValues.size());

    if (!ValidateConfiguration()) {
        Logger::Error("[ConfigManager::LoadConfiguration] Configuration validation failed");
        Logger::Trace("[ConfigManager::LoadConfiguration] Exit - returning false due to validation failure");
        return false;
    }

    Logger::Debug("[ConfigManager::LoadConfiguration] Validation passed, applying security configuration");
    ApplySecurityConfiguration();
    Logger::Debug("[ConfigManager::LoadConfiguration] Applying EAC configuration");
    ApplyEACConfiguration();

    Logger::Info("Configuration loaded: %zu entries", m_configValues.size());
    Logger::Trace("[ConfigManager::LoadConfiguration] Exit - returning true");
    return true;
}

bool ConfigManager::SaveConfiguration(const std::string& configFile) {
    Logger::Trace("[ConfigManager::SaveConfiguration] Entry - configFile='%s'", configFile.c_str());
    Logger::Info("Saving configuration to: %s", configFile.c_str());
    std::ofstream file(configFile);
    if (!file.is_open()) {
        Logger::Error("[ConfigManager::SaveConfiguration] Cannot open file for write: %s", configFile.c_str());
        Logger::Trace("[ConfigManager::SaveConfiguration] Exit - returning false, file not writable");
        return false;
    }

    // Group by section
    Logger::Debug("[ConfigManager::SaveConfiguration] Grouping %zu config entries by section", m_configValues.size());
    std::map<std::string, std::map<std::string, std::string>> sections;
    for (auto& [fullKey, val] : m_configValues) {
        if (auto dot = fullKey.find('.'); dot != std::string::npos) {
            auto sect = fullKey.substr(0, dot);
            auto key  = fullKey.substr(dot + 1);
            sections[sect][key] = val;
            Logger::Trace("[ConfigManager::SaveConfiguration] Grouped key '%s' into section '%s'", key.c_str(), sect.c_str());
        } else {
            sections[""][fullKey] = val;
            Logger::Trace("[ConfigManager::SaveConfiguration] Grouped key '%s' into global section", fullKey.c_str());
        }
    }
    Logger::Debug("[ConfigManager::SaveConfiguration] Grouped into %zu sections", sections.size());

    file << "# RS2V Server Configuration\n";
    file << "# Generated on: " << GetCurrentTimestamp() << "\n\n";

    for (auto& [sect, kvs] : sections) {
        if (!sect.empty()) {
            file << "[" << sect << "]\n";
            Logger::Trace("[ConfigManager::SaveConfiguration] Writing section [%s] with %zu keys", sect.c_str(), kvs.size());
        } else {
            Logger::Trace("[ConfigManager::SaveConfiguration] Writing global section with %zu keys", kvs.size());
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
    Logger::Trace("[ConfigManager::SaveConfiguration] Exit - returning true");
    return true;
}

bool ConfigManager::ReloadConfiguration() {
    Logger::Trace("[ConfigManager::ReloadConfiguration] Entry - reloading from '%s'", m_primaryConfigFile.c_str());
    Logger::Info("Reloading configuration: %s", m_primaryConfigFile.c_str());
    Logger::Debug("[ConfigManager::ReloadConfiguration] Backing up %zu existing config values before reload", m_configValues.size());
    auto backup = m_configValues;
    m_configValues.clear();

    if (!LoadConfiguration(m_primaryConfigFile)) {
        Logger::Error("[ConfigManager::ReloadConfiguration] Reload failed, restoring previous configuration with %zu entries", backup.size());
        m_configValues = std::move(backup);
        Logger::Trace("[ConfigManager::ReloadConfiguration] Exit - returning false, backup restored");
        return false;
    }

    Logger::Debug("[ConfigManager::ReloadConfiguration] Reload succeeded, notifying listeners of configuration change");
    NotifyConfigurationChanged();
    Logger::Info("Configuration reloaded");
    Logger::Trace("[ConfigManager::ReloadConfiguration] Exit - returning true");
    return true;
}

std::string ConfigManager::GetString(const std::string& key, const std::string& defaultValue) const {
    Logger::Trace("[ConfigManager::GetString] Entry - key='%s', defaultValue='%s'", key.c_str(), defaultValue.c_str());
    auto it = m_configValues.find(key);
    if (it != m_configValues.end()) {
        Logger::Trace("[ConfigManager::GetString] Exit - key found, returning value='%s'", it->second.c_str());
        return it->second;
    }
    Logger::Debug("[ConfigManager::GetString] Config key '%s' not found, using default: '%s'", key.c_str(), defaultValue.c_str());
    Logger::Trace("[ConfigManager::GetString] Exit - returning default value");
    return defaultValue;
}

int ConfigManager::GetInt(const std::string& key, int defaultValue) const {
    Logger::Trace("[ConfigManager::GetInt] Entry - key='%s', defaultValue=%d", key.c_str(), defaultValue);
    auto s = GetString(key);
    if (s.empty()) {
        Logger::Debug("[ConfigManager::GetInt] String value is empty for key '%s', returning default %d", key.c_str(), defaultValue);
        Logger::Trace("[ConfigManager::GetInt] Exit - returning default %d", defaultValue);
        return defaultValue;
    }
    try {
        size_t pos = 0;
        int result = std::stoi(s, &pos);
        // Reject trailing non-numeric garbage (e.g. "123abc", "60.5") that stoi
        // would otherwise silently truncate. Trailing whitespace is tolerated.
        // Note: pure-numeric values consume the whole string, so valid config
        // is parsed byte-identically; out-of-range/overflow throws and is caught.
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
        if (pos != s.size()) {
            Logger::Warn("[ConfigManager::GetInt] Non-numeric trailing data in int for key '%s': '%s', returning default %d", key.c_str(), s.c_str(), defaultValue);
            Logger::Trace("[ConfigManager::GetInt] Exit - returning default due to trailing garbage");
            return defaultValue;
        }
        Logger::Trace("[ConfigManager::GetInt] Exit - parsed value=%d for key='%s'", result, key.c_str());
        return result;
    } catch (...) {
        Logger::Warn("[ConfigManager::GetInt] Invalid int for key '%s': '%s', returning default %d", key.c_str(), s.c_str(), defaultValue);
        Logger::Trace("[ConfigManager::GetInt] Exit - returning default due to parse error");
        return defaultValue;
    }
}

bool ConfigManager::GetBool(const std::string& key, bool defaultValue) const {
    Logger::Trace("[ConfigManager::GetBool] Entry - key='%s', defaultValue=%s", key.c_str(), defaultValue ? "true" : "false");
    auto s = StringUtils::ToLower(GetString(key));
    if (s.empty()) {
        Logger::Debug("[ConfigManager::GetBool] String value is empty for key '%s', returning default %s", key.c_str(), defaultValue ? "true" : "false");
        Logger::Trace("[ConfigManager::GetBool] Exit - returning default");
        return defaultValue;
    }
    if (s == "true" || s == "1" || s == "yes" || s == "on") {
        Logger::Trace("[ConfigManager::GetBool] Exit - parsed as true for key='%s' (raw='%s')", key.c_str(), s.c_str());
        return true;
    }
    if (s == "false" || s == "0" || s == "no" || s == "off") {
        Logger::Trace("[ConfigManager::GetBool] Exit - parsed as false for key='%s' (raw='%s')", key.c_str(), s.c_str());
        return false;
    }
    Logger::Warn("[ConfigManager::GetBool] Invalid bool for key '%s': '%s', returning default %s", key.c_str(), s.c_str(), defaultValue ? "true" : "false");
    Logger::Trace("[ConfigManager::GetBool] Exit - returning default due to invalid value");
    return defaultValue;
}

float ConfigManager::GetFloat(const std::string& key, float defaultValue) const {
    Logger::Trace("[ConfigManager::GetFloat] Entry - key='%s', defaultValue=%f", key.c_str(), defaultValue);
    auto s = GetString(key);
    if (s.empty()) {
        Logger::Debug("[ConfigManager::GetFloat] String value is empty for key '%s', returning default %f", key.c_str(), defaultValue);
        Logger::Trace("[ConfigManager::GetFloat] Exit - returning default");
        return defaultValue;
    }
    try {
        size_t pos = 0;
        float result = std::stof(s, &pos);
        // Reject trailing non-numeric garbage (e.g. "1.5xyz"); stof accepts
        // valid forms (decimals, exponents, inf/nan) fully, so well-formed
        // config values are parsed byte-identically. Trailing whitespace OK.
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
        if (pos != s.size()) {
            Logger::Warn("[ConfigManager::GetFloat] Non-numeric trailing data in float for key '%s': '%s', returning default %f", key.c_str(), s.c_str(), defaultValue);
            Logger::Trace("[ConfigManager::GetFloat] Exit - returning default due to trailing garbage");
            return defaultValue;
        }
        Logger::Trace("[ConfigManager::GetFloat] Exit - parsed value=%f for key='%s'", result, key.c_str());
        return result;
    } catch (...) {
        Logger::Warn("[ConfigManager::GetFloat] Invalid float for key '%s': '%s', returning default %f", key.c_str(), s.c_str(), defaultValue);
        Logger::Trace("[ConfigManager::GetFloat] Exit - returning default due to parse error");
        return defaultValue;
    }
}

void ConfigManager::SetString(const std::string& key, const std::string& value) {
    Logger::Trace("[ConfigManager::SetString] Entry - key='%s', value='%s'", key.c_str(), value.c_str());
    m_configValues[key] = value;
    Logger::Debug("[ConfigManager::SetString] Config updated: %s = '%s'", key.c_str(), value.c_str());
    if (m_autoSave) {
        Logger::Debug("[ConfigManager::SetString] Auto-save enabled, saving to '%s'", m_primaryConfigFile.c_str());
        SaveConfiguration(m_primaryConfigFile);
    } else {
        Logger::Trace("[ConfigManager::SetString] Auto-save disabled, skipping save");
    }
    Logger::Trace("[ConfigManager::SetString] Exit");
}

void ConfigManager::SetInt(const std::string& key, int v) {
    Logger::Trace("[ConfigManager::SetInt] Entry - key='%s', value=%d", key.c_str(), v);
    SetString(key, std::to_string(v));
    Logger::Trace("[ConfigManager::SetInt] Exit");
}
void ConfigManager::SetBool(const std::string& key, bool v) {
    Logger::Trace("[ConfigManager::SetBool] Entry - key='%s', value=%s", key.c_str(), v ? "true" : "false");
    SetString(key, v ? "true" : "false");
    Logger::Trace("[ConfigManager::SetBool] Exit");
}
void ConfigManager::SetFloat(const std::string& key, float v) {
    Logger::Trace("[ConfigManager::SetFloat] Entry - key='%s', value=%f", key.c_str(), v);
    SetString(key, std::to_string(v));
    Logger::Trace("[ConfigManager::SetFloat] Exit");
}

bool ConfigManager::HasKey(const std::string& key) const {
    Logger::Trace("[ConfigManager::HasKey] Entry - key='%s'", key.c_str());
    bool found = m_configValues.find(key) != m_configValues.end();
    Logger::Trace("[ConfigManager::HasKey] Exit - key '%s' %s", key.c_str(), found ? "found" : "not found");
    return found;
}

void ConfigManager::RemoveKey(const std::string& key) {
    Logger::Trace("[ConfigManager::RemoveKey] Entry - key='%s'", key.c_str());
    if (m_configValues.erase(key)) {
        Logger::Debug("[ConfigManager::RemoveKey] Config key removed: '%s'", key.c_str());
    } else {
        Logger::Debug("[ConfigManager::RemoveKey] Config key '%s' not found, nothing to remove", key.c_str());
    }
    Logger::Trace("[ConfigManager::RemoveKey] Exit");
}

std::vector<std::string> ConfigManager::GetSectionKeys(const std::string& section) const {
    Logger::Trace("[ConfigManager::GetSectionKeys] Entry - section='%s'", section.c_str());
    std::vector<std::string> keys;
    std::string prefix = section + ".";
    Logger::Debug("[ConfigManager::GetSectionKeys] Searching for keys with prefix '%s'", prefix.c_str());
    for (auto& [k, v] : m_configValues) {
        if (k.rfind(prefix, 0) == 0) {
            std::string subKey = k.substr(prefix.size());
            keys.push_back(subKey);
            Logger::Trace("[ConfigManager::GetSectionKeys] Found key: '%s'", subKey.c_str());
        }
    }
    Logger::Debug("[ConfigManager::GetSectionKeys] Found %zu keys in section '%s'", keys.size(), section.c_str());
    Logger::Trace("[ConfigManager::GetSectionKeys] Exit - returning %zu keys", keys.size());
    return keys;
}

std::vector<std::string> ConfigManager::GetAllSections() const {
    Logger::Trace("[ConfigManager::GetAllSections] Entry");
    std::set<std::string> secs;
    for (auto& [k, v] : m_configValues) {
        if (auto dot = k.find('.'); dot != std::string::npos) {
            secs.insert(k.substr(0, dot));
        }
    }
    Logger::Debug("[ConfigManager::GetAllSections] Found %zu distinct sections", secs.size());
    Logger::Trace("[ConfigManager::GetAllSections] Exit - returning %zu sections", secs.size());
    return std::vector<std::string>(secs.begin(), secs.end());
}

void ConfigManager::SetAutoSave(bool enabled) {
    Logger::Trace("[ConfigManager::SetAutoSave] Entry - enabled=%s", enabled ? "true" : "false");
    m_autoSave = enabled;
    Logger::Info("[ConfigManager::SetAutoSave] Auto-save %s", enabled ? "enabled" : "disabled");
    Logger::Trace("[ConfigManager::SetAutoSave] Exit");
}

bool ConfigManager::ValidateConfiguration() {
    Logger::Trace("[ConfigManager::ValidateConfiguration] Entry");
    bool ok = true;
    Logger::Debug("[ConfigManager::ValidateConfiguration] Validating server config section");
    ok &= ValidateServerConfig();
    Logger::Debug("[ConfigManager::ValidateConfiguration] Validating network config section");
    ok &= ValidateNetworkConfig();
    Logger::Debug("[ConfigManager::ValidateConfiguration] Validating security config section");
    ok &= ValidateSecurityConfig();
    Logger::Debug("[ConfigManager::ValidateConfiguration] Validating EAC config section");
    ok &= ValidateEACConfig();
    Logger::Debug("[ConfigManager::ValidateConfiguration] Validating game config section");
    ok &= ValidateGameConfig();
    if (ok) {
        Logger::Debug("[ConfigManager::ValidateConfiguration] All configuration sections valid");
    } else {
        Logger::Warn("[ConfigManager::ValidateConfiguration] One or more configuration sections failed validation");
    }
    Logger::Trace("[ConfigManager::ValidateConfiguration] Exit - returning %s", ok ? "true" : "false");
    return ok;
}

bool ConfigManager::ValidateServerConfig() {
    Logger::Trace("[ConfigManager::ValidateServerConfig] Entry");
    if (GetString("General.server_name", "").empty()) {
        Logger::Error("[ConfigManager::ValidateServerConfig] General.server_name is required but empty or missing");
        Logger::Trace("[ConfigManager::ValidateServerConfig] Exit - returning false");
        return false;
    }
    Logger::Debug("[ConfigManager::ValidateServerConfig] server_name validated OK");

    int mp = GetInt("General.max_players", 64);
    Logger::Debug("[ConfigManager::ValidateServerConfig] max_players value: %d", mp);
    if (mp < 1 || mp > 128) {
        Logger::Error("[ConfigManager::ValidateServerConfig] General.max_players must be 1-128, got %d", mp);
        Logger::Trace("[ConfigManager::ValidateServerConfig] Exit - returning false");
        return false;
    }
    Logger::Debug("[ConfigManager::ValidateServerConfig] max_players validated OK: %d", mp);

    int port = GetInt("Network.port", 7777);
    Logger::Debug("[ConfigManager::ValidateServerConfig] port value: %d", port);
    if (port < 1024 || port > 65535) {
        Logger::Error("[ConfigManager::ValidateServerConfig] Network.port must be 1024-65535, got %d", port);
        Logger::Trace("[ConfigManager::ValidateServerConfig] Exit - returning false");
        return false;
    }
    Logger::Debug("[ConfigManager::ValidateServerConfig] port validated OK: %d", port);

    int tr = GetInt("General.tick_rate", 60);
    Logger::Debug("[ConfigManager::ValidateServerConfig] tick_rate value: %d", tr);
    if (tr < 10 || tr > 128) {
        Logger::Error("[ConfigManager::ValidateServerConfig] General.tick_rate must be 10-128, got %d", tr);
        Logger::Trace("[ConfigManager::ValidateServerConfig] Exit - returning false");
        return false;
    }
    Logger::Debug("[ConfigManager::ValidateServerConfig] tick_rate validated OK: %d", tr);

    Logger::Trace("[ConfigManager::ValidateServerConfig] Exit - returning true, all checks passed");
    return true;
}

bool ConfigManager::ValidateNetworkConfig() {
    Logger::Trace("[ConfigManager::ValidateNetworkConfig] Entry");
    int pkt = GetInt("Network.max_packet_size", 1200);
    Logger::Debug("[ConfigManager::ValidateNetworkConfig] max_packet_size value: %d", pkt);
    if (pkt < 64 || pkt > 65536) {
        Logger::Error("[ConfigManager::ValidateNetworkConfig] Network.max_packet_size must be 64-65536, got %d", pkt);
        Logger::Trace("[ConfigManager::ValidateNetworkConfig] Exit - returning false");
        return false;
    }
    Logger::Debug("[ConfigManager::ValidateNetworkConfig] max_packet_size validated OK: %d", pkt);
    Logger::Trace("[ConfigManager::ValidateNetworkConfig] Exit - returning true");
    return true;
}

bool ConfigManager::ValidateSecurityConfig() {
    Logger::Trace("[ConfigManager::ValidateSecurityConfig] Entry");
    bool fallbackAuth = GetBool("Security.fallback_custom_auth", false);
    Logger::Debug("[ConfigManager::ValidateSecurityConfig] fallback_custom_auth=%s", fallbackAuth ? "true" : "false");
    if (fallbackAuth) {
        auto f = GetString("Security.custom_auth_tokens_file", "");
        Logger::Debug("[ConfigManager::ValidateSecurityConfig] Checking token file existence: '%s'", f.c_str());
        if (!std::filesystem::exists(f)) {
            Logger::Error("[ConfigManager::ValidateSecurityConfig] Token file missing: '%s'", f.c_str());
            Logger::Trace("[ConfigManager::ValidateSecurityConfig] Exit - returning false");
            return false;
        }
        Logger::Debug("[ConfigManager::ValidateSecurityConfig] Token file exists: '%s'", f.c_str());
    } else {
        Logger::Debug("[ConfigManager::ValidateSecurityConfig] Fallback custom auth disabled, skipping token file check");
    }
    Logger::Trace("[ConfigManager::ValidateSecurityConfig] Exit - returning true");
    return true;
}

bool ConfigManager::ValidateEACConfig() {
    Logger::Trace("[ConfigManager::ValidateEACConfig] Entry");
    static const std::vector<std::string> modes = {"off", "safe", "emulate"};
    auto m = GetString("Security.anti_cheat_mode", "off");
    Logger::Debug("[ConfigManager::ValidateEACConfig] anti_cheat_mode value: '%s'", m.c_str());
    if (std::find(modes.begin(), modes.end(), m) == modes.end()) {
        Logger::Error("[ConfigManager::ValidateEACConfig] Security.anti_cheat_mode must be off|safe|emulate, got '%s'", m.c_str());
        Logger::Trace("[ConfigManager::ValidateEACConfig] Exit - returning false");
        return false;
    }
    Logger::Debug("[ConfigManager::ValidateEACConfig] anti_cheat_mode validated OK: '%s'", m.c_str());
    Logger::Trace("[ConfigManager::ValidateEACConfig] Exit - returning true");
    return true;
}

bool ConfigManager::ValidateGameConfig() {
    Logger::Trace("[ConfigManager::ValidateGameConfig] Entry");
    auto mf = GetString("General.map_rotation_file", "config/maps.ini");
    Logger::Debug("[ConfigManager::ValidateGameConfig] map_rotation_file value: '%s'", mf.c_str());
    if (!std::filesystem::exists(mf)) {
        Logger::Error("[ConfigManager::ValidateGameConfig] Map rotation file missing: '%s'", mf.c_str());
        Logger::Trace("[ConfigManager::ValidateGameConfig] Exit - returning false");
        return false;
    }
    Logger::Debug("[ConfigManager::ValidateGameConfig] Map rotation file exists: '%s'", mf.c_str());
    Logger::Trace("[ConfigManager::ValidateGameConfig] Exit - returning true");
    return true;
}

void ConfigManager::ApplySecurityConfiguration() {
    Logger::Trace("[ConfigManager::ApplySecurityConfiguration] Entry");
    Logger::Debug("[ConfigManager::ApplySecurityConfiguration] Security configuration applied");
    Logger::Trace("[ConfigManager::ApplySecurityConfiguration] Exit");
}

void ConfigManager::ApplyEACConfiguration() {
    Logger::Trace("[ConfigManager::ApplyEACConfiguration] Entry");
    auto mode = GetString("Security.anti_cheat_mode", "off");
    Logger::Info("[ConfigManager::ApplyEACConfiguration] AntiCheat mode: '%s'", mode.c_str());
    Logger::Debug("[ConfigManager::ApplyEACConfiguration] EAC configuration applied with mode='%s'", mode.c_str());
    Logger::Trace("[ConfigManager::ApplyEACConfiguration] Exit");
}

void ConfigManager::InitializeConfigWatchers() {
    Logger::Trace("[ConfigManager::InitializeConfigWatchers] Entry");

    // Live configuration reloading is opt-out: enabled unless explicitly
    // disabled via [Configuration] live_reload=false. The watcher polls the
    // primary config file's modification time on a background thread and calls
    // ReloadConfiguration() (which notifies registered listeners) on change.
    if (!GetBool("Configuration.live_reload", true)) {
        Logger::Info("[ConfigManager::InitializeConfigWatchers] Live config reload disabled via Configuration.live_reload");
        Logger::Trace("[ConfigManager::InitializeConfigWatchers] Exit - disabled by config");
        return;
    }

    if (!StartFileWatcher()) {
        Logger::Warn("[ConfigManager::InitializeConfigWatchers] Failed to start config file watcher; live reload inactive");
    } else {
        Logger::Info("[ConfigManager::InitializeConfigWatchers] Config file watcher active for live reloading");
    }

    Logger::Trace("[ConfigManager::InitializeConfigWatchers] Exit");
}

void ConfigManager::NotifyConfigurationChanged() {
    Logger::Trace("[ConfigManager::NotifyConfigurationChanged] Entry");
    Logger::Debug("[ConfigManager::NotifyConfigurationChanged] Notifying %zu listeners", m_listeners.size());
    size_t expiredCount = 0;
    m_listeners.erase(
        std::remove_if(m_listeners.begin(), m_listeners.end(),
                       [&expiredCount](auto& w){ bool expired = w.expired(); if (expired) expiredCount++; return expired; }),
        m_listeners.end());
    if (expiredCount > 0) {
        Logger::Debug("[ConfigManager::NotifyConfigurationChanged] Removed %zu expired listener(s), %zu remaining", expiredCount, m_listeners.size());
    }
    for (size_t i = 0; i < m_listeners.size(); ++i) {
        auto& w = m_listeners[i];
        if (auto l = w.lock()) {
            Logger::Trace("[ConfigManager::NotifyConfigurationChanged] Notifying listener %zu/%zu", i + 1, m_listeners.size());
            try { l->OnConfigurationChanged(); }
            catch (const std::exception& e) {
                Logger::Error("[ConfigManager::NotifyConfigurationChanged] Listener %zu threw exception: %s", i + 1, e.what());
            }
        }
    }
    Logger::Trace("[ConfigManager::NotifyConfigurationChanged] Exit");
}

std::string ConfigManager::GetConfigComment(const std::string& section,
                                            const std::string& key) {
    Logger::Trace("[ConfigManager::GetConfigComment] Entry - section='%s', key='%s'", section.c_str(), key.c_str());
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
    std::string result = it != comments.end() ? it->second : "";
    Logger::Trace("[ConfigManager::GetConfigComment] Exit - fullKey='%s', comment='%s'", fk.c_str(), result.empty() ? "(none)" : result.c_str());
    return result;
}

std::string ConfigManager::GetCurrentTimestamp() {
    Logger::Trace("[ConfigManager::GetCurrentTimestamp] Entry");
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    std::ostringstream ss;
    ss << std::put_time(std::localtime(&t), "%Y-%m-%d %H:%M:%S");
    std::string result = ss.str();
    Logger::Trace("[ConfigManager::GetCurrentTimestamp] Exit - timestamp='%s'", result.c_str());
    return result;
}

bool ConfigManager::SaveAllConfigurations() {
    Logger::Trace("[ConfigManager::SaveAllConfigurations] Entry");
    if (!m_primaryConfigFile.empty()) {
        Logger::Debug("[ConfigManager::SaveAllConfigurations] Saving to primary config file: '%s'", m_primaryConfigFile.c_str());
        bool result = SaveConfiguration(m_primaryConfigFile);
        Logger::Trace("[ConfigManager::SaveAllConfigurations] Exit - returning %s", result ? "true" : "false");
        return result;
    }
    Logger::Debug("[ConfigManager::SaveAllConfigurations] No primary config file set, nothing to save");
    Logger::Trace("[ConfigManager::SaveAllConfigurations] Exit - returning true (nothing to save)");
    return true;
}

// ---------------------------------------------------------------------------
// Live configuration reloading - File Watcher
// ---------------------------------------------------------------------------

bool ConfigManager::StartFileWatcher() {
    Logger::Trace("[ConfigManager::StartFileWatcher] Entry");

    if (m_fileWatcherRunning.load()) {
        Logger::Warn("[ConfigManager::StartFileWatcher] File watcher is already running");
        Logger::Trace("[ConfigManager::StartFileWatcher] Exit - returning false, already running");
        return false;
    }

    if (m_primaryConfigFile.empty()) {
        Logger::Error("[ConfigManager::StartFileWatcher] Cannot start file watcher: no primary config file set");
        Logger::Trace("[ConfigManager::StartFileWatcher] Exit - returning false, no config file");
        return false;
    }

    if (!std::filesystem::exists(m_primaryConfigFile)) {
        Logger::Error("[ConfigManager::StartFileWatcher] Cannot start file watcher: config file does not exist: '%s'", m_primaryConfigFile.c_str());
        Logger::Trace("[ConfigManager::StartFileWatcher] Exit - returning false, file missing");
        return false;
    }

    m_fileWatcherRunning.store(true);
    Logger::Info("[ConfigManager::StartFileWatcher] Starting file watcher for: '%s'", m_primaryConfigFile.c_str());

    m_fileWatcherThread = std::thread(&ConfigManager::FileWatcherThread, this);

    Logger::Info("[ConfigManager::StartFileWatcher] File watcher started successfully");
    Logger::Trace("[ConfigManager::StartFileWatcher] Exit - returning true");
    return true;
}

void ConfigManager::StopFileWatcher() {
    Logger::Trace("[ConfigManager::StopFileWatcher] Entry");

    if (!m_fileWatcherRunning.load()) {
        Logger::Debug("[ConfigManager::StopFileWatcher] File watcher is not running, nothing to stop");
        Logger::Trace("[ConfigManager::StopFileWatcher] Exit - watcher was not running");
        return;
    }

    Logger::Info("[ConfigManager::StopFileWatcher] Stopping file watcher...");
    m_fileWatcherRunning.store(false);

    if (m_fileWatcherThread.joinable()) {
        Logger::Debug("[ConfigManager::StopFileWatcher] Joining file watcher thread");
        m_fileWatcherThread.join();
        Logger::Debug("[ConfigManager::StopFileWatcher] File watcher thread joined successfully");
    }

    Logger::Info("[ConfigManager::StopFileWatcher] File watcher stopped");
    Logger::Trace("[ConfigManager::StopFileWatcher] Exit");
}

bool ConfigManager::IsFileWatcherRunning() const {
    Logger::Trace("[ConfigManager::IsFileWatcherRunning] Entry");
    bool running = m_fileWatcherRunning.load();
    Logger::Trace("[ConfigManager::IsFileWatcherRunning] Exit - returning %s", running ? "true" : "false");
    return running;
}

void ConfigManager::FileWatcherThread() {
    Logger::Trace("[ConfigManager::FileWatcherThread] Entry - watcher thread started");
    Logger::Info("[ConfigManager::FileWatcherThread] File watcher thread running for: '%s'", m_primaryConfigFile.c_str());

    std::filesystem::file_time_type lastWriteTime;
    try {
        lastWriteTime = std::filesystem::last_write_time(m_primaryConfigFile);
        Logger::Debug("[ConfigManager::FileWatcherThread] Initial last-write-time captured for '%s'", m_primaryConfigFile.c_str());
    } catch (const std::exception& e) {
        Logger::Error("[ConfigManager::FileWatcherThread] Failed to get initial last-write-time: %s", e.what());
        m_fileWatcherRunning.store(false);
        Logger::Trace("[ConfigManager::FileWatcherThread] Exit - aborting due to initial timestamp error");
        return;
    }

    while (m_fileWatcherRunning.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(2));

        if (!m_fileWatcherRunning.load()) {
            Logger::Debug("[ConfigManager::FileWatcherThread] Stop requested during sleep, breaking out of loop");
            break;
        }

        try {
            if (!std::filesystem::exists(m_primaryConfigFile)) {
                Logger::Warn("[ConfigManager::FileWatcherThread] Config file no longer exists: '%s'", m_primaryConfigFile.c_str());
                continue;
            }

            auto currentWriteTime = std::filesystem::last_write_time(m_primaryConfigFile);
            if (currentWriteTime != lastWriteTime) {
                Logger::Info("[ConfigManager::FileWatcherThread] Config file change detected: '%s'", m_primaryConfigFile.c_str());
                lastWriteTime = currentWriteTime;

                std::lock_guard<std::mutex> lock(m_configMutex);
                Logger::Debug("[ConfigManager::FileWatcherThread] Acquired config mutex, reloading configuration");
                if (ReloadConfiguration()) {
                    Logger::Info("[ConfigManager::FileWatcherThread] Configuration reloaded successfully after file change");
                } else {
                    Logger::Error("[ConfigManager::FileWatcherThread] Configuration reload failed after file change");
                }
            }
        } catch (const std::exception& e) {
            Logger::Error("[ConfigManager::FileWatcherThread] Error checking file modification time: %s", e.what());
        } catch (...) {
            // A non-std throw (e.g. from a reload listener callback) must not
            // escape this thread function into std::terminate. Log and keep
            // watching.
            Logger::Error("[ConfigManager::FileWatcherThread] Non-std exception while checking/reloading config");
        }
    }

    Logger::Info("[ConfigManager::FileWatcherThread] File watcher thread exiting");
    Logger::Trace("[ConfigManager::FileWatcherThread] Exit");
}

// ---------------------------------------------------------------------------
// Configuration backup and rollback
// ---------------------------------------------------------------------------

bool ConfigManager::BackupConfiguration(const std::string& backupPath) {
    Logger::Trace("[ConfigManager::BackupConfiguration] Entry - backupPath='%s'", backupPath.c_str());

    if (m_primaryConfigFile.empty()) {
        Logger::Error("[ConfigManager::BackupConfiguration] Cannot backup: no primary config file set");
        Logger::Trace("[ConfigManager::BackupConfiguration] Exit - returning false, no config file");
        return false;
    }

    if (!std::filesystem::exists(m_primaryConfigFile)) {
        Logger::Error("[ConfigManager::BackupConfiguration] Cannot backup: config file does not exist: '%s'", m_primaryConfigFile.c_str());
        Logger::Trace("[ConfigManager::BackupConfiguration] Exit - returning false, file missing");
        return false;
    }

    std::string targetPath = backupPath;
    if (targetPath.empty()) {
        // Generate timestamped backup path
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);
        std::ostringstream ss;
        ss << std::put_time(std::localtime(&t), "%Y%m%d_%H%M%S");
        targetPath = m_primaryConfigFile + ".backup." + ss.str();
        Logger::Debug("[ConfigManager::BackupConfiguration] Generated backup path: '%s'", targetPath.c_str());
    }

    try {
        std::filesystem::copy_file(m_primaryConfigFile, targetPath,
                                   std::filesystem::copy_options::overwrite_existing);
        Logger::Info("[ConfigManager::BackupConfiguration] Configuration backed up: '%s' -> '%s'", m_primaryConfigFile.c_str(), targetPath.c_str());
        Logger::Trace("[ConfigManager::BackupConfiguration] Exit - returning true");
        return true;
    } catch (const std::exception& e) {
        Logger::Error("[ConfigManager::BackupConfiguration] Failed to create backup: %s", e.what());
        Logger::Trace("[ConfigManager::BackupConfiguration] Exit - returning false due to copy error");
        return false;
    }
}

bool ConfigManager::RollbackConfiguration(const std::string& backupPath) {
    Logger::Trace("[ConfigManager::RollbackConfiguration] Entry - backupPath='%s'", backupPath.c_str());

    if (backupPath.empty()) {
        Logger::Error("[ConfigManager::RollbackConfiguration] Cannot rollback: no backup path specified");
        Logger::Trace("[ConfigManager::RollbackConfiguration] Exit - returning false, empty path");
        return false;
    }

    if (!std::filesystem::exists(backupPath)) {
        Logger::Error("[ConfigManager::RollbackConfiguration] Cannot rollback: backup file does not exist: '%s'", backupPath.c_str());
        Logger::Trace("[ConfigManager::RollbackConfiguration] Exit - returning false, backup missing");
        return false;
    }

    if (m_primaryConfigFile.empty()) {
        Logger::Error("[ConfigManager::RollbackConfiguration] Cannot rollback: no primary config file set");
        Logger::Trace("[ConfigManager::RollbackConfiguration] Exit - returning false, no config file");
        return false;
    }

    try {
        Logger::Info("[ConfigManager::RollbackConfiguration] Restoring configuration from backup: '%s'", backupPath.c_str());
        std::filesystem::copy_file(backupPath, m_primaryConfigFile,
                                   std::filesystem::copy_options::overwrite_existing);
        Logger::Debug("[ConfigManager::RollbackConfiguration] Backup file copied to '%s', reloading configuration", m_primaryConfigFile.c_str());
    } catch (const std::exception& e) {
        Logger::Error("[ConfigManager::RollbackConfiguration] Failed to restore backup file: %s", e.what());
        Logger::Trace("[ConfigManager::RollbackConfiguration] Exit - returning false due to copy error");
        return false;
    }

    if (!ReloadConfiguration()) {
        Logger::Error("[ConfigManager::RollbackConfiguration] Failed to reload configuration after rollback");
        Logger::Trace("[ConfigManager::RollbackConfiguration] Exit - returning false due to reload failure");
        return false;
    }

    Logger::Info("[ConfigManager::RollbackConfiguration] Configuration rolled back successfully from: '%s'", backupPath.c_str());
    Logger::Trace("[ConfigManager::RollbackConfiguration] Exit - returning true");
    return true;
}

std::vector<std::string> ConfigManager::GetAvailableBackups() const {
    Logger::Trace("[ConfigManager::GetAvailableBackups] Entry");
    std::vector<std::string> backups;

    if (m_primaryConfigFile.empty()) {
        Logger::Warn("[ConfigManager::GetAvailableBackups] No primary config file set, cannot scan for backups");
        Logger::Trace("[ConfigManager::GetAvailableBackups] Exit - returning empty list");
        return backups;
    }

    std::filesystem::path configPath(m_primaryConfigFile);
    std::filesystem::path configDir = configPath.parent_path();
    std::string configFilename = configPath.filename().string();

    if (configDir.empty()) {
        configDir = ".";
    }

    Logger::Debug("[ConfigManager::GetAvailableBackups] Scanning directory '%s' for backups of '%s'",
                  configDir.string().c_str(), configFilename.c_str());

    std::string backupPattern = configFilename + ".backup.";

    try {
        for (const auto& entry : std::filesystem::directory_iterator(configDir)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            std::string filename = entry.path().filename().string();
            if (filename.rfind(backupPattern, 0) == 0) {
                std::string fullPath = entry.path().string();
                backups.push_back(fullPath);
                Logger::Debug("[ConfigManager::GetAvailableBackups] Found backup: '%s'", fullPath.c_str());
            }
        }
    } catch (const std::exception& e) {
        Logger::Error("[ConfigManager::GetAvailableBackups] Error scanning for backups: %s", e.what());
    }

    // Sort backups alphabetically (timestamps in filenames ensure chronological order)
    std::sort(backups.begin(), backups.end());

    Logger::Info("[ConfigManager::GetAvailableBackups] Found %zu backup file(s)", backups.size());
    Logger::Trace("[ConfigManager::GetAvailableBackups] Exit - returning %zu backups", backups.size());
    return backups;
}