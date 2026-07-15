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
#include <charconv>
#include <system_error>

namespace {

using ConfigValueMap = std::map<std::string, std::string>;

std::string GetValue(const ConfigValueMap& values,
                     const std::string& key,
                     const std::string& defaultValue = {}) {
    const auto it = values.find(key);
    return it != values.end() ? it->second : defaultValue;
}

int GetIntValue(const ConfigValueMap& values,
                const std::string& key,
                int defaultValue) {
    const std::string text = GetValue(values, key);
    if (text.empty()) {
        return defaultValue;
    }

    try {
        std::size_t consumed = 0;
        const int parsed = std::stoi(text, &consumed);
        while (consumed < text.size() &&
               std::isspace(static_cast<unsigned char>(text[consumed]))) {
            ++consumed;
        }
        return consumed == text.size() ? parsed : defaultValue;
    } catch (...) {
        return defaultValue;
    }
}

bool GetBoolValue(const ConfigValueMap& values,
                  const std::string& key,
                  bool defaultValue) {
    const std::string text = StringUtils::ToLower(GetValue(values, key));
    if (text == "true" || text == "1" || text == "yes" || text == "on") {
        return true;
    }
    if (text == "false" || text == "0" || text == "no" || text == "off") {
        return false;
    }
    return defaultValue;
}

bool IsBoolValue(const std::string& value) {
    const std::string text = StringUtils::ToLower(StringUtils::Trim(value));
    return text == "true" || text == "1" || text == "yes" || text == "on" ||
           text == "false" || text == "0" || text == "no" || text == "off";
}

} // namespace

ConfigManager::ConfigManager() {
    Logger::Trace("[ConfigManager::ConfigManager] Entry - default constructor called");
    Logger::Info("ConfigManager initialized");
    Logger::Trace("[ConfigManager::ConfigManager] Exit");
}

ConfigManager::~ConfigManager() {
    Logger::Trace("[ConfigManager::~ConfigManager] Entry - destructor called, stopping file watcher");
    StopFileWatcher();
    // Loading configuration is a read operation.  Rewriting the primary file
    // here made even short-lived readers (notably main::InitializeLogging)
    // reorder keys, replace comments, and update the file timestamp at every
    // server start.  Persistence remains explicit: callers may use
    // SaveConfiguration()/SaveAllConfigurations(), or opt into immediate
    // writes with SetAutoSave(true).
    Logger::Trace("[ConfigManager::~ConfigManager] Exit");
}

bool ConfigManager::Initialize() {
    return Initialize("config/server.ini");
}

bool ConfigManager::Initialize(const std::string& configFile) {
    Logger::Trace("[ConfigManager::Initialize] Entry");
    Logger::Info("Initializing Configuration Manager...");

    // Ensure the selected config file's parent directory exists. For the
    // default path this remains config/, while alternate-instance paths no
    // longer cause an unrelated config/ directory to be selected.
    try {
        const std::filesystem::path selectedPath(configFile);
        const std::filesystem::path parent = selectedPath.parent_path();
        if (!parent.empty()) {
            Logger::Debug("[ConfigManager::Initialize] Ensuring config directory: %s",
                          parent.string().c_str());
            std::filesystem::create_directories(parent);
            Logger::Info("Configuration directory ensured: %s", parent.string().c_str());
        }
    } catch (const std::exception& e) {
        Logger::Error("[ConfigManager::Initialize] Failed to create config directory: %s", e.what());
        Logger::Trace("[ConfigManager::Initialize] Exit - returning false due to directory creation failure");
        return false;
    }

    Logger::Debug("[ConfigManager::Initialize] Main config file path resolved to: %s", configFile.c_str());
    if (!LoadConfiguration(configFile)) {
        Logger::Error("[ConfigManager::Initialize] Failed to load main configuration: %s", configFile.c_str());
        Logger::Trace("[ConfigManager::Initialize] Exit - returning false due to LoadConfiguration failure");
        return false;
    }

    // Start the opt-out polling watcher after the primary file is loaded. The
    // watcher owns reload notification; initialization itself remains read-only.
    Logger::Debug("[ConfigManager::Initialize] Proceeding to initialize config watchers");
    InitializeConfigWatchers();

    Logger::Info("Configuration Manager initialized successfully");
    Logger::Trace("[ConfigManager::Initialize] Exit - returning true");
    return true;
}

bool ConfigManager::LoadConfiguration(const std::string& configFile) {
    Logger::Trace("[ConfigManager::LoadConfiguration] Entry - configFile='%s'", configFile.c_str());
    Logger::Info("Loading configuration file: %s", configFile.c_str());

    const bool loaded = LoadConfigurationTransaction(configFile, nullptr);
    Logger::Trace("[ConfigManager::LoadConfiguration] Exit - returning %s",
                  loaded ? "true" : "false");
    return loaded;
}

