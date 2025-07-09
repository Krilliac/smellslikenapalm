// src/Config/ConfigValidator.cpp - Complete implementation for RS2V Server configuration validation
#include "Config/ConfigValidator.h"
#include "Utils/Logger.h"
#include "Utils/StringUtils.h"
#include "Utils/FileUtils.h"
#include <algorithm>
#include <regex>
#include <filesystem>
#include <set>

ConfigValidator::ConfigValidator() {
    Logger::Info("ConfigValidator initialized");
    InitializeValidationRules();
}

ConfigValidator::~ConfigValidator() = default;

bool ConfigValidator::Initialize() {
    Logger::Info("Initializing Configuration Validator...");
    
    // Load validation schemas
    if (!LoadValidationSchemas()) {
        Logger::Error("Failed to load validation schemas");
        return false;
    }
    
    // Initialize custom validators
    InitializeCustomValidators();
    
    Logger::Info("Configuration Validator initialized successfully");
    return true;
}

ValidationResult ConfigValidator::ValidateConfiguration(const std::map<std::string, std::string>& config) {
    Logger::Debug("Starting comprehensive configuration validation...");
    
    ValidationResult result;
    result.isValid = true;
    
    // Validate each configuration section
    ValidateServerSection(config, result);
    ValidateNetworkSection(config, result);
    ValidateGameSection(config, result);
    ValidateSecuritySection(config, result);
    ValidateEACSection(config, result);
    ValidateAntiCheatSection(config, result);
    ValidateLoggingSection(config, result);
    ValidatePerformanceSection(config, result);
    
    // Validate cross-section dependencies
    ValidateCrossSectionDependencies(config, result);
    
    // Validate security requirements
    ValidateSecurityRequirements(config, result);
    
    // Check for deprecated settings
    CheckDeprecatedSettings(config, result);
    
    // Final validation pass
    if (result.isValid) {
        Logger::Info("Configuration validation passed with %zu warnings", result.warnings.size());
    } else {
        Logger::Error("Configuration validation failed with %zu errors and %zu warnings", 
                     result.errors.size(), result.warnings.size());
    }
    
    return result;
}

bool ConfigValidator::ValidateConfigurationFile(const std::string& configFile) {
    Logger::Info("Validating configuration file: %s", configFile.c_str());
    
    // Check if file exists
    if (!std::filesystem::exists(configFile)) {
        Logger::Error("Configuration file does not exist: %s", configFile.c_str());
        return false;
    }
    
    // Load and parse configuration
    std::map<std::string, std::string> config;
    if (!LoadConfigurationFromFile(configFile, config)) {
        Logger::Error("Failed to load configuration from file: %s", configFile.c_str());
        return false;
    }
    
    // Validate loaded configuration
    ValidationResult result = ValidateConfiguration(config);
    
    // Log validation results
    LogValidationResults(result);
    
    return result.isValid;
}

ValidationResult ConfigValidator::ValidateConfigValue(const std::string& key, const std::string& value) {
    ValidationResult result;
    result.isValid = true;
    
    Logger::Debug("Validating individual config value: %s = %s", key.c_str(), value.c_str());
    
    // Find validation rule for this key
    auto it = m_validationRules.find(key);
    if (it == m_validationRules.end()) {
        result.warnings.push_back("No validation rule found for key: " + key);
        return result;
    }
    
    const ValidationRule& rule = it->second;
    
    // Validate based on data type
    switch (rule.dataType) {
        case ConfigDataType::STRING:
            ValidateStringValue(key, value, rule, result);
            break;
        case ConfigDataType::INTEGER:
            ValidateIntegerValue(key, value, rule, result);
            break;
        case ConfigDataType::FLOAT:
            ValidateFloatValue(key, value, rule, result);
            break;
        case ConfigDataType::BOOLEAN:
            ValidateBooleanValue(key, value, rule, result);
            break;
        case ConfigDataType::PATH:
            ValidatePathValue(key, value, rule, result);
            break;
        case ConfigDataType::IP_ADDRESS:
            ValidateIPAddressValue(key, value, rule, result);
            break;
        case ConfigDataType::PORT:
            ValidatePortValue(key, value, rule, result);
            break;
        case ConfigDataType::ENUM:
            ValidateEnumValue(key, value, rule, result);
            break;
    }
    
    return result;
}

