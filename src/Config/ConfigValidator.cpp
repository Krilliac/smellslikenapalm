// src/Config/ConfigValidator.cpp

#include "Config/ConfigValidator.h"
#include "Utils/Logger.h"
#include "Utils/StringUtils.h"
#include "Utils/FileUtils.h"
#include <fstream>
#include <regex>
#include <filesystem>
#include <thread>

ConfigValidator::ConfigValidator() {
    Logger::Trace("[ConfigValidator::ConfigValidator] Entry - default constructor called");
    Logger::Info("ConfigValidator initialized");
    InitializeValidationRules();
    Logger::Trace("[ConfigValidator::ConfigValidator] Exit");
}

void ConfigValidator::InitializeValidationRules() {
    Logger::Trace("[ConfigValidator::InitializeValidationRules] Entry");
    Logger::Debug("[ConfigValidator::InitializeValidationRules] Initializing validation rules");
    Logger::Trace("[ConfigValidator::InitializeValidationRules] Exit");
}

void ConfigValidator::InitializeCustomValidators() {
    Logger::Trace("[ConfigValidator::InitializeCustomValidators] Entry");
    Logger::Debug("[ConfigValidator::InitializeCustomValidators] Initializing custom validators");
    Logger::Trace("[ConfigValidator::InitializeCustomValidators] Exit");
}

ConfigValidator::~ConfigValidator() {
    Logger::Trace("[ConfigValidator::~ConfigValidator] Entry - destructor called");
    Logger::Trace("[ConfigValidator::~ConfigValidator] Exit");
}

bool ConfigValidator::Initialize() {
    Logger::Trace("[ConfigValidator::Initialize] Entry");
    Logger::Info("Initializing Configuration Validator...");
    Logger::Debug("[ConfigValidator::Initialize] Calling InitializeCustomValidators");
    InitializeCustomValidators();
    Logger::Info("Configuration Validator initialized successfully");
    Logger::Trace("[ConfigValidator::Initialize] Exit - returning true");
    return true;
}

ValidationResult ConfigValidator::ValidateConfiguration(const std::map<std::string, std::string>& config) {
    Logger::Trace("[ConfigValidator::ValidateConfiguration] Entry - config has %zu entries", config.size());
    Logger::Debug("[ConfigValidator::ValidateConfiguration] Starting comprehensive configuration validation...");
    ValidationResult result; result.isValid = true;

    Logger::Debug("[ConfigValidator::ValidateConfiguration] Validating General section");
    ValidateGeneralSection(config, result);
    Logger::Debug("[ConfigValidator::ValidateConfiguration] Validating Network section");
    ValidateNetworkSection(config, result);
    Logger::Debug("[ConfigValidator::ValidateConfiguration] Validating Security section");
    ValidateSecuritySection(config, result);
    Logger::Debug("[ConfigValidator::ValidateConfiguration] Validating EAC section");
    ValidateEACSection(config, result);
    Logger::Debug("[ConfigValidator::ValidateConfiguration] Validating AntiCheat section");
    ValidateAntiCheatSection(config, result);
    Logger::Debug("[ConfigValidator::ValidateConfiguration] Validating Logging section");
    ValidateLoggingSection(config, result);
    Logger::Debug("[ConfigValidator::ValidateConfiguration] Validating Performance section");
    ValidatePerformanceSection(config, result);
    Logger::Debug("[ConfigValidator::ValidateConfiguration] Validating GamePlay section");
    ValidateGamePlaySection(config, result);
    Logger::Debug("[ConfigValidator::ValidateConfiguration] Validating cross-section dependencies");
    ValidateCrossSectionDependencies(config, result);
    Logger::Debug("[ConfigValidator::ValidateConfiguration] Checking deprecated settings");
    CheckDeprecatedSettings(config, result);

    if (result.isValid)
        Logger::Info("[ConfigValidator::ValidateConfiguration] Configuration validation passed with %zu warnings", result.warnings.size());
    else
        Logger::Error("[ConfigValidator::ValidateConfiguration] Configuration validation failed with %zu errors and %zu warnings",
                      result.errors.size(), result.warnings.size());

    Logger::Trace("[ConfigValidator::ValidateConfiguration] Exit - isValid=%s, errors=%zu, warnings=%zu",
                  result.isValid ? "true" : "false", result.errors.size(), result.warnings.size());
    return result;
}