bool ConfigManager::ParseConfigurationFile(const std::string& configFile,
                                           ConfigValues& parsedValues) const {
    parsedValues.clear();

    // Coordinate our own saves, backups, and rollbacks with reads. External
    // editors are still untrusted; validation below keeps a partial external
    // write from ever replacing the live state.
    std::lock_guard<std::mutex> ioLock(m_fileIoMutex);

    std::ifstream file(configFile);
    if (!file.is_open()) {
        Logger::Error("[ConfigManager::ParseConfigurationFile] Cannot open configuration file: %s",
                      configFile.c_str());
        return false;
    }

    Logger::Debug("[ConfigManager::ParseConfigurationFile] File opened successfully; parsing into temporary state");

    std::string line;
    std::string currentSection;
    size_t lineNumber = 0;

    while (std::getline(file, line)) {
        ++lineNumber;
        // Strip trailing comments. Both '#' and ';' begin a comment (the config
        // files use ';' as a comment char, including inline after a value), but a
        // comment char only STARTS a comment when it is at the START of the line or
        // is PRECEDED BY WHITESPACE - otherwise it is a literal character inside a
        // value (e.g. rcon_password=Sup#rSecret!). Erasing from the first '#'/';'
        // anywhere on the line silently truncates any value containing one,
        // corrupting/weakening secrets, paths, and regexes. In-value '#'/';' (no
        // leading whitespace) is preserved, while full-line and whitespace-
        // separated trailing comment banners are still dropped cleanly.
        for (size_t i = 0; i < line.size(); ++i) {
            if ((line[i] == '#' || line[i] == ';') &&
                (i == 0 || std::isspace(static_cast<unsigned char>(line[i - 1])))) {
                Logger::Trace("[ConfigManager::ParseConfigurationFile] Line %zu: stripping comment at position %zu", lineNumber, i);
                line.erase(i);
                break;
            }
        }
        line = StringUtils::Trim(line);
        if (line.empty()) {
            Logger::Trace("[ConfigManager::ParseConfigurationFile] Line %zu: empty or comment-only, skipping", lineNumber);
            continue;
        }

        // Section header
        if (line.front() == '[' && line.back() == ']') {
            currentSection = line.substr(1, line.size() - 2);
            Logger::Debug("[ConfigManager::ParseConfigurationFile] Section header found at line %zu: '%s'", lineNumber, currentSection.c_str());
            continue;
        }

        // Parse key=value
        auto eq = line.find('=');
        if (eq == std::string::npos) {
            Logger::Warn("[ConfigManager::ParseConfigurationFile] Invalid line %zu (no '=' separator): '%s'", lineNumber, line.c_str());
            continue;
        }
        std::string key   = StringUtils::Trim(line.substr(0, eq));
        std::string value = StringUtils::Trim(line.substr(eq + 1));

        // Reject malformed lines with an empty key (e.g. "=value" or "  =value").
        // Storing an empty key would create a bogus "<section>." entry that can
        // never be looked up and pollutes section enumeration; skip + warn.
        if (key.empty()) {
            Logger::Warn("[ConfigManager::ParseConfigurationFile] Invalid line %zu (empty key before '='): '%s'", lineNumber, line.c_str());
            continue;
        }

        std::string fullKey = currentSection.empty() ? key : (currentSection + "." + key);
        parsedValues[fullKey] = value;
        Logger::Debug("[ConfigManager::ParseConfigurationFile] Loaded config entry: %s = '%s' (line %zu)", fullKey.c_str(), value.c_str(), lineNumber);
    }
    Logger::Debug("[ConfigManager::ParseConfigurationFile] File parsing complete, total raw entries: %zu",
                  parsedValues.size());
    return true;
}

bool ConfigManager::LoadConfigurationTransaction(
    const std::string& configFile,
    const std::string* expectedPrimaryFile) {
    ConfigValues parsedValues;
    if (!ParseConfigurationFile(configFile, parsedValues)) {
        return false;
    }

    if (!ValidateConfiguration(parsedValues)) {
        Logger::Error("[ConfigManager::LoadConfigurationTransaction] Configuration validation failed; live state unchanged");
        return false;
    }

    const std::size_t entryCount = parsedValues.size();
    {
        std::unique_lock<std::shared_mutex> stateLock(m_stateMutex);
        if (expectedPrimaryFile != nullptr &&
            m_primaryConfigFile != *expectedPrimaryFile) {
            Logger::Warn("[ConfigManager::LoadConfigurationTransaction] Primary file changed from '%s' while reload was in flight; discarding stale reload",
                         expectedPrimaryFile->c_str());
            return false;
        }

        m_configValues.swap(parsedValues);
        m_primaryConfigFile = configFile;
    }

    Logger::Debug("[ConfigManager::LoadConfigurationTransaction] Validation passed and complete state published; applying security configuration");
    ApplySecurityConfiguration();
    Logger::Debug("[ConfigManager::LoadConfigurationTransaction] Applying EAC configuration");
    ApplyEACConfiguration();

    Logger::Info("Configuration loaded: %zu entries", entryCount);
    return true;
}