void ConfigValidator::ValidateServerSection(const std::map<std::string, std::string>& config, ValidationResult& result) {
    Logger::Debug("Validating Server section...");
    
    // Server Name validation
    ValidateRequired(config, "Server.ServerName", result);
    if (auto value = GetConfigValue(config, "Server.ServerName")) {
        if (value->length() > 64) {
            result.errors.push_back("Server.ServerName must be 64 characters or less");
            result.isValid = false;
        }
        if (value->empty()) {
            result.errors.push_back("Server.ServerName cannot be empty");
            result.isValid = false;
        }
    }
    
    // Max Players validation
    ValidateRequired(config, "Server.MaxPlayers", result);
    if (auto value = GetConfigValue(config, "Server.MaxPlayers")) {
        int maxPlayers = StringToInt(*value);
        if (maxPlayers < 1 || maxPlayers > 128) {
            result.errors.push_back("Server.MaxPlayers must be between 1 and 128");
            result.isValid = false;
        }
    }
    
    // Game Port validation
    ValidateRequired(config, "Server.GamePort", result);
    ValidatePortRange(config, "Server.GamePort", 1024, 65535, result);
    
    // Query Port validation
    ValidateRequired(config, "Server.QueryPort", result);
    ValidatePortRange(config, "Server.QueryPort", 1024, 65535, result);
    
    // Check port conflicts
    auto gamePort = GetConfigValue(config, "Server.GamePort");
    auto queryPort = GetConfigValue(config, "Server.QueryPort");
    if (gamePort && queryPort && *gamePort == *queryPort) {
        result.errors.push_back("Server.GamePort and Server.QueryPort cannot be the same");
        result.isValid = false;
    }
    
    // Tick Rate validation
    if (auto value = GetConfigValue(config, "Server.TickRate")) {
        int tickRate = StringToInt(*value);
        if (tickRate < 10 || tickRate > 128) {
            result.errors.push_back("Server.TickRate must be between 10 and 128");
            result.isValid = false;
        }
        if (tickRate > 60) {
            result.warnings.push_back("Server.TickRate above 60 may impact performance");
        }
    }
    
    // Map Name validation
    ValidateRequired(config, "Server.MapName", result);
    if (auto value = GetConfigValue(config, "Server.MapName")) {
        if (!ValidateMapName(*value)) {
            result.warnings.push_back("Server.MapName may not be a valid RS2V map: " + *value);
        }
    }
    
    Logger::Debug("Server section validation complete");
}

void ConfigValidator::ValidateNetworkSection(const std::map<std::string, std::string>& config, ValidationResult& result) {
    Logger::Debug("Validating Network section...");
    
    // Max Packet Size validation
    if (auto value = GetConfigValue(config, "Network.MaxPacketSize")) {
        int packetSize = StringToInt(*value);
        if (packetSize < 64 || packetSize > 65536) {
            result.errors.push_back("Network.MaxPacketSize must be between 64 and 65536");
            result.isValid = false;
        }
        if (packetSize > 1500) {
            result.warnings.push_back("Network.MaxPacketSize above 1500 may cause fragmentation");
        }
    }
    
    // Timeout validation
    if (auto value = GetConfigValue(config, "Network.TimeoutSeconds")) {
        int timeout = StringToInt(*value);
        if (timeout < 5 || timeout > 300) {
            result.errors.push_back("Network.TimeoutSeconds must be between 5 and 300");
            result.isValid = false;
        }
    }
    
    // Compression settings
    if (auto value = GetConfigValue(config, "Network.CompressionEnabled")) {
        if (!ValidateBooleanString(*value)) {
            result.errors.push_back("Network.CompressionEnabled must be true or false");
            result.isValid = false;
        }
    }
    
    // Network interface validation
    if (auto value = GetConfigValue(config, "Network.BindAddress")) {
        if (!ValidateIPAddress(*value)) {
            result.errors.push_back("Network.BindAddress is not a valid IP address: " + *value);
            result.isValid = false;
        }
    }
    
    Logger::Debug("Network section validation complete");
}

void ConfigValidator::ValidateGameSection(const std::map<std::string, std::string>& config, ValidationResult& result) {
    Logger::Debug("Validating Game section...");
    
    // Map Name validation (more detailed)
    ValidateRequired(config, "Game.MapName", result);
    if (auto value = GetConfigValue(config, "Game.MapName")) {
        if (!ValidateRS2VMapName(*value)) {
            result.errors.push_back("Game.MapName is not a valid RS2V map: " + *value);
            result.isValid = false;
        }
    }
    
    // Game Mode validation
    ValidateRequired(config, "Game.GameMode", result);
    if (auto value = GetConfigValue(config, "Game.GameMode")) {
        if (!ValidateGameMode(*value)) {
            result.errors.push_back("Game.GameMode is not a valid RS2V game mode: " + *value);
            result.isValid = false;
        }
    }
    
    // Round Time validation
    if (auto value = GetConfigValue(config, "Game.RoundTimeLimit")) {
        int roundTime = StringToInt(*value);
        if (roundTime < 60 || roundTime > 7200) {
            result.errors.push_back("Game.RoundTimeLimit must be between 60 and 7200 seconds");
            result.isValid = false;
        }
    }
    
    // Team settings validation
    if (auto value = GetConfigValue(config, "Game.AllowTeamSwitch")) {
        if (!ValidateBooleanString(*value)) {
            result.errors.push_back("Game.AllowTeamSwitch must be true or false");
            result.isValid = false;
        }
    }
    
    // Friendly fire validation
    if (auto value = GetConfigValue(config, "Game.FriendlyFireScale")) {
        float ffScale = StringToFloat(*value);
        if (ffScale < 0.0f || ffScale > 2.0f) {
            result.errors.push_back("Game.FriendlyFireScale must be between 0.0 and 2.0");
            result.isValid = false;
        }
    }
    
    Logger::Debug("Game section validation complete");
}