bool ConfigValidator::ValidateConfigurationFile(const std::string& configFile) {
    Logger::Trace("[ConfigValidator::ValidateConfigurationFile] Entry - configFile='%s'", configFile.c_str());
    Logger::Info("[ConfigValidator::ValidateConfigurationFile] Validating configuration file: %s", configFile.c_str());
    if (!std::filesystem::exists(configFile)) {
        Logger::Error("[ConfigValidator::ValidateConfigurationFile] Configuration file does not exist: '%s'", configFile.c_str());
        Logger::Trace("[ConfigValidator::ValidateConfigurationFile] Exit - returning false, file not found");
        return false;
    }
    Logger::Debug("[ConfigValidator::ValidateConfigurationFile] File exists, loading configuration entries");
    std::map<std::string, std::string> config;
    if (!LoadConfigurationFromFile(configFile, config)) {
        Logger::Error("[ConfigValidator::ValidateConfigurationFile] Failed to load configuration from file: '%s'", configFile.c_str());
        Logger::Trace("[ConfigValidator::ValidateConfigurationFile] Exit - returning false, load failed");
        return false;
    }
    Logger::Debug("[ConfigValidator::ValidateConfigurationFile] Loaded %zu config entries, proceeding to validate", config.size());
    auto result = ValidateConfiguration(config);
    LogValidationResults(result);
    Logger::Trace("[ConfigValidator::ValidateConfigurationFile] Exit - returning %s", result.isValid ? "true" : "false");
    return result.isValid;
}

void ConfigValidator::ValidateGeneralSection(const std::map<std::string, std::string>& cfg, ValidationResult& r) {
    Logger::Trace("[ConfigValidator::ValidateGeneralSection] Entry");
    Logger::Debug("[ConfigValidator::ValidateGeneralSection] Validating [General] section...");
    // server_name
    Logger::Debug("[ConfigValidator::ValidateGeneralSection] Checking required key General.server_name");
    ValidateRequired(cfg, "General.server_name", r);
    // max_players
    if (auto v = GetConfig(cfg, "General.max_players")) {
        int x = StringToInt(*v);
        Logger::Debug("[ConfigValidator::ValidateGeneralSection] General.max_players raw='%s', parsed=%d", v->c_str(), x);
        if (x < 1 || x > 128) {
            Logger::Error("[ConfigValidator::ValidateGeneralSection] General.max_players out of range [1,128]: %d", x);
            r.errors.push_back("General.max_players must be between 1 and 128");
            r.isValid = false;
        } else {
            Logger::Debug("[ConfigValidator::ValidateGeneralSection] General.max_players validated OK: %d", x);
        }
    } else {
        Logger::Debug("[ConfigValidator::ValidateGeneralSection] General.max_players not present, skipping validation");
    }
    // tick_rate
    if (auto v = GetConfig(cfg, "General.tick_rate")) {
        int x = StringToInt(*v);
        Logger::Debug("[ConfigValidator::ValidateGeneralSection] General.tick_rate raw='%s', parsed=%d", v->c_str(), x);
        if (x < 10 || x > 128) {
            Logger::Error("[ConfigValidator::ValidateGeneralSection] General.tick_rate out of range [10,128]: %d", x);
            r.errors.push_back("General.tick_rate must be between 10 and 128");
            r.isValid = false;
        } else {
            Logger::Debug("[ConfigValidator::ValidateGeneralSection] General.tick_rate validated OK: %d", x);
        }
    } else {
        Logger::Debug("[ConfigValidator::ValidateGeneralSection] General.tick_rate not present, skipping validation");
    }
    Logger::Trace("[ConfigValidator::ValidateGeneralSection] Exit");
}