bool ConfigManager::SaveConfiguration(const std::string& configFile) {
    Logger::Trace("[ConfigManager::SaveConfiguration] Entry - configFile='%s'", configFile.c_str());
    Logger::Info("Saving configuration to: %s", configFile.c_str());

    ConfigValues valuesSnapshot;
    {
        std::shared_lock<std::shared_mutex> stateLock(m_stateMutex);
        valuesSnapshot = m_configValues;
    }

    std::lock_guard<std::mutex> ioLock(m_fileIoMutex);
    std::ofstream file(configFile);
    if (!file.is_open()) {
        Logger::Error("[ConfigManager::SaveConfiguration] Cannot open file for write: %s", configFile.c_str());
        Logger::Trace("[ConfigManager::SaveConfiguration] Exit - returning false, file not writable");
        return false;
    }

    // Group by section
    Logger::Debug("[ConfigManager::SaveConfiguration] Grouping %zu config entries by section", valuesSnapshot.size());
    std::map<std::string, std::map<std::string, std::string>> sections;
    for (const auto& [fullKey, val] : valuesSnapshot) {
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
    std::string primaryFile;
    {
        std::shared_lock<std::shared_mutex> stateLock(m_stateMutex);
        primaryFile = m_primaryConfigFile;
    }

    Logger::Trace("[ConfigManager::ReloadConfiguration] Entry - reloading from '%s'", primaryFile.c_str());
    if (primaryFile.empty()) {
        Logger::Error("[ConfigManager::ReloadConfiguration] Cannot reload: no primary configuration file is set");
        return false;
    }

    Logger::Info("Reloading configuration: %s", primaryFile.c_str());
    if (!LoadConfigurationTransaction(primaryFile, &primaryFile)) {
        Logger::Error("[ConfigManager::ReloadConfiguration] Reload failed; previous configuration remains live");
        Logger::Trace("[ConfigManager::ReloadConfiguration] Exit - returning false, live state unchanged");
        return false;
    }

    Logger::Debug("[ConfigManager::ReloadConfiguration] Reload succeeded, notifying listeners of configuration change");
    NotifyConfigurationChanged();
    Logger::Info("Configuration reloaded");
    Logger::Trace("[ConfigManager::ReloadConfiguration] Exit - returning true");
    return true;
}

bool ConfigManager::ImportConfiguration(const std::string& sourceFile) {
    Logger::Info("[ConfigManager::ImportConfiguration] Importing configuration from: %s",
                 sourceFile.c_str());

    ConfigValues importedValues;
    if (!ParseConfigurationFile(sourceFile, importedValues)) {
        Logger::Error("[ConfigManager::ImportConfiguration] Failed to parse import file: %s",
                      sourceFile.c_str());
        return false;
    }

    std::string autoSavePath;
    {
        std::unique_lock<std::shared_mutex> stateLock(m_stateMutex);
        ConfigValues mergedValues = m_configValues;
        for (const auto& [key, value] : importedValues) {
            mergedValues[key] = value;
        }

        if (!ValidateConfiguration(mergedValues)) {
            Logger::Error("[ConfigManager::ImportConfiguration] Merged configuration validation failed; live state unchanged");
            return false;
        }

        m_configValues.swap(mergedValues);
        if (m_autoSave) {
            autoSavePath = m_primaryConfigFile;
        }
    }

    ApplySecurityConfiguration();
    ApplyEACConfiguration();
    if (!autoSavePath.empty()) {
        SaveConfiguration(autoSavePath);
    }
    NotifyConfigurationChanged();
    Logger::Info("[ConfigManager::ImportConfiguration] Configuration imported successfully");
    return true;
}

bool ConfigManager::ExportConfiguration(const std::string& targetFile) {
    Logger::Info("[ConfigManager::ExportConfiguration] Exporting configuration to: %s",
                 targetFile.c_str());
    return SaveConfiguration(targetFile);
}

void ConfigManager::ResetToDefaults() {
    ConfigValues defaultValues = {
        {"General.server_name", "RS2V Custom Server"},
        {"General.max_players", "64"},
        {"General.tick_rate", "60"},
        {"General.map_rotation_file", "config/maps.ini"},
        {"General.data_directory", "data/"},
        {"Game.initial_map", "VNTE-Resort"},
        {"Gameplay.wait_for_ready_player", "true"},
        {"Network.port", "7777"},
        {"Network.max_packet_size", "1200"},
        {"Security.anti_cheat_mode", "off"},
        {"Security.enable_anti_cheat", "false"},
        {"Security.fallback_custom_auth", "false"},
        {"EAC.listen_port", "7957"},
        {"Configuration.live_reload", "true"},
    };

    if (!ValidateConfiguration(defaultValues)) {
        Logger::Error("[ConfigManager::ResetToDefaults] Built-in defaults failed validation; live state unchanged");
        return;
    }

    std::string autoSavePath;
    {
        std::unique_lock<std::shared_mutex> stateLock(m_stateMutex);
        m_configValues.swap(defaultValues);
        if (m_autoSave) {
            autoSavePath = m_primaryConfigFile;
        }
    }

    ApplySecurityConfiguration();
    ApplyEACConfiguration();
    if (!autoSavePath.empty()) {
        SaveConfiguration(autoSavePath);
    }
    NotifyConfigurationChanged();
    Logger::Info("[ConfigManager::ResetToDefaults] Configuration reset to built-in defaults");
}

std::string ConfigManager::GetString(const std::string& key, const std::string& defaultValue) const {
    Logger::Trace("[ConfigManager::GetString] Entry - key='%s', defaultValue='%s'", key.c_str(), defaultValue.c_str());
    std::shared_lock<std::shared_mutex> stateLock(m_stateMutex);
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
    std::string autoSavePath;
    {
        std::unique_lock<std::shared_mutex> stateLock(m_stateMutex);
        m_configValues[key] = value;
        if (m_autoSave) {
            autoSavePath = m_primaryConfigFile;
        }
    }

    Logger::Debug("[ConfigManager::SetString] Config updated: %s = '%s'", key.c_str(), value.c_str());
    if (!autoSavePath.empty()) {
        // Save from a fresh snapshot after releasing the exclusive state lock.
        // This keeps Set -> Save reentrant-safe without a recursive mutex.
        Logger::Debug("[ConfigManager::SetString] Auto-save enabled, saving to '%s'", autoSavePath.c_str());
        SaveConfiguration(autoSavePath);
    } else {
        Logger::Trace("[ConfigManager::SetString] Auto-save disabled or no primary file set, skipping save");
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
    std::shared_lock<std::shared_mutex> stateLock(m_stateMutex);
    bool found = m_configValues.find(key) != m_configValues.end();
    Logger::Trace("[ConfigManager::HasKey] Exit - key '%s' %s", key.c_str(), found ? "found" : "not found");
    return found;
}

void ConfigManager::RemoveKey(const std::string& key) {
    Logger::Trace("[ConfigManager::RemoveKey] Entry - key='%s'", key.c_str());
    std::string autoSavePath;
    bool removed = false;
    {
        std::unique_lock<std::shared_mutex> stateLock(m_stateMutex);
        removed = m_configValues.erase(key) != 0;
        if (removed && m_autoSave) {
            autoSavePath = m_primaryConfigFile;
        }
    }

    if (removed) {
        Logger::Debug("[ConfigManager::RemoveKey] Config key removed: '%s'", key.c_str());
        if (!autoSavePath.empty()) {
            Logger::Debug("[ConfigManager::RemoveKey] Auto-save enabled, saving to '%s'",
                          autoSavePath.c_str());
            SaveConfiguration(autoSavePath);
        }
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
    std::shared_lock<std::shared_mutex> stateLock(m_stateMutex);
    for (const auto& [k, v] : m_configValues) {
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
    std::shared_lock<std::shared_mutex> stateLock(m_stateMutex);
    for (const auto& [k, v] : m_configValues) {
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
    {
        std::unique_lock<std::shared_mutex> stateLock(m_stateMutex);
        m_autoSave = enabled;
    }
    Logger::Info("[ConfigManager::SetAutoSave] Auto-save %s", enabled ? "enabled" : "disabled");
    Logger::Trace("[ConfigManager::SetAutoSave] Exit");
}

bool ConfigManager::IsAutoSaveEnabled() const {
    std::shared_lock<std::shared_mutex> stateLock(m_stateMutex);
    return m_autoSave;
}

void ConfigManager::AddConfigurationListener(
    std::shared_ptr<IConfigurationListener> listener) {
    if (!listener) {
        Logger::Warn("[ConfigManager::AddConfigurationListener] Ignoring null listener");
        return;
    }

    std::lock_guard<std::mutex> listenerLock(m_listenerMutex);
    m_listeners.erase(
        std::remove_if(m_listeners.begin(), m_listeners.end(),
                       [](const auto& weak) { return weak.expired(); }),
        m_listeners.end());
    m_listeners.emplace_back(std::move(listener));
    Logger::Debug("[ConfigManager::AddConfigurationListener] Configuration listener added");
}

void ConfigManager::RemoveConfigurationListener(
    std::shared_ptr<IConfigurationListener> listener) {
    std::lock_guard<std::mutex> listenerLock(m_listenerMutex);
    m_listeners.erase(
        std::remove_if(
            m_listeners.begin(), m_listeners.end(),
            [&listener](const auto& weak) {
                const auto strong = weak.lock();
                return !strong || (listener && strong.get() == listener.get());
            }),
        m_listeners.end());
    Logger::Debug("[ConfigManager::RemoveConfigurationListener] Configuration listener removed");
}

bool ConfigManager::ValidateConfiguration(const ConfigValues& values) const {
    Logger::Trace("[ConfigManager::ValidateConfiguration] Entry");
    bool ok = true;
    Logger::Debug("[ConfigManager::ValidateConfiguration] Validating server config section");
    ok &= ValidateServerConfig(values);
    Logger::Debug("[ConfigManager::ValidateConfiguration] Validating network config section");
    ok &= ValidateNetworkConfig(values);
    Logger::Debug("[ConfigManager::ValidateConfiguration] Validating security config section");
    ok &= ValidateSecurityConfig(values);
    Logger::Debug("[ConfigManager::ValidateConfiguration] Validating EAC config section");
    ok &= ValidateEACConfig(values);
    Logger::Debug("[ConfigManager::ValidateConfiguration] Validating game config section");
    ok &= ValidateGameConfig(values);
    if (ok) {
        Logger::Debug("[ConfigManager::ValidateConfiguration] All configuration sections valid");
    } else {
        Logger::Warn("[ConfigManager::ValidateConfiguration] One or more configuration sections failed validation");
    }
    Logger::Trace("[ConfigManager::ValidateConfiguration] Exit - returning %s", ok ? "true" : "false");
    return ok;
}

bool ConfigManager::ValidateServerConfig(const ConfigValues& values) const {
    Logger::Trace("[ConfigManager::ValidateServerConfig] Entry");
    if (GetValue(values, "General.server_name", "").empty()) {
        Logger::Error("[ConfigManager::ValidateServerConfig] General.server_name is required but empty or missing");
        Logger::Trace("[ConfigManager::ValidateServerConfig] Exit - returning false");
        return false;
    }
    Logger::Debug("[ConfigManager::ValidateServerConfig] server_name validated OK");

    int mp = GetIntValue(values, "General.max_players", 64);
    Logger::Debug("[ConfigManager::ValidateServerConfig] max_players value: %d", mp);
    if (mp < 1 || mp > 128) {
        Logger::Error("[ConfigManager::ValidateServerConfig] General.max_players must be 1-128, got %d", mp);
        Logger::Trace("[ConfigManager::ValidateServerConfig] Exit - returning false");
        return false;
    }
    Logger::Debug("[ConfigManager::ValidateServerConfig] max_players validated OK: %d", mp);

    int port = GetIntValue(values, "Network.port", 7777);
    Logger::Debug("[ConfigManager::ValidateServerConfig] port value: %d", port);
    if (port < 1024 || port > 65535) {
        Logger::Error("[ConfigManager::ValidateServerConfig] Network.port must be 1024-65535, got %d", port);
        Logger::Trace("[ConfigManager::ValidateServerConfig] Exit - returning false");
        return false;
    }
    Logger::Debug("[ConfigManager::ValidateServerConfig] port validated OK: %d", port);

    int tr = GetIntValue(values, "General.tick_rate", 60);
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

bool ConfigManager::ValidateNetworkConfig(const ConfigValues& values) const {
    Logger::Trace("[ConfigManager::ValidateNetworkConfig] Entry");
    int pkt = GetIntValue(values, "Network.max_packet_size", 1200);
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

bool ConfigManager::ValidateSecurityConfig(const ConfigValues& values) const {
    Logger::Trace("[ConfigManager::ValidateSecurityConfig] Entry");
    bool fallbackAuth = GetBoolValue(values, "Security.fallback_custom_auth", false);
    Logger::Debug("[ConfigManager::ValidateSecurityConfig] fallback_custom_auth=%s", fallbackAuth ? "true" : "false");
    if (fallbackAuth) {
        auto f = GetValue(values, "Security.custom_auth_tokens_file", "");
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

bool ConfigManager::ValidateEACConfig(const ConfigValues& values) const {
    Logger::Trace("[ConfigManager::ValidateEACConfig] Entry");
    static const std::vector<std::string> modes = {"off", "safe", "emulate"};
    const auto m = StringUtils::ToLower(
        StringUtils::Trim(GetValue(values, "Security.anti_cheat_mode", "off")));
    Logger::Debug("[ConfigManager::ValidateEACConfig] anti_cheat_mode value: '%s'", m.c_str());
    if (std::find(modes.begin(), modes.end(), m) == modes.end()) {
        Logger::Error("[ConfigManager::ValidateEACConfig] Security.anti_cheat_mode must be off|safe|emulate, got '%s'", m.c_str());
        Logger::Trace("[ConfigManager::ValidateEACConfig] Exit - returning false");
        return false;
    }

    // Parse the raw value strictly instead of using GetInt(). GetInt() falls
    // back to its default for malformed input, which would silently turn a
    // typo into a live listener on the default port.
    const std::string portText =
        StringUtils::Trim(GetValue(values, "EAC.listen_port", "7957"));
    int listenPort = 0;
    const char* const begin = portText.data();
    const char* const end = begin + portText.size();
    const auto parsed = std::from_chars(begin, end, listenPort, 10);
    if (portText.empty() || parsed.ec != std::errc{} || parsed.ptr != end ||
        listenPort < 1 || listenPort > 65535) {
        Logger::Error("[ConfigManager::ValidateEACConfig] EAC.listen_port must be an integer in range 1-65535, got '%s'",
                      portText.c_str());
        Logger::Trace("[ConfigManager::ValidateEACConfig] Exit - returning false");
        return false;
    }

    Logger::Debug("[ConfigManager::ValidateEACConfig] anti_cheat_mode validated OK: '%s'", m.c_str());
    Logger::Debug("[ConfigManager::ValidateEACConfig] listen_port validated OK: %d", listenPort);
    Logger::Trace("[ConfigManager::ValidateEACConfig] Exit - returning true");
    return true;
}

bool ConfigManager::ValidateGameConfig(const ConfigValues& values) const {
    Logger::Trace("[ConfigManager::ValidateGameConfig] Entry");
    const auto waitForReadyIt = values.find("Gameplay.wait_for_ready_player");
    if (waitForReadyIt != values.end() && !IsBoolValue(waitForReadyIt->second)) {
        Logger::Error("[ConfigManager::ValidateGameConfig] Gameplay.wait_for_ready_player must be a boolean, got '%s'",
                      waitForReadyIt->second.c_str());
        Logger::Trace("[ConfigManager::ValidateGameConfig] Exit - returning false");
        return false;
    }

    auto mf = GetValue(values, "General.map_rotation_file", "config/maps.ini");
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

    // Promote weak listeners to a strong snapshot while holding the registry
    // lock, then invoke arbitrary user code off-lock. A listener may safely
    // read configuration, add/remove listeners, or stop the watcher from its
    // callback without deadlocking this registry.
    std::vector<std::shared_ptr<IConfigurationListener>> listeners;
    size_t expiredCount = 0;
    {
        std::lock_guard<std::mutex> listenerLock(m_listenerMutex);
        auto out = m_listeners.begin();
        for (auto it = m_listeners.begin(); it != m_listeners.end(); ++it) {
            if (auto listener = it->lock()) {
                listeners.push_back(std::move(listener));
                *out++ = *it;
            } else {
                ++expiredCount;
            }
        }
        m_listeners.erase(out, m_listeners.end());
    }

    Logger::Debug("[ConfigManager::NotifyConfigurationChanged] Notifying %zu listeners",
                  listeners.size());
    if (expiredCount > 0) {
        Logger::Debug("[ConfigManager::NotifyConfigurationChanged] Removed %zu expired listener(s), %zu remaining",
                      expiredCount, listeners.size());
    }

    for (size_t i = 0; i < listeners.size(); ++i) {
        Logger::Trace("[ConfigManager::NotifyConfigurationChanged] Notifying listener %zu/%zu",
                      i + 1, listeners.size());
        try {
            listeners[i]->OnConfigurationChanged();
        } catch (const std::exception& e) {
            Logger::Error("[ConfigManager::NotifyConfigurationChanged] Listener %zu threw exception: %s",
                          i + 1, e.what());
        } catch (...) {
            Logger::Error("[ConfigManager::NotifyConfigurationChanged] Listener %zu threw a non-standard exception",
                          i + 1);
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
        {"EAC.listen_port", "EAC emulator UDP listen port (1-65535)"},
        {"Logging.log_level", "Log verbosity: trace|debug|info|warn|error"},
        {"Gameplay.wait_for_ready_player", "Hold Preparation until a retail player finalizes a role; false allows empty headless progression"}
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
    std::tm localTime{};
#ifdef _WIN32
    localtime_s(&localTime, &t);
#else
    localtime_r(&t, &localTime);
#endif
    std::ostringstream ss;
    ss << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
    std::string result = ss.str();
    Logger::Trace("[ConfigManager::GetCurrentTimestamp] Exit - timestamp='%s'", result.c_str());
    return result;
}

bool ConfigManager::SaveAllConfigurations() {
    Logger::Trace("[ConfigManager::SaveAllConfigurations] Entry");
    std::string primaryFile;
    {
        std::shared_lock<std::shared_mutex> stateLock(m_stateMutex);
        primaryFile = m_primaryConfigFile;
    }

    if (!primaryFile.empty()) {
        Logger::Debug("[ConfigManager::SaveAllConfigurations] Saving to primary config file: '%s'", primaryFile.c_str());
        bool result = SaveConfiguration(primaryFile);
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

    std::unique_lock<std::mutex> lifecycleLock(m_watcherLifecycleMutex);
    const std::thread::id callerId = std::this_thread::get_id();
    if (callerId == m_watcherThreadId) {
        Logger::Error("[ConfigManager::StartFileWatcher] Cannot start/restart watcher from its own thread");
        return false;
    }

    m_watcherLifecycleCv.wait(
        lifecycleLock, [this] { return !m_watcherJoinInProgress; });

    if (m_fileWatcherRunning.load()) {
        Logger::Warn("[ConfigManager::StartFileWatcher] File watcher is already running");
        Logger::Trace("[ConfigManager::StartFileWatcher] Exit - returning false, already running");
        return false;
    }

    // A watcher can mark itself stopped before the owner observes it (for
    // example when the initial last_write_time lookup fails).  The std::thread
    // remains joinable even though the running flag is false; assigning a new
    // thread over it would call std::terminate.  Reap that completed thread
    // before attempting a restart.
    if (m_fileWatcherThread.joinable()) {
        Logger::Debug("[ConfigManager::StartFileWatcher] Joining completed watcher before restart");
        std::thread completedThread = std::move(m_fileWatcherThread);
        m_watcherJoinInProgress = true;
        lifecycleLock.unlock();
        completedThread.join();
        lifecycleLock.lock();
        m_watcherJoinInProgress = false;
        m_watcherThreadId = std::thread::id{};
        m_watcherLifecycleCv.notify_all();
    }

    std::string primaryFile;
    {
        std::shared_lock<std::shared_mutex> stateLock(m_stateMutex);
        primaryFile = m_primaryConfigFile;
    }

    if (primaryFile.empty()) {
        Logger::Error("[ConfigManager::StartFileWatcher] Cannot start file watcher: no primary config file set");
        Logger::Trace("[ConfigManager::StartFileWatcher] Exit - returning false, no config file");
        return false;
    }

    {
        std::lock_guard<std::mutex> ioLock(m_fileIoMutex);
        if (!std::filesystem::exists(primaryFile)) {
            Logger::Error("[ConfigManager::StartFileWatcher] Cannot start file watcher: config file does not exist: '%s'", primaryFile.c_str());
            Logger::Trace("[ConfigManager::StartFileWatcher] Exit - returning false, file missing");
            return false;
        }
    }

    m_fileWatcherRunning.store(true);
    Logger::Info("[ConfigManager::StartFileWatcher] Starting file watcher for: '%s'", primaryFile.c_str());

    try {
        m_fileWatcherThread =
            std::thread(&ConfigManager::FileWatcherThread, this, primaryFile);
        m_watcherThreadId = m_fileWatcherThread.get_id();
    } catch (const std::exception& e) {
        m_fileWatcherRunning.store(false);
        m_watcherWake.notify_all();
        Logger::Error("[ConfigManager::StartFileWatcher] Failed to create watcher thread: %s", e.what());
        return false;
    }

    Logger::Info("[ConfigManager::StartFileWatcher] File watcher started successfully");
    Logger::Trace("[ConfigManager::StartFileWatcher] Exit - returning true");
    return true;
}

void ConfigManager::StopFileWatcher() {
    Logger::Trace("[ConfigManager::StopFileWatcher] Entry");

    std::unique_lock<std::mutex> lifecycleLock(m_watcherLifecycleMutex);
    if (std::this_thread::get_id() == m_watcherThreadId) {
        // A callback can request a stop from the watcher thread. Never detach
        // or self-join: the owner reaps this joinable thread on the next Stop,
        // Start, or destruction.
        m_fileWatcherRunning.store(false);
        m_watcherWake.notify_all();
        Logger::Warn("[ConfigManager::StopFileWatcher] Stop requested from watcher thread; join deferred to owner");
        return;
    }

    m_watcherLifecycleCv.wait(
        lifecycleLock, [this] { return !m_watcherJoinInProgress; });

    // Re-read running state only after any prior join finishes. A concurrent
    // Start holds this lifecycle lock through thread creation, so this exchange
    // cannot accidentally miss a newly-started watcher.
    const bool wasRunning = m_fileWatcherRunning.exchange(false);
    m_watcherWake.notify_all();

    if (!wasRunning && !m_fileWatcherThread.joinable()) {
        Logger::Debug("[ConfigManager::StopFileWatcher] File watcher is not running, nothing to stop");
        Logger::Trace("[ConfigManager::StopFileWatcher] Exit - watcher was not running");
        return;
    }

    if (wasRunning) {
        Logger::Info("[ConfigManager::StopFileWatcher] Stopping file watcher...");
    } else {
        Logger::Debug("[ConfigManager::StopFileWatcher] Reaping a completed watcher thread");
    }

    if (m_fileWatcherThread.joinable()) {
        Logger::Debug("[ConfigManager::StopFileWatcher] Joining file watcher thread");
        std::thread watcherThread = std::move(m_fileWatcherThread);
        m_watcherJoinInProgress = true;
        lifecycleLock.unlock();
        watcherThread.join();
        lifecycleLock.lock();
        m_watcherJoinInProgress = false;
        m_watcherThreadId = std::thread::id{};
        m_watcherLifecycleCv.notify_all();
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

void ConfigManager::FileWatcherThread(std::string watchedFile) {
    Logger::Trace("[ConfigManager::FileWatcherThread] Entry - watcher thread started");
    Logger::Info("[ConfigManager::FileWatcherThread] File watcher thread running for: '%s'", watchedFile.c_str());

    std::filesystem::file_time_type lastWriteTime;
    try {
        std::lock_guard<std::mutex> ioLock(m_fileIoMutex);
        lastWriteTime = std::filesystem::last_write_time(watchedFile);
        Logger::Debug("[ConfigManager::FileWatcherThread] Initial last-write-time captured for '%s'", watchedFile.c_str());
    } catch (const std::exception& e) {
        Logger::Error("[ConfigManager::FileWatcherThread] Failed to get initial last-write-time: %s", e.what());
        m_fileWatcherRunning.store(false);
        Logger::Trace("[ConfigManager::FileWatcherThread] Exit - aborting due to initial timestamp error");
        return;
    }

    while (m_fileWatcherRunning.load()) {
        std::unique_lock<std::mutex> waitLock(m_watcherWaitMutex);
        const bool stopping = m_watcherWake.wait_for(
            waitLock, std::chrono::seconds(2),
            [this] { return !m_fileWatcherRunning.load(); });
        waitLock.unlock();

        if (stopping || !m_fileWatcherRunning.load()) {
            Logger::Debug("[ConfigManager::FileWatcherThread] Stop requested during sleep, breaking out of loop");
            break;
        }

        try {
            std::string currentPrimary;
            {
                std::shared_lock<std::shared_mutex> stateLock(m_stateMutex);
                currentPrimary = m_primaryConfigFile;
            }
            if (currentPrimary != watchedFile) {
                Logger::Info("[ConfigManager::FileWatcherThread] Primary file changed to '%s'; stopping stale watcher for '%s'",
                             currentPrimary.c_str(), watchedFile.c_str());
                m_fileWatcherRunning.store(false);
                break;
            }

            std::filesystem::file_time_type currentWriteTime;
            {
                std::lock_guard<std::mutex> ioLock(m_fileIoMutex);
                if (!std::filesystem::exists(watchedFile)) {
                    Logger::Warn("[ConfigManager::FileWatcherThread] Config file no longer exists: '%s'", watchedFile.c_str());
                    continue;
                }
                currentWriteTime = std::filesystem::last_write_time(watchedFile);
            }

            if (currentWriteTime != lastWriteTime) {
                Logger::Info("[ConfigManager::FileWatcherThread] Config file change detected: '%s'", watchedFile.c_str());

                if (ReloadConfiguration()) {
                    // Only acknowledge a timestamp after a valid state was
                    // published. Invalid/partial external writes are retried.
                    lastWriteTime = currentWriteTime;
                    Logger::Info("[ConfigManager::FileWatcherThread] Configuration reloaded successfully after file change");
                } else {
                    Logger::Error("[ConfigManager::FileWatcherThread] Configuration reload failed after file change; retaining previous state");
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

    m_fileWatcherRunning.store(false);
    Logger::Info("[ConfigManager::FileWatcherThread] File watcher thread exiting");
    Logger::Trace("[ConfigManager::FileWatcherThread] Exit");
}

// ---------------------------------------------------------------------------
// Configuration backup and rollback
// ---------------------------------------------------------------------------

bool ConfigManager::BackupConfiguration(const std::string& backupPath) {
    Logger::Trace("[ConfigManager::BackupConfiguration] Entry - backupPath='%s'", backupPath.c_str());

    std::string primaryFile;
    {
        std::shared_lock<std::shared_mutex> stateLock(m_stateMutex);
        primaryFile = m_primaryConfigFile;
    }

    if (primaryFile.empty()) {
        Logger::Error("[ConfigManager::BackupConfiguration] Cannot backup: no primary config file set");
        Logger::Trace("[ConfigManager::BackupConfiguration] Exit - returning false, no config file");
        return false;
    }

    std::string targetPath = backupPath;
    if (targetPath.empty()) {
        // Generate timestamped backup path
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);
        std::tm localTime{};
#ifdef _WIN32
        localtime_s(&localTime, &t);
#else
        localtime_r(&t, &localTime);
#endif
        std::ostringstream ss;
        ss << std::put_time(&localTime, "%Y%m%d_%H%M%S");
        targetPath = primaryFile + ".backup." + ss.str();
        Logger::Debug("[ConfigManager::BackupConfiguration] Generated backup path: '%s'", targetPath.c_str());
    }

    try {
        std::lock_guard<std::mutex> ioLock(m_fileIoMutex);
        if (!std::filesystem::exists(primaryFile)) {
            Logger::Error("[ConfigManager::BackupConfiguration] Cannot backup: config file does not exist: '%s'", primaryFile.c_str());
            return false;
        }
        std::filesystem::copy_file(primaryFile, targetPath,
                                   std::filesystem::copy_options::overwrite_existing);
        Logger::Info("[ConfigManager::BackupConfiguration] Configuration backed up: '%s' -> '%s'", primaryFile.c_str(), targetPath.c_str());
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

    std::string primaryFile;
    {
        std::shared_lock<std::shared_mutex> stateLock(m_stateMutex);
        primaryFile = m_primaryConfigFile;
    }
    if (primaryFile.empty()) {
        Logger::Error("[ConfigManager::RollbackConfiguration] Cannot rollback: no primary config file set");
        Logger::Trace("[ConfigManager::RollbackConfiguration] Exit - returning false, no config file");
        return false;
    }

    try {
        std::lock_guard<std::mutex> ioLock(m_fileIoMutex);
        if (!std::filesystem::exists(backupPath)) {
            Logger::Error("[ConfigManager::RollbackConfiguration] Cannot rollback: backup file does not exist: '%s'", backupPath.c_str());
            return false;
        }
        Logger::Info("[ConfigManager::RollbackConfiguration] Restoring configuration from backup: '%s'", backupPath.c_str());
        std::filesystem::copy_file(backupPath, primaryFile,
                                   std::filesystem::copy_options::overwrite_existing);
        Logger::Debug("[ConfigManager::RollbackConfiguration] Backup file copied to '%s', reloading configuration", primaryFile.c_str());
    } catch (const std::exception& e) {
        Logger::Error("[ConfigManager::RollbackConfiguration] Failed to restore backup file: %s", e.what());
        Logger::Trace("[ConfigManager::RollbackConfiguration] Exit - returning false due to copy error");
        return false;
    }

    if (!LoadConfigurationTransaction(primaryFile, &primaryFile)) {
        Logger::Error("[ConfigManager::RollbackConfiguration] Failed to reload configuration after rollback");
        Logger::Trace("[ConfigManager::RollbackConfiguration] Exit - returning false due to reload failure");
        return false;
    }

    NotifyConfigurationChanged();
    Logger::Info("[ConfigManager::RollbackConfiguration] Configuration rolled back successfully from: '%s'", backupPath.c_str());
    Logger::Trace("[ConfigManager::RollbackConfiguration] Exit - returning true");
    return true;
}

std::vector<std::string> ConfigManager::GetAvailableBackups() const {
    Logger::Trace("[ConfigManager::GetAvailableBackups] Entry");
    std::vector<std::string> backups;

    std::string primaryFile;
    {
        std::shared_lock<std::shared_mutex> stateLock(m_stateMutex);
        primaryFile = m_primaryConfigFile;
    }

    if (primaryFile.empty()) {
        Logger::Warn("[ConfigManager::GetAvailableBackups] No primary config file set, cannot scan for backups");
        Logger::Trace("[ConfigManager::GetAvailableBackups] Exit - returning empty list");
        return backups;
    }

    std::filesystem::path configPath(primaryFile);
    std::filesystem::path configDir = configPath.parent_path();
    std::string configFilename = configPath.filename().string();

    if (configDir.empty()) {
        configDir = ".";
    }

    Logger::Debug("[ConfigManager::GetAvailableBackups] Scanning directory '%s' for backups of '%s'",
                  configDir.string().c_str(), configFilename.c_str());

    std::string backupPattern = configFilename + ".backup.";

    try {
        std::lock_guard<std::mutex> ioLock(m_fileIoMutex);
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