void ConfigValidator::ValidateSecuritySection(const std::map<std::string, std::string>& config, ValidationResult& result) {
    Logger::Debug("Validating Security section...");
    
    // Secure Mode validation
    if (auto value = GetConfigValue(config, "Security.SecureMode")) {
        if (!ValidateBooleanString(*value)) {
            result.errors.push_back("Security.SecureMode must be true or false");
            result.isValid = false;
        } else if (StringToBool(*value)) {
            // In secure mode, validate that security requirements are met
            ValidateSecureModeRequirements(config, result);
        }
    }
    
    // Privacy Mode validation
    if (auto value = GetConfigValue(config, "Security.PrivacyMode")) {
        if (!ValidateBooleanString(*value)) {
            result.errors.push_back("Security.PrivacyMode must be true or false");
            result.isValid = false;
        }
    }
    
    // Telemetry validation
    if (auto value = GetConfigValue(config, "Security.AllowTelemetry")) {
        if (!ValidateBooleanString(*value)) {
            result.errors.push_back("Security.AllowTelemetry must be true or false");
            result.isValid = false;
        }
        
        // Security warning if telemetry is enabled
        if (StringToBool(*value)) {
            result.warnings.push_back("Security.AllowTelemetry is enabled - this may send data to third parties");
        }
    }
    
    // Admin password validation
    if (auto value = GetConfigValue(config, "Security.AdminPassword")) {
        if (!ValidatePasswordStrength(*value)) {
            result.warnings.push_back("Security.AdminPassword should be at least 8 characters with mixed case, numbers, and symbols");
        }
    }
    
    // Encryption settings
    if (auto value = GetConfigValue(config, "Security.EnableEncryption")) {
        if (!ValidateBooleanString(*value)) {
            result.errors.push_back("Security.EnableEncryption must be true or false");
            result.isValid = false;
        }
    }
    
    Logger::Debug("Security section validation complete");
}

void ConfigValidator::ValidateEACSection(const std::map<std::string, std::string>& config, ValidationResult& result) {
    Logger::Debug("Validating EAC section...");
    
    // EAC Mode validation
    if (auto value = GetConfigValue(config, "EAC.Mode")) {
        std::vector<std::string> validModes = {"DISABLED", "PASSIVE_LOGGING", "BLOCK_MODE"};
        if (std::find(validModes.begin(), validModes.end(), *value) == validModes.end()) {
            result.errors.push_back("EAC.Mode must be one of: DISABLED, PASSIVE_LOGGING, BLOCK_MODE");
            result.isValid = false;
        }
        
        // Security check - EAC should be disabled for security
        if (*value != "DISABLED") {
            result.warnings.push_back("EAC.Mode is not DISABLED - this may allow telemetry to Epic Games");
        }
    }
    
    // EAC Proxy Port validation
    if (auto value = GetConfigValue(config, "EAC.ProxyPort")) {
        ValidatePortRange(config, "EAC.ProxyPort", 1024, 65535, result);
        
        // Check for port conflicts with server ports
        auto gamePort = GetConfigValue(config, "Server.GamePort");
        auto queryPort = GetConfigValue(config, "Server.QueryPort");
        if ((gamePort && *value == *gamePort) || (queryPort && *value == *queryPort)) {
            result.errors.push_back("EAC.ProxyPort conflicts with server ports");
            result.isValid = false;
        }
    }
    
    // EAC logging validation
    if (auto value = GetConfigValue(config, "EAC.EnableLogging")) {
        if (!ValidateBooleanString(*value)) {
            result.errors.push_back("EAC.EnableLogging must be true or false");
            result.isValid = false;
        }
    }
    
    // Forward to Epic validation
    if (auto value = GetConfigValue(config, "EAC.ForwardToEpic")) {
        if (!ValidateBooleanString(*value)) {
            result.errors.push_back("EAC.ForwardToEpic must be true or false");
            result.isValid = false;
        }
        
        // Security warning if forwarding is enabled
        if (StringToBool(*value)) {
            result.warnings.push_back("EAC.ForwardToEpic is enabled - this will send data to Epic Games");
        }
    }
    
    // Validate EAC configuration consistency
    ValidateEACConsistency(config, result);
    
    Logger::Debug("EAC section validation complete");
}