void ConfigValidator::ValidateNetworkSection(const std::map<std::string, std::string>& cfg, ValidationResult& r) {
    Logger::Trace("[ConfigValidator::ValidateNetworkSection] Entry");
    Logger::Debug("[ConfigValidator::ValidateNetworkSection] Validating [Network] section...");
    // port
    if (auto v = GetConfig(cfg, "Network.port")) {
        Logger::Debug("[ConfigValidator::ValidateNetworkSection] Network.port present, raw='%s', validating port range [1024,65535]", v->c_str());
        ValidatePortRange(cfg, "Network.port", 1024, 65535, r);
    } else {
        Logger::Debug("[ConfigValidator::ValidateNetworkSection] Network.port not present, skipping port validation");
    }
    // max_packet_size
    if (auto v = GetConfig(cfg, "Network.max_packet_size")) {
        int x = StringToInt(*v);
        Logger::Debug("[ConfigValidator::ValidateNetworkSection] Network.max_packet_size raw='%s', parsed=%d", v->c_str(), x);
        if (x < 64 || x > 65536) {
            Logger::Error("[ConfigValidator::ValidateNetworkSection] Network.max_packet_size out of range [64,65536]: %d", x);
            r.errors.push_back("Network.max_packet_size must be between 64 and 65536");
            r.isValid = false;
        } else {
            Logger::Debug("[ConfigValidator::ValidateNetworkSection] Network.max_packet_size validated OK: %d", x);
        }
        if (x > 1500) {
            Logger::Warn("[ConfigValidator::ValidateNetworkSection] Network.max_packet_size=%d above MTU 1500, may cause fragmentation", x);
            r.warnings.push_back("Network.max_packet_size above 1500 may cause fragmentation");
        }
    } else {
        Logger::Debug("[ConfigValidator::ValidateNetworkSection] Network.max_packet_size not present, skipping");
    }
    Logger::Trace("[ConfigValidator::ValidateNetworkSection] Exit");
}

void ConfigValidator::ValidateSecuritySection(const std::map<std::string, std::string>& cfg, ValidationResult& r) {
    Logger::Trace("[ConfigValidator::ValidateSecuritySection] Entry");
    Logger::Debug("[ConfigValidator::ValidateSecuritySection] Validating [Security] section...");
    // enable_steam_auth is boolean
    if (auto v = GetConfig(cfg, "Security.enable_steam_auth")) {
        Logger::Debug("[ConfigValidator::ValidateSecuritySection] Security.enable_steam_auth raw='%s', checking boolean validity", v->c_str());
        if (!ValidateBooleanString(*v)) {
            Logger::Error("[ConfigValidator::ValidateSecuritySection] Security.enable_steam_auth is not a valid boolean: '%s'", v->c_str());
            r.errors.push_back("Security.enable_steam_auth must be true or false");
            r.isValid = false;
        } else {
            Logger::Debug("[ConfigValidator::ValidateSecuritySection] Security.enable_steam_auth validated OK");
        }
    } else {
        Logger::Debug("[ConfigValidator::ValidateSecuritySection] Security.enable_steam_auth not present, skipping");
    }
    // custom_auth_tokens_file must exist if fallback_custom_auth is true
    bool fallbackCustomAuth = GetBool(cfg, "Security.fallback_custom_auth", false);
    Logger::Debug("[ConfigValidator::ValidateSecuritySection] fallback_custom_auth=%s", fallbackCustomAuth ? "true" : "false");
    if (fallbackCustomAuth) {
        std::string f = GetString(cfg, "Security.custom_auth_tokens_file", "");
        Logger::Debug("[ConfigValidator::ValidateSecuritySection] custom_auth_tokens_file='%s', checking existence", f.c_str());
        if (!std::filesystem::exists(f)) {
            Logger::Error("[ConfigValidator::ValidateSecuritySection] Token file does not exist: '%s'", f.c_str());
            r.errors.push_back("Security.custom_auth_tokens_file does not exist: " + f);
            r.isValid = false;
        } else {
            Logger::Debug("[ConfigValidator::ValidateSecuritySection] Token file exists: '%s'", f.c_str());
        }
    }
    Logger::Trace("[ConfigValidator::ValidateSecuritySection] Exit");
}

void ConfigValidator::ValidateEACSection(const std::map<std::string, std::string>& cfg, ValidationResult& r) {
    Logger::Trace("[ConfigValidator::ValidateEACSection] Entry");
    Logger::Debug("[ConfigValidator::ValidateEACSection] Validating [Security] anti_cheat_mode...");
    static const std::vector<std::string> modes = { "off", "safe", "emulate" };
    std::string m = GetString(cfg, "Security.anti_cheat_mode", "off");
    Logger::Debug("[ConfigValidator::ValidateEACSection] anti_cheat_mode value: '%s'", m.c_str());
    if (std::find(modes.begin(), modes.end(), m) == modes.end()) {
        Logger::Error("[ConfigValidator::ValidateEACSection] Invalid anti_cheat_mode '%s', must be off|safe|emulate", m.c_str());
        r.errors.push_back("Security.anti_cheat_mode must be one of: off, safe, emulate");
        r.isValid = false;
    } else {
        Logger::Debug("[ConfigValidator::ValidateEACSection] anti_cheat_mode validated OK: '%s'", m.c_str());
    }
    Logger::Trace("[ConfigValidator::ValidateEACSection] Exit");
}

void ConfigValidator::ValidateAntiCheatSection(const std::map<std::string, std::string>& cfg, ValidationResult& r) {
    Logger::Trace("[ConfigValidator::ValidateAntiCheatSection] Entry");
    Logger::Debug("[ConfigValidator::ValidateAntiCheatSection] Validating [AntiCheat] settings...");
    if (auto v = GetConfig(cfg, "AntiCheat.max_speed")) {
        float x = StringToFloat(*v);
        Logger::Debug("[ConfigValidator::ValidateAntiCheatSection] AntiCheat.max_speed raw='%s', parsed=%f", v->c_str(), x);
        if (x < 1.0f || x > 20.0f) {
            Logger::Error("[ConfigValidator::ValidateAntiCheatSection] AntiCheat.max_speed out of range [1.0,20.0]: %f", x);
            r.errors.push_back("AntiCheat.max_speed must be between 1.0 and 20.0");
            r.isValid = false;
        } else {
            Logger::Debug("[ConfigValidator::ValidateAntiCheatSection] AntiCheat.max_speed validated OK: %f", x);
        }
    } else {
        Logger::Debug("[ConfigValidator::ValidateAntiCheatSection] AntiCheat.max_speed not present, skipping");
    }
    Logger::Trace("[ConfigValidator::ValidateAntiCheatSection] Exit");
}

void ConfigValidator::ValidateLoggingSection(const std::map<std::string, std::string>& cfg, ValidationResult& r) {
    Logger::Trace("[ConfigValidator::ValidateLoggingSection] Entry");
    Logger::Debug("[ConfigValidator::ValidateLoggingSection] Validating [Logging] section...");
    if (auto v = GetConfig(cfg, "Logging.log_level")) {
        Logger::Debug("[ConfigValidator::ValidateLoggingSection] Logging.log_level raw='%s'", v->c_str());
        static const std::vector<std::string> levels = { "trace","debug","info","warn","error","fatal" };
        if (std::find(levels.begin(), levels.end(), *v) == levels.end()) {
            Logger::Error("[ConfigValidator::ValidateLoggingSection] Invalid log_level '%s', must be trace|debug|info|warn|error|fatal", v->c_str());
            r.errors.push_back("Logging.log_level must be one of: trace, debug, info, warn, error, fatal");
            r.isValid = false;
        } else {
            Logger::Debug("[ConfigValidator::ValidateLoggingSection] Logging.log_level validated OK: '%s'", v->c_str());
        }
    } else {
        Logger::Debug("[ConfigValidator::ValidateLoggingSection] Logging.log_level not present, skipping");
    }
    Logger::Trace("[ConfigValidator::ValidateLoggingSection] Exit");
}

void ConfigValidator::ValidatePerformanceSection(const std::map<std::string, std::string>& cfg, ValidationResult& r) {
    Logger::Trace("[ConfigValidator::ValidatePerformanceSection] Entry");
    Logger::Debug("[ConfigValidator::ValidatePerformanceSection] Validating [Performance] section...");
    if (auto v = GetConfig(cfg, "ThreadPool.worker_thread_count")) {
        int t = StringToInt(*v);
        int maxT = std::thread::hardware_concurrency();
        Logger::Debug("[ConfigValidator::ValidatePerformanceSection] worker_thread_count raw='%s', parsed=%d, hardware_concurrency=%d, maxAllowed=%d", v->c_str(), t, maxT, maxT*2);
        if (t < 0 || t > (int)maxT*2) {
            Logger::Error("[ConfigValidator::ValidatePerformanceSection] worker_thread_count=%d out of range [0,%d]", t, maxT*2);
            r.errors.push_back("ThreadPool.worker_thread_count must be between 0 and " + std::to_string(maxT*2));
            r.isValid = false;
        } else {
            Logger::Debug("[ConfigValidator::ValidatePerformanceSection] worker_thread_count validated OK: %d", t);
        }
    } else {
        Logger::Debug("[ConfigValidator::ValidatePerformanceSection] ThreadPool.worker_thread_count not present, skipping");
    }
    Logger::Trace("[ConfigValidator::ValidatePerformanceSection] Exit");
}

void ConfigValidator::ValidateGamePlaySection(const std::map<std::string, std::string>& cfg, ValidationResult& r) {
    Logger::Trace("[ConfigValidator::ValidateGamePlaySection] Entry");
    Logger::Debug("[ConfigValidator::ValidateGamePlaySection] Validating [Gameplay Settings] section...");
    // friendly_fire
    if (auto v = GetConfig(cfg, "Gameplay.friendly_fire")) {
        Logger::Debug("[ConfigValidator::ValidateGamePlaySection] Gameplay.friendly_fire raw='%s'", v->c_str());
        if (!ValidateBooleanString(*v)) {
            Logger::Error("[ConfigValidator::ValidateGamePlaySection] Gameplay.friendly_fire is not a valid boolean: '%s'", v->c_str());
            r.errors.push_back("Gameplay.friendly_fire must be true or false");
            r.isValid = false;
        } else {
            Logger::Debug("[ConfigValidator::ValidateGamePlaySection] Gameplay.friendly_fire validated OK: '%s'", v->c_str());
        }
    } else {
        Logger::Debug("[ConfigValidator::ValidateGamePlaySection] Gameplay.friendly_fire not present, skipping");
    }
    Logger::Trace("[ConfigValidator::ValidateGamePlaySection] Exit");
}

void ConfigValidator::ValidateCrossSectionDependencies(const std::map<std::string, std::string>& cfg, ValidationResult& r) {
    Logger::Trace("[ConfigValidator::ValidateCrossSectionDependencies] Entry");
    Logger::Debug("[ConfigValidator::ValidateCrossSectionDependencies] Validating cross-section dependencies...");
    bool secureMode = GetBool(cfg, "Security.secure_mode", false);
    std::string acMode = GetString(cfg, "Security.anti_cheat_mode", "off");
    Logger::Debug("[ConfigValidator::ValidateCrossSectionDependencies] secure_mode=%s, anti_cheat_mode='%s'", secureMode ? "true" : "false", acMode.c_str());
    // If secure mode then anti_cheat_mode must be off
    if (secureMode && acMode != "off")
    {
        Logger::Error("[ConfigValidator::ValidateCrossSectionDependencies] Conflict: secure_mode=true requires anti_cheat_mode=off, but got '%s'", acMode.c_str());
        r.errors.push_back("Security.secure_mode requires Security.anti_cheat_mode=off");
        r.isValid = false;
    } else {
        Logger::Debug("[ConfigValidator::ValidateCrossSectionDependencies] Cross-section dependency check passed");
    }
    Logger::Trace("[ConfigValidator::ValidateCrossSectionDependencies] Exit");
}

void ConfigValidator::CheckDeprecatedSettings(const std::map<std::string, std::string>& cfg, ValidationResult& r) {
    Logger::Trace("[ConfigValidator::CheckDeprecatedSettings] Entry");
    Logger::Debug("[ConfigValidator::CheckDeprecatedSettings] Checking deprecated settings...");
    static const std::map<std::string, std::string> deprecated = {
        { "Network.OldProtocol", "Use updated transport settings in [Network]" },
        { "Security.EnableEAC",     "Use Security.anti_cheat_mode instead" }
    };
    for (auto& [k, msg] : deprecated) {
        if (cfg.find(k) != cfg.end()) {
            Logger::Warn("[ConfigValidator::CheckDeprecatedSettings] Deprecated setting found: '%s' - %s", k.c_str(), msg.c_str());
            r.warnings.push_back("Deprecated '" + k + "': " + msg);
        } else {
            Logger::Trace("[ConfigValidator::CheckDeprecatedSettings] Deprecated key '%s' not present (good)", k.c_str());
        }
    }
    Logger::Trace("[ConfigValidator::CheckDeprecatedSettings] Exit");
}

std::optional<std::string> ConfigValidator::GetConfig(
    const std::map<std::string, std::string>& cfg,
    const std::string& key)
{
    Logger::Trace("[ConfigValidator::GetConfig] Entry - key='%s'", key.c_str());
    auto it = cfg.find(key);
    if (it != cfg.end()) {
        Logger::Trace("[ConfigValidator::GetConfig] Exit - key found, value='%s'", it->second.c_str());
        return it->second;
    }
    Logger::Trace("[ConfigValidator::GetConfig] Exit - key not found, returning nullopt");
    return std::nullopt;
}

std::optional<std::string> ConfigValidator::GetConfigValue(
    const std::map<std::string, std::string>& cfg,
    const std::string& key)
{
    Logger::Trace("[ConfigValidator::GetConfigValue] Entry - key='%s', delegating to GetConfig", key.c_str());
    auto result = GetConfig(cfg, key);
    Logger::Trace("[ConfigValidator::GetConfigValue] Exit - %s", result.has_value() ? "found" : "not found");
    return result;
}

std::string ConfigValidator::GetString(
    const std::map<std::string, std::string>& cfg,
    const std::string& key,
    const std::string& defaultValue)
{
    Logger::Trace("[ConfigValidator::GetString] Entry - key='%s', defaultValue='%s'", key.c_str(), defaultValue.c_str());
    auto it = cfg.find(key);
    if (it != cfg.end()) {
        Logger::Trace("[ConfigValidator::GetString] Exit - key found, returning '%s'", it->second.c_str());
        return it->second;
    }
    Logger::Trace("[ConfigValidator::GetString] Exit - key not found, returning default '%s'", defaultValue.c_str());
    return defaultValue;
}

bool ConfigValidator::GetBool(
    const std::map<std::string, std::string>& cfg,
    const std::string& key,
    bool defaultValue)
{
    Logger::Trace("[ConfigValidator::GetBool] Entry - key='%s', defaultValue=%s", key.c_str(), defaultValue ? "true" : "false");
    auto it = cfg.find(key);
    if (it == cfg.end()) {
        Logger::Trace("[ConfigValidator::GetBool] Exit - key not found, returning default %s", defaultValue ? "true" : "false");
        return defaultValue;
    }
    auto s = StringUtils::ToLower(it->second);
    if (s == "true" || s == "1") {
        Logger::Trace("[ConfigValidator::GetBool] Exit - parsed as true (raw='%s')", it->second.c_str());
        return true;
    }
    if (s == "false" || s == "0") {
        Logger::Trace("[ConfigValidator::GetBool] Exit - parsed as false (raw='%s')", it->second.c_str());
        return false;
    }
    Logger::Debug("[ConfigValidator::GetBool] Could not parse '%s' as bool for key '%s', returning default %s", it->second.c_str(), key.c_str(), defaultValue ? "true" : "false");
    Logger::Trace("[ConfigValidator::GetBool] Exit - returning default due to unrecognized value");
    return defaultValue;
}

bool ConfigValidator::ValidateBooleanString(const std::string& v) {
    Logger::Trace("[ConfigValidator::ValidateBooleanString] Entry - v='%s'", v.c_str());
    auto s = StringUtils::ToLower(v);
    bool result = s=="true"||s=="false"||s=="1"||s=="0";
    Logger::Trace("[ConfigValidator::ValidateBooleanString] Exit - returning %s for input '%s'", result ? "true" : "false", v.c_str());
    return result;
}

bool ConfigValidator::LoadConfigurationFromFile(
    const std::string& file,
    std::map<std::string, std::string>& cfg)
{
    Logger::Trace("[ConfigValidator::LoadConfigurationFromFile] Entry - file='%s'", file.c_str());
    std::ifstream in(file);
    if (!in.is_open()) {
        Logger::Error("[ConfigValidator::LoadConfigurationFromFile] Cannot open file: '%s'", file.c_str());
        Logger::Trace("[ConfigValidator::LoadConfigurationFromFile] Exit - returning false");
        return false;
    }
    Logger::Debug("[ConfigValidator::LoadConfigurationFromFile] File opened successfully: '%s'", file.c_str());
    std::string line, section;
    size_t lineNumber = 0;
    while (std::getline(in, line)) {
        ++lineNumber;
        if (auto p = line.find('#'); p!=std::string::npos) line.erase(p);
        line = StringUtils::Trim(line);
        if (line.empty()) continue;
        if (line.front()=='[' && line.back()==']') {
            section = line.substr(1, line.size()-2);
            Logger::Trace("[ConfigValidator::LoadConfigurationFromFile] Line %zu: section header [%s]", lineNumber, section.c_str());
            continue;
        }
        auto eq = line.find('=');
        if (eq==std::string::npos) {
            Logger::Warn("[ConfigValidator::LoadConfigurationFromFile] Line %zu: no '=' separator, skipping: '%s'", lineNumber, line.c_str());
            continue;
        }
        auto key = StringUtils::Trim(line.substr(0, eq));
        auto val = StringUtils::Trim(line.substr(eq+1));
        std::string fullKey = section.empty() ? key : section+"."+key;
        cfg[fullKey] = val;
        Logger::Trace("[ConfigValidator::LoadConfigurationFromFile] Line %zu: loaded %s='%s'", lineNumber, fullKey.c_str(), val.c_str());
    }
    Logger::Debug("[ConfigValidator::LoadConfigurationFromFile] Parsed %zu config entries from '%s'", cfg.size(), file.c_str());
    Logger::Trace("[ConfigValidator::LoadConfigurationFromFile] Exit - returning true");
    return true;
}

void ConfigValidator::LogValidationResults(const ValidationResult& r) {
    Logger::Trace("[ConfigValidator::LogValidationResults] Entry - isValid=%s, errors=%zu, warnings=%zu",
                  r.isValid ? "true" : "false", r.errors.size(), r.warnings.size());
    if (r.isValid) {
        Logger::Info("[ConfigValidator::LogValidationResults] Configuration validation PASSED");
        for (size_t i = 0; i < r.warnings.size(); ++i) {
            Logger::Warn("[ConfigValidator::LogValidationResults] Warning %zu/%zu: %s", i + 1, r.warnings.size(), r.warnings[i].c_str());
        }
    } else {
        Logger::Error("[ConfigValidator::LogValidationResults] Configuration validation FAILED");
        for (size_t i = 0; i < r.errors.size(); ++i) {
            Logger::Error("[ConfigValidator::LogValidationResults] Error %zu/%zu: %s", i + 1, r.errors.size(), r.errors[i].c_str());
        }
        for (size_t i = 0; i < r.warnings.size(); ++i) {
            Logger::Warn("[ConfigValidator::LogValidationResults] Warning %zu/%zu: %s", i + 1, r.warnings.size(), r.warnings[i].c_str());
        }
    }
    Logger::Trace("[ConfigValidator::LogValidationResults] Exit");
}

void ConfigValidator::ValidateRequired(
    const std::map<std::string, std::string>& cfg,
    const std::string& key,
    ValidationResult& r)
{
    Logger::Trace("[ConfigValidator::ValidateRequired] Entry - key='%s'", key.c_str());
    if (cfg.find(key) == cfg.end()) {
        Logger::Error("[ConfigValidator::ValidateRequired] Required key missing: '%s'", key.c_str());
        r.errors.push_back("Required key missing: " + key);
        r.isValid = false;
    } else {
        Logger::Debug("[ConfigValidator::ValidateRequired] Required key '%s' is present", key.c_str());
    }
    Logger::Trace("[ConfigValidator::ValidateRequired] Exit");
}

void ConfigValidator::ValidatePortRange(
    const std::map<std::string, std::string>& cfg,
    const std::string& key,
    int lo, int hi,
    ValidationResult& r)
{
    Logger::Trace("[ConfigValidator::ValidatePortRange] Entry - key='%s', lo=%d, hi=%d", key.c_str(), lo, hi);
    if (auto v = GetConfigValue(cfg, key)) {
        int x = StringToInt(*v);
        Logger::Debug("[ConfigValidator::ValidatePortRange] key='%s', raw='%s', parsed=%d, range=[%d,%d]", key.c_str(), v->c_str(), x, lo, hi);
        if (x<lo||x>hi) {
            Logger::Error("[ConfigValidator::ValidatePortRange] %s=%d out of range [%d,%d]", key.c_str(), x, lo, hi);
            r.errors.push_back(key + " must be between " + std::to_string(lo) +
                                " and " + std::to_string(hi));
            r.isValid = false;
        } else {
            Logger::Debug("[ConfigValidator::ValidatePortRange] %s=%d validated OK within [%d,%d]", key.c_str(), x, lo, hi);
        }
    } else {
        Logger::Debug("[ConfigValidator::ValidatePortRange] Key '%s' not found, skipping port range validation", key.c_str());
    }
    Logger::Trace("[ConfigValidator::ValidatePortRange] Exit");
}

int ConfigValidator::StringToInt(const std::string& s) {
    Logger::Trace("[ConfigValidator::StringToInt] Entry - s='%s'", s.c_str());
    try {
        int result = std::stoi(s);
        Logger::Trace("[ConfigValidator::StringToInt] Exit - parsed %d", result);
        return result;
    } catch(const std::exception& e) {
        Logger::Error("[ConfigValidator::StringToInt] Failed to parse '%s' as int: %s, returning 0", s.c_str(), e.what());
        return 0;
    }
}

float ConfigValidator::StringToFloat(const std::string& s) {
    Logger::Trace("[ConfigValidator::StringToFloat] Entry - s='%s'", s.c_str());
    try {
        float result = std::stof(s);
        Logger::Trace("[ConfigValidator::StringToFloat] Exit - parsed %f", result);
        return result;
    } catch(const std::exception& e) {
        Logger::Error("[ConfigValidator::StringToFloat] Failed to parse '%s' as float: %s, returning 0.0", s.c_str(), e.what());
        return 0.0f;
    }
}