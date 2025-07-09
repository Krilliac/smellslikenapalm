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
    Logger::Info("ConfigValidator initialized");
    InitializeValidationRules();
}

ConfigValidator::~ConfigValidator() = default;

bool ConfigValidator::Initialize() {
    Logger::Info("Initializing Configuration Validator...");
    InitializeCustomValidators();
    Logger::Info("Configuration Validator initialized successfully");
    return true;
}

ValidationResult ConfigValidator::ValidateConfiguration(const std::map<std::string, std::string>& config) {
    Logger::Debug("Starting comprehensive configuration validation...");
    ValidationResult result; result.isValid = true;

    ValidateGeneralSection(config, result);
    ValidateNetworkSection(config, result);
    ValidateSecuritySection(config, result);
    ValidateEACSection(config, result);
    ValidateAntiCheatSection(config, result);
    ValidateLoggingSection(config, result);
    ValidatePerformanceSection(config, result);
    ValidateGamePlaySection(config, result);
    ValidateCrossSectionDependencies(config, result);
    CheckDeprecatedSettings(config, result);

    if (result.isValid)
        Logger::Info("Configuration validation passed with %zu warnings", result.warnings.size());
    else
        Logger::Error("Configuration validation failed with %zu errors and %zu warnings",
                      result.errors.size(), result.warnings.size());

    return result;
}

bool ConfigValidator::ValidateConfigurationFile(const std::string& configFile) {
    Logger::Info("Validating configuration file: %s", configFile.c_str());
    if (!std::filesystem::exists(configFile)) {
        Logger::Error("Configuration file does not exist: %s", configFile.c_str());
        return false;
    }
    std::map<std::string, std::string> config;
    if (!LoadConfigurationFromFile(configFile, config)) {
        Logger::Error("Failed to load configuration from file: %s", configFile.c_str());
        return false;
    }
    auto result = ValidateConfiguration(config);
    LogValidationResults(result);
    return result.isValid;
}

void ConfigValidator::ValidateGeneralSection(const std::map<std::string, std::string>& cfg, ValidationResult& r) {
    Logger::Debug("Validating [General] section...");
    // server_name
    ValidateRequired(cfg, "General.server_name", r);
    // max_players
    if (auto v = GetConfig(cfg, "General.max_players")) {
        int x = StringToInt(*v);
        if (x < 1 || x > 128) {
            r.errors.push_back("General.max_players must be between 1 and 128");
            r.isValid = false;
        }
    }
    // tick_rate
    if (auto v = GetConfig(cfg, "General.tick_rate")) {
        int x = StringToInt(*v);
        if (x < 10 || x > 128) {
            r.errors.push_back("General.tick_rate must be between 10 and 128");
            r.isValid = false;
        }
    }
}

void ConfigValidator::ValidateNetworkSection(const std::map<std::string, std::string>& cfg, ValidationResult& r) {
    Logger::Debug("Validating [Network] section...");
    // port
    if (auto v = GetConfig(cfg, "Network.port")) {
        ValidatePortRange(cfg, "Network.port", 1024, 65535, r);
    }
    // max_packet_size
    if (auto v = GetConfig(cfg, "Network.max_packet_size")) {
        int x = StringToInt(*v);
        if (x < 64 || x > 65536) {
            r.errors.push_back("Network.max_packet_size must be between 64 and 65536");
            r.isValid = false;
        }
        if (x > 1500)
            r.warnings.push_back("Network.max_packet_size above 1500 may cause fragmentation");
    }
}

void ConfigValidator::ValidateSecuritySection(const std::map<std::string, std::string>& cfg, ValidationResult& r) {
    Logger::Debug("Validating [Security] section...");
    // enable_steam_auth is boolean
    if (auto v = GetConfig(cfg, "Security.enable_steam_auth")) {
        if (!ValidateBooleanString(*v)) {
            r.errors.push_back("Security.enable_steam_auth must be true or false");
            r.isValid = false;
        }
    }
    // custom_auth_tokens_file must exist if fallback_custom_auth is true
    if (GetBool(cfg, "Security.fallback_custom_auth", false)) {
        std::string f = GetString(cfg, "Security.custom_auth_tokens_file", "");
        if (!Utils::FileExists(f)) {
            r.errors.push_back("Security.custom_auth_tokens_file does not exist: " + f);
            r.isValid = false;
        }
    }
}

void ConfigValidator::ValidateEACSection(const std::map<std::string, std::string>& cfg, ValidationResult& r) {
    Logger::Debug("Validating [Security] anti_cheat_mode...");
    static const std::vector<std::string> modes = { "off", "safe", "emulate" };
    std::string m = GetString(cfg, "Security.anti_cheat_mode", "off");
    if (std::find(modes.begin(), modes.end(), m) == modes.end()) {
        r.errors.push_back("Security.anti_cheat_mode must be one of: off, safe, emulate");
        r.isValid = false;
    }
}

void ConfigValidator::ValidateAntiCheatSection(const std::map<std::string, std::string>& cfg, ValidationResult& r) {
    Logger::Debug("Validating [AntiCheat] settings...");
    if (auto v = GetConfig(cfg, "AntiCheat.max_speed")) {
        float x = StringToFloat(*v);
        if (x < 1.0f || x > 20.0f) {
            r.errors.push_back("AntiCheat.max_speed must be between 1.0 and 20.0");
            r.isValid = false;
        }
    }
}

void ConfigValidator::ValidateLoggingSection(const std::map<std::string, std::string>& cfg, ValidationResult& r) {
    Logger::Debug("Validating [Logging] section...");
    if (auto v = GetConfig(cfg, "Logging.log_level")) {
        static const std::vector<std::string> levels = { "trace","debug","info","warn","error","fatal" };
        if (std::find(levels.begin(), levels.end(), *v) == levels.end()) {
            r.errors.push_back("Logging.log_level must be one of: trace, debug, info, warn, error, fatal");
            r.isValid = false;
        }
    }
}

void ConfigValidator::ValidatePerformanceSection(const std::map<std::string, std::string>& cfg, ValidationResult& r) {
    Logger::Debug("Validating [Performance] section...");
    if (auto v = GetConfig(cfg, "ThreadPool.worker_thread_count")) {
        int t = StringToInt(*v);
        int maxT = std::thread::hardware_concurrency();
        if (t < 0 || t > (int)maxT*2) {
            r.errors.push_back("ThreadPool.worker_thread_count must be between 0 and " + std::to_string(maxT*2));
            r.isValid = false;
        }
    }
}

void ConfigValidator::ValidateGamePlaySection(const std::map<std::string, std::string>& cfg, ValidationResult& r) {
    Logger::Debug("Validating [Gameplay Settings] section...");
    // friendly_fire
    if (auto v = GetConfig(cfg, "Gameplay.friendly_fire")) {
        if (!ValidateBooleanString(*v)) {
            r.errors.push_back("Gameplay.friendly_fire must be true or false");
            r.isValid = false;
        }
    }
}

void ConfigValidator::ValidateCrossSectionDependencies(const std::map<std::string, std::string>& cfg, ValidationResult& r) {
    Logger::Debug("Validating cross-section dependencies...");
    // If secure mode then anti_cheat_mode must be off
    if (GetBool(cfg, "Security.secure_mode", false) &&
        GetString(cfg, "Security.anti_cheat_mode", "off") != "off")
    {
        r.errors.push_back("Security.secure_mode requires Security.anti_cheat_mode=off");
        r.isValid = false;
    }
}

void ConfigValidator::CheckDeprecatedSettings(const std::map<std::string, std::string>& cfg, ValidationResult& r) {
    Logger::Debug("Checking deprecated settings...");
    static const std::map<std::string, std::string> deprecated = {
        { "Network.OldProtocol", "Use updated transport settings in [Network]" },
        { "Security.EnableEAC",     "Use Security.anti_cheat_mode instead" }
    };
    for (auto& [k, msg] : deprecated) {
        if (cfg.find(k) != cfg.end())
            r.warnings.push_back("Deprecated '" + k + "': " + msg);
    }
}

std::optional<std::string> ConfigValidator::GetConfigValue(
    const std::map<std::string, std::string>& cfg,
    const std::string& key)
{
    auto it = cfg.find(key);
    if (it != cfg.end()) return it->second;
    return std::nullopt;
}

bool ConfigValidator::ValidateBooleanString(const std::string& v) {
    auto s = StringUtils::ToLower(v);
    return s=="true"||s=="false"||s=="1"||s=="0";
}

bool ConfigValidator::LoadConfigurationFromFile(
    const std::string& file,
    std::map<std::string, std::string>& cfg)
{
    std::ifstream in(file);
    if (!in.is_open()) return false;
    std::string line, section;
    while (std::getline(in, line)) {
        if (auto p = line.find('#'); p!=std::string::npos) line.erase(p);
        line = StringUtils::Trim(line);
        if (line.empty()) continue;
        if (line.front()=='[' && line.back()==']') {
            section = line.substr(1, line.size()-2);
            continue;
        }
        auto eq = line.find('=');
        if (eq==std::string::npos) continue;
        auto key = StringUtils::Trim(line.substr(0, eq));
        auto val = StringUtils::Trim(line.substr(eq+1));
        cfg[ section.empty() ? key : section+"."+key ] = val;
    }
    return true;
}

void ConfigValidator::LogValidationResults(const ValidationResult& r) {
    if (r.isValid) {
        Logger::Info("Configuration validation PASSED");
        for (auto& w : r.warnings) Logger::Warn("Warning: %s", w.c_str());
    } else {
        Logger::Error("Configuration validation FAILED");
        for (auto& e : r.errors) Logger::Error("Error: %s", e.c_str());
        for (auto& w : r.warnings) Logger::Warn("Warning: %s", w.c_str());
    }
}

void ConfigValidator::ValidateRequired(
    const std::map<std::string, std::string>& cfg,
    const std::string& key,
    ValidationResult& r)
{
    if (cfg.find(key) == cfg.end()) {
        r.errors.push_back("Required key missing: " + key);
        r.isValid = false;
    }
}

void ConfigValidator::ValidatePortRange(
    const std::map<std::string, std::string>& cfg,
    const std::string& key,
    int lo, int hi,
    ValidationResult& r)
{
    if (auto v = GetConfigValue(cfg, key)) {
        int x = StringToInt(*v);
        if (x<lo||x>hi) {
            r.errors.push_back(key + " must be between " + std::to_string(lo) +
                                " and " + std::to_string(hi));
            r.isValid = false;
        }
    }
}

int ConfigValidator::StringToInt(const std::string& s) {
    try { return std::stoi(s); } catch(...) { return 0; }
}

float ConfigValidator::StringToFloat(const std::string& s) {
    try { return std::stof(s); } catch(...) { return 0.0f; }
}