void ConfigValidator::ValidateAntiCheatSection(const std::map<std::string, std::string>& config, ValidationResult& result) {
    Logger::Debug("Validating AntiCheat section...");
    
    // Movement validation settings
    if (auto value = GetConfigValue(config, "AntiCheat.MaxSpeed")) {
        float maxSpeed = StringToFloat(*value);
        if (maxSpeed < 1.0f || maxSpeed > 20.0f) {
            result.errors.push_back("AntiCheat.MaxSpeed must be between 1.0 and 20.0");
            result.isValid = false;
        }
        if (maxSpeed > 10.0f) {
            result.warnings.push_back("AntiCheat.MaxSpeed above 10.0 may allow speed hacks");
        }
    }
    
    // Headshot ratio validation
    if (auto value = GetConfigValue(config, "AntiCheat.MaxHeadshotRatio")) {
        float ratio = StringToFloat(*value);
        if (ratio < 0.0f || ratio > 1.0f) {
            result.errors.push_back("AntiCheat.MaxHeadshotRatio must be between 0.0 and 1.0");
            result.isValid = false;
        }
        if (ratio > 0.9f) {
            result.warnings.push_back("AntiCheat.MaxHeadshotRatio above 0.9 may be too permissive");
        }
    }
    
    // Accuracy threshold validation
    if (auto value = GetConfigValue(config, "AntiCheat.MaxAccuracyThreshold")) {
        float accuracy = StringToFloat(*value);
        if (accuracy < 0.0f || accuracy > 1.0f) {
            result.errors.push_back("AntiCheat.MaxAccuracyThreshold must be between 0.0 and 1.0");
            result.isValid = false;
        }
        if (accuracy > 0.98f) {
            result.warnings.push_back("AntiCheat.MaxAccuracyThreshold above 0.98 may be too permissive");
        }
    }
    
    // Validation strictness
    if (auto value = GetConfigValue(config, "AntiCheat.ValidationStrictness")) {
        float strictness = StringToFloat(*value);
        if (strictness < 0.1f || strictness > 2.0f) {
            result.errors.push_back("AntiCheat.ValidationStrictness must be between 0.1 and 2.0");
            result.isValid = false;
        }
    }
    
    // Minimum reaction time validation
    if (auto value = GetConfigValue(config, "AntiCheat.MinReactionTimeMs")) {
        int reactionTime = StringToInt(*value);
        if (reactionTime < 50 || reactionTime > 1000) {
            result.errors.push_back("AntiCheat.MinReactionTimeMs must be between 50 and 1000");
            result.isValid = false;
        }
        if (reactionTime < 100) {
            result.warnings.push_back("AntiCheat.MinReactionTimeMs below 100 may cause false positives");
        }
    }
    
    // Custom anti-cheat validation
    if (auto value = GetConfigValue(config, "AntiCheat.EnableCustomAntiCheat")) {
        if (!ValidateBooleanString(*value)) {
            result.errors.push_back("AntiCheat.EnableCustomAntiCheat must be true or false");
            result.isValid = false;
        }
    }
    
    Logger::Debug("AntiCheat section validation complete");
}

void ConfigValidator::ValidateLoggingSection(const std::map<std::string, std::string>& config, ValidationResult& result) {
    Logger::Debug("Validating Logging section...");
    
    // Log level validation
    if (auto value = GetConfigValue(config, "Logging.LogLevel")) {
        std::vector<std::string> validLevels = {"DEBUG", "INFO", "WARN", "ERROR", "FATAL"};
        if (std::find(validLevels.begin(), validLevels.end(), *value) == validLevels.end()) {
            result.errors.push_back("Logging.LogLevel must be one of: DEBUG, INFO, WARN, ERROR, FATAL");
            result.isValid = false;
        }
    }
    
    // Log file validation
    if (auto value = GetConfigValue(config, "Logging.LogFile")) {
        if (!ValidateFilePath(*value)) {
            result.errors.push_back("Logging.LogFile is not a valid file path: " + *value);
            result.isValid = false;
        }
        
        // Check if directory exists
        std::filesystem::path logPath(*value);
        if (!std::filesystem::exists(logPath.parent_path())) {
            result.warnings.push_back("Logging.LogFile directory does not exist: " + logPath.parent_path().string());
        }
    }
    
    // Log rotation validation
    if (auto value = GetConfigValue(config, "Logging.MaxLogSizeMB")) {
        int maxSize = StringToInt(*value);
        if (maxSize < 1 || maxSize > 1000) {
            result.errors.push_back("Logging.MaxLogSizeMB must be between 1 and 1000");
            result.isValid = false;
        }
    }
    
    // Packet logging validation
    if (auto value = GetConfigValue(config, "Logging.PacketLogging")) {
        if (!ValidateBooleanString(*value)) {
            result.errors.push_back("Logging.PacketLogging must be true or false");
            result.isValid = false;
        }
    }
    
    Logger::Debug("Logging section validation complete");
}

void ConfigValidator::ValidatePerformanceSection(const std::map<std::string, std::string>& config, ValidationResult& result) {
    Logger::Debug("Validating Performance section...");
    
    // Thread pool size validation
    if (auto value = GetConfigValue(config, "Performance.ThreadPoolSize")) {
        int threadCount = StringToInt(*value);
        int maxThreads = std::thread::hardware_concurrency();
        if (threadCount < 1 || threadCount > maxThreads * 2) {
            result.errors.push_back("Performance.ThreadPoolSize must be between 1 and " + std::to_string(maxThreads * 2));
            result.isValid = false;
        }
        if (threadCount > maxThreads) {
            result.warnings.push_back("Performance.ThreadPoolSize exceeds CPU cores - may reduce performance");
        }
    }
    
    // Memory limits validation
    if (auto value = GetConfigValue(config, "Performance.MaxMemoryMB")) {
        int maxMemory = StringToInt(*value);
        if (maxMemory < 512 || maxMemory > 32768) {
            result.errors.push_back("Performance.MaxMemoryMB must be between 512 and 32768");
            result.isValid = false;
        }
    }
    
    // Optimization settings
    if (auto value = GetConfigValue(config, "Performance.EnableOptimizations")) {
        if (!ValidateBooleanString(*value)) {
            result.errors.push_back("Performance.EnableOptimizations must be true or false");
            result.isValid = false;
        }
    }
    
    Logger::Debug("Performance section validation complete");
}

void ConfigValidator::ValidateCrossSectionDependencies(const std::map<std::string, std::string>& config, ValidationResult& result) {
    Logger::Debug("Validating cross-section dependencies...");
    
    // EAC and Security dependencies
    auto eacMode = GetConfigValue(config, "EAC.Mode");
    auto secureMode = GetConfigValue(config, "Security.SecureMode");
    
    if (secureMode && StringToBool(*secureMode)) {
        if (eacMode && *eacMode != "DISABLED") {
            result.errors.push_back("Security.SecureMode requires EAC.Mode to be DISABLED");
            result.isValid = false;
        }
    }
    
    // Anti-cheat and EAC dependencies
    auto customAC = GetConfigValue(config, "AntiCheat.EnableCustomAntiCheat");
    if (eacMode && *eacMode == "DISABLED") {
        if (!customAC || !StringToBool(*customAC)) {
            result.warnings.push_back("EAC is disabled but custom anti-cheat is not enabled - server may be vulnerable");
        }
    }
    
    // Performance and server size dependencies
    auto maxPlayers = GetConfigValue(config, "Server.MaxPlayers");
    auto threadPoolSize = GetConfigValue(config, "Performance.ThreadPoolSize");
    if (maxPlayers && threadPoolSize) {
        int players = StringToInt(*maxPlayers);
        int threads = StringToInt(*threadPoolSize);
        if (players > 64 && threads < 4) {
            result.warnings.push_back("High player count with low thread pool size may cause performance issues");
        }
    }
    
    // Network and game dependencies
    auto packetSize = GetConfigValue(config, "Network.MaxPacketSize");
    auto tickRate = GetConfigValue(config, "Server.TickRate");
    if (packetSize && tickRate) {
        int size = StringToInt(*packetSize);
        int rate = StringToInt(*tickRate);
        if (size > 1400 && rate > 60) {
            result.warnings.push_back("Large packets with high tick rate may cause network congestion");
        }
    }
    
    Logger::Debug("Cross-section dependencies validation complete");
}

void ConfigValidator::ValidateSecurityRequirements(const std::map<std::string, std::string>& config, ValidationResult& result) {
    Logger::Debug("Validating security requirements...");
    
    // Check for insecure default passwords
    if (auto value = GetConfigValue(config, "Security.AdminPassword")) {
        std::vector<std::string> insecurePasswords = {"admin", "password", "123456", "changeme", ""};
        if (std::find(insecurePasswords.begin(), insecurePasswords.end(), *value) != insecurePasswords.end()) {
            result.errors.push_back("Security.AdminPassword is using an insecure default password");
            result.isValid = false;
        }
    }
    
    // Validate secure communication settings
    auto encryption = GetConfigValue(config, "Security.EnableEncryption");
    if (!encryption || !StringToBool(*encryption)) {
        result.warnings.push_back("Security.EnableEncryption is disabled - communications may not be secure");
    }
    
    // Check for debug settings in production
    auto logLevel = GetConfigValue(config, "Logging.LogLevel");
    if (logLevel && *logLevel == "DEBUG") {
        result.warnings.push_back("Logging.LogLevel is set to DEBUG - may expose sensitive information");
    }
    
    Logger::Debug("Security requirements validation complete");
}

void ConfigValidator::CheckDeprecatedSettings(const std::map<std::string, std::string>& config, ValidationResult& result) {
    Logger::Debug("Checking for deprecated settings...");
    
    // List of deprecated configuration keys
    std::map<std::string, std::string> deprecatedKeys = {
        {"Server.LegacyMode", "Use Server.CompatibilityMode instead"},
        {"Network.OldProtocol", "Old protocol support has been removed"},
        {"EAC.EnableEAC", "Use EAC.Mode instead"},
        {"Security.WeakEncryption", "Weak encryption is no longer supported"},
        {"Game.ClassicMode", "Classic mode has been integrated into standard mode"}
    };
    
    for (const auto& [key, message] : deprecatedKeys) {
        if (config.find(key) != config.end()) {
            result.warnings.push_back("Deprecated setting '" + key + "': " + message);
        }
    }
    
    Logger::Debug("Deprecated settings check complete");
}

void ConfigValidator::ValidateSecureModeRequirements(const std::map<std::string, std::string>& config, ValidationResult& result) {
    Logger::Debug("Validating secure mode requirements...");
    
    // In secure mode, certain settings must be configured securely
    std::vector<std::pair<std::string, std::string>> requiredSecureSettings = {
        {"EAC.Mode", "DISABLED"},
        {"Security.PrivacyMode", "true"},
        {"Security.AllowTelemetry", "false"},
        {"AntiCheat.EnableCustomAntiCheat", "true"},
        {"AntiCheat.StrictValidation", "true"}
    };
    
    for (const auto& [key, expectedValue] : requiredSecureSettings) {
        auto value = GetConfigValue(config, key);
        if (!value || *value != expectedValue) {
            result.errors.push_back("Secure mode requires " + key + " to be " + expectedValue);
            result.isValid = false;
        }
    }
    
    Logger::Debug("Secure mode requirements validation complete");
}

void ConfigValidator::ValidateEACConsistency(const std::map<std::string, std::string>& config, ValidationResult& result) {
    Logger::Debug("Validating EAC configuration consistency...");
    
    auto eacMode = GetConfigValue(config, "EAC.Mode");
    if (!eacMode) return;
    
    if (*eacMode == "DISABLED") {
        // When EAC is disabled, related settings should be consistent
        auto enableProxy = GetConfigValue(config, "EAC.EnableEACProxy");
        auto forwardToEpic = GetConfigValue(config, "EAC.ForwardToEpic");
        
        if (enableProxy && StringToBool(*enableProxy)) {
            result.warnings.push_back("EAC.EnableEACProxy is true but EAC.Mode is DISABLED");
        }
        
        if (forwardToEpic && StringToBool(*forwardToEpic)) {
            result.warnings.push_back("EAC.ForwardToEpic is true but EAC.Mode is DISABLED");
        }
    } else if (*eacMode == "PASSIVE_LOGGING") {
        // In passive logging mode, proxy should be enabled
        auto enableProxy = GetConfigValue(config, "EAC.EnableEACProxy");
        if (!enableProxy || !StringToBool(*enableProxy)) {
            result.warnings.push_back("EAC.Mode is PASSIVE_LOGGING but EAC.EnableEACProxy is not enabled");
        }
    }
    
    Logger::Debug("EAC consistency validation complete");
}

// Utility methods
std::optional<std::string> ConfigValidator::GetConfigValue(const std::map<std::string, std::string>& config, const std::string& key) {
    auto it = config.find(key);
    if (it != config.end()) {
        return it->second;
    }
    return std::nullopt;
}

void ConfigValidator::ValidateRequired(const std::map<std::string, std::string>& config, const std::string& key, ValidationResult& result) {
    if (config.find(key) == config.end()) {
        result.errors.push_back("Required configuration key missing: " + key);
        result.isValid = false;
    }
}

void ConfigValidator::ValidatePortRange(const std::map<std::string, std::string>& config, const std::string& key, int minPort, int maxPort, ValidationResult& result) {
    if (auto value = GetConfigValue(config, key)) {
        int port = StringToInt(*value);
        if (port < minPort || port > maxPort) {
            result.errors.push_back(key + " must be between " + std::to_string(minPort) + " and " + std::to_string(maxPort));
            result.isValid = false;
        }
    }
}

bool ConfigValidator::ValidateBooleanString(const std::string& value) {
    std::string lower = StringUtils::ToLower(value);
    return lower == "true" || lower == "false" || lower == "1" || lower == "0" || lower == "yes" || lower == "no";
}

bool ConfigValidator::ValidateIPAddress(const std::string& ip) {
    std::regex ipRegex(R"(^((25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.){3}(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)$)");
    return std::regex_match(ip, ipRegex) || ip == "0.0.0.0" || ip == "localhost";
}

bool ConfigValidator::ValidateFilePath(const std::string& path) {
    try {
        std::filesystem::path p(path);
        return !path.empty() && p.is_relative() || p.is_absolute();
    } catch (const std::exception&) {
        return false;
    }
}

bool ConfigValidator::ValidateMapName(const std::string& mapName) {
    // Basic map name validation
    return !mapName.empty() && mapName.length() <= 64;
}

bool ConfigValidator::ValidateRS2VMapName(const std::string& mapName) {
    // RS2V specific map validation
    std::vector<std::string> validMaps = {
        "VNTE-CuChi", "VNTE-AnLao", "VNTE-HueCity", "VNTE-Firebase",
        "VNTE-Song_Be", "VNTE-Hill937", "VNTE-Resort", "VNTE-OperationFlaming"
    };
    
    return std::find(validMaps.begin(), validMaps.end(), mapName) != validMaps.end();
}

bool ConfigValidator::ValidateGameMode(const std::string& gameMode) {
    std::vector<std::string> validModes = {
        "Territories", "Supremacy", "Skirmish", "Campaign"
    };
    
    return std::find(validModes.begin(), validModes.end(), gameMode) != validModes.end();
}

bool ConfigValidator::ValidatePasswordStrength(const std::string& password) {
    if (password.length() < 8) return false;
    
    bool hasLower = false, hasUpper = false, hasDigit = false, hasSpecial = false;
    
    for (char c : password) {
        if (std::islower(c)) hasLower = true;
        else if (std::isupper(c)) hasUpper = true;
        else if (std::isdigit(c)) hasDigit = true;
        else hasSpecial = true;
    }
    
    return hasLower && hasUpper && hasDigit && hasSpecial;
}

// Type conversion utilities
int ConfigValidator::StringToInt(const std::string& str) {
    try {
        return std::stoi(str);
    } catch (const std::exception&) {
        return 0;
    }
}

float ConfigValidator::StringToFloat(const std::string& str) {
    try {
        return std::stof(str);
    } catch (const std::exception&) {
        return 0.0f;
    }
}

bool ConfigValidator::StringToBool(const std::string& str) {
    std::string lower = StringUtils::ToLower(str);
    return lower == "true" || lower == "1" || lower == "yes" || lower == "on";
}

void ConfigValidator::ValidateStringValue(const std::string& key, const std::string& value, const ValidationRule& rule, ValidationResult& result) {
    if (value.length() < rule.minLength) {
        result.errors.push_back(key + " must be at least " + std::to_string(rule.minLength) + " characters");
        result.isValid = false;
    }
    
    if (value.length() > rule.maxLength) {
        result.errors.push_back(key + " must be at most " + std::to_string(rule.maxLength) + " characters");
        result.isValid = false;
    }
    
    if (!rule.regexPattern.empty()) {
        std::regex pattern(rule.regexPattern);
        if (!std::regex_match(value, pattern)) {
            result.errors.push_back(key + " does not match required pattern");
            result.isValid = false;
        }
    }
}

void ConfigValidator::ValidateIntegerValue(const std::string& key, const std::string& value, const ValidationRule& rule, ValidationResult& result) {
    try {
        int intValue = std::stoi(value);
        if (intValue < rule.minValue) {
            result.errors.push_back(key + " must be at least " + std::to_string(static_cast<int>(rule.minValue)));
            result.isValid = false;
        }
        if (intValue > rule.maxValue) {
            result.errors.push_back(key + " must be at most " + std::to_string(static_cast<int>(rule.maxValue)));
            result.isValid = false;
        }
    } catch (const std::exception&) {
        result.errors.push_back(key + " is not a valid integer");
        result.isValid = false;
    }
}

void ConfigValidator::ValidateFloatValue(const std::string& key, const std::string& value, const ValidationRule& rule, ValidationResult& result) {
    try {
        float floatValue = std::stof(value);
        if (floatValue < rule.minValue) {
            result.errors.push_back(key + " must be at least " + std::to_string(rule.minValue));
            result.isValid = false;
        }
        if (floatValue > rule.maxValue) {
            result.errors.push_back(key + " must be at most " + std::to_string(rule.maxValue));
            result.isValid = false;
        }
    } catch (const std::exception&) {
        result.errors.push_back(key + " is not a valid number");
        result.isValid = false;
    }
}

void ConfigValidator::ValidateBooleanValue(const std::string& key, const std::string& value, const ValidationRule& rule, ValidationResult& result) {
    if (!ValidateBooleanString(value)) {
        result.errors.push_back(key + " must be true or false");
        result.isValid = false;
    }
}

void ConfigValidator::ValidatePathValue(const std::string& key, const std::string& value, const ValidationRule& rule, ValidationResult& result) {
    if (!ValidateFilePath(value)) {
        result.errors.push_back(key + " is not a valid file path");
        result.isValid = false;
    }
}

void ConfigValidator::ValidateIPAddressValue(const std::string& key, const std::string& value, const ValidationRule& rule, ValidationResult& result) {
    if (!ValidateIPAddress(value)) {
        result.errors.push_back(key + " is not a valid IP address");
        result.isValid = false;
    }
}

void ConfigValidator::ValidatePortValue(const std::string& key, const std::string& value, const ValidationRule& rule, ValidationResult& result) {
    ValidateIntegerValue(key, value, rule, result);
    
    try {
        int port = std::stoi(value);
        if (port < 1024 || port > 65535) {
            result.errors.push_back(key + " must be between 1024 and 65535");
            result.isValid = false;
        }
    } catch (const std::exception&) {
        // Already handled in ValidateIntegerValue
    }
}

void ConfigValidator::ValidateEnumValue(const std::string& key, const std::string& value, const ValidationRule& rule, ValidationResult& result) {
    if (std::find(rule.allowedValues.begin(), rule.allowedValues.end(), value) == rule.allowedValues.end()) {
        std::string allowedStr;
        for (size_t i = 0; i < rule.allowedValues.size(); ++i) {
            if (i > 0) allowedStr += ", ";
            allowedStr += rule.allowedValues[i];
        }
        result.errors.push_back(key + " must be one of: " + allowedStr);
        result.isValid = false;
    }
}

void ConfigValidator::InitializeValidationRules() {
    Logger::Debug("Initializing validation rules...");
    
    // Define validation rules for each configuration key
    // This is where all the specific validation rules are stored
    
    // Server section rules
    m_validationRules["Server.ServerName"] = {ConfigDataType::STRING, 1, 64, 0, 0, {}, ""};
    m_validationRules["Server.MaxPlayers"] = {ConfigDataType::INTEGER, 0, 0, 1, 128, {}, ""};
    m_validationRules["Server.GamePort"] = {ConfigDataType::PORT, 0, 0, 1024, 65535, {}, ""};
    m_validationRules["Server.QueryPort"] = {ConfigDataType::PORT, 0, 0, 1024, 65535, {}, ""};
    m_validationRules["Server.TickRate"] = {ConfigDataType::INTEGER, 0, 0, 10, 128, {}, ""};
    
    // Network section rules
    m_validationRules["Network.MaxPacketSize"] = {ConfigDataType::INTEGER, 0, 0, 64, 65536, {}, ""};
    m_validationRules["Network.TimeoutSeconds"] = {ConfigDataType::INTEGER, 0, 0, 5, 300, {}, ""};
    
    // EAC section rules
    m_validationRules["EAC.Mode"] = {ConfigDataType::ENUM, 0, 0, 0, 0, {"DISABLED", "PASSIVE_LOGGING", "BLOCK_MODE"}, ""};
    m_validationRules["EAC.ProxyPort"] = {ConfigDataType::PORT, 0, 0, 1024, 65535, {}, ""};
    
    // Security section rules
    m_validationRules["Security.AdminPassword"] = {ConfigDataType::STRING, 8, 128, 0, 0, {}, ""};
    
    // Anti-cheat section rules
    m_validationRules["AntiCheat.MaxSpeed"] = {ConfigDataType::FLOAT, 0, 0, 1.0f, 20.0f, {}, ""};
    m_validationRules["AntiCheat.MaxHeadshotRatio"] = {ConfigDataType::FLOAT, 0, 0, 0.0f, 1.0f, {}, ""};
    
    Logger::Debug("Validation rules initialized: %zu rules", m_validationRules.size());
}

void ConfigValidator::InitializeCustomValidators() {
    Logger::Debug("Initializing custom validators...");
    // Custom validation logic can be added here
    Logger::Debug("Custom validators initialized");
}

bool ConfigValidator::LoadValidationSchemas() {
    Logger::Debug("Loading validation schemas...");
    // Load validation schemas from files if needed
    Logger::Debug("Validation schemas loaded");
    return true;
}

bool ConfigValidator::LoadConfigurationFromFile(const std::string& configFile, std::map<std::string, std::string>& config) {
    std::ifstream file(configFile);
    if (!file.is_open()) {
        return false;
    }
    
    std::string line;
    std::string currentSection;
    
    while (std::getline(file, line)) {
        // Remove comments and trim
        size_t commentPos = line.find('#');
        if (commentPos != std::string::npos) {
            line = line.substr(0, commentPos);
        }
        
        line = StringUtils::Trim(line);
        if (line.empty()) continue;
        
        // Section header
        if (line.front() == '[' && line.back() == ']') {
            currentSection = line.substr(1, line.length() - 2);
            continue;
        }
        
        // Key-value pair
        size_t equalPos = line.find('=');
        if (equalPos != std::string::npos) {
            std::string key = StringUtils::Trim(line.substr(0, equalPos));
            std::string value = StringUtils::Trim(line.substr(equalPos + 1));
            
            std::string fullKey = currentSection.empty() ? key : currentSection + "." + key;
            config[fullKey] = value;
        }
    }
    
    return true;
}

void ConfigValidator::LogValidationResults(const ValidationResult& result) {
    if (result.isValid) {
        Logger::Info("Configuration validation PASSED");
        if (!result.warnings.empty()) {
            Logger::Info("Validation warnings:");
            for (const auto& warning : result.warnings) {
                Logger::Warn("  %s", warning.c_str());
            }
        }
    } else {
        Logger::Error("Configuration validation FAILED");
        Logger::Error("Validation errors:");
        for (const auto& error : result.errors) {
            Logger::Error("  %s", error.c_str());
        }
        
        if (!result.warnings.empty()) {
            Logger::Warn("Validation warnings:");
            for (const auto& warning : result.warnings) {
                Logger::Warn("  %s", warning.c_str());
            }
        }
    }
}