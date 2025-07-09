// src/Config/ConfigValidator.h

#pragma once

#include <string>
#include <map>
#include <vector>
#include <optional>

struct ValidationResult {
    bool isValid;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

class ConfigValidator {
public:
    ConfigValidator();
    ~ConfigValidator();

    // Initialize internal validation rules
    bool Initialize();

    // Validate an in-memory config map
    ValidationResult ValidateConfiguration(const std::map<std::string, std::string>& config);

    // Load and validate a config file on disk
    bool ValidateConfigurationFile(const std::string& configFile);

private:
    // Section validators
    void ValidateGeneralSection(const std::map<std::string, std::string>& cfg, ValidationResult& r);
    void ValidateNetworkSection(const std::map<std::string, std::string>& cfg, ValidationResult& r);
    void ValidateSecuritySection(const std::map<std::string, std::string>& cfg, ValidationResult& r);
    void ValidateEACSection(const std::map<std::string, std::string>& cfg, ValidationResult& r);
    void ValidateAntiCheatSection(const std::map<std::string, std::string>& cfg, ValidationResult& r);
    void ValidateLoggingSection(const std::map<std::string, std::string>& cfg, ValidationResult& r);
    void ValidatePerformanceSection(const std::map<std::string, std::string>& cfg, ValidationResult& r);
    void ValidateGamePlaySection(const std::map<std::string, std::string>& cfg, ValidationResult& r);
    void ValidateCrossSectionDependencies(const std::map<std::string, std::string>& cfg, ValidationResult& r);
    void CheckDeprecatedSettings(const std::map<std::string, std::string>& cfg, ValidationResult& r);

    // Helpers for parsing and validation
    std::optional<std::string> GetConfig(const std::map<std::string, std::string>& cfg, const std::string& key);
    bool ValidateBooleanString(const std::string& v);
    bool LoadConfigurationFromFile(const std::string& file, std::map<std::string, std::string>& cfg);
    void LogValidationResults(const ValidationResult& r);

    // Individual validation utilities
    void ValidateRequired(const std::map<std::string, std::string>& cfg, const std::string& key, ValidationResult& r);
    void ValidatePortRange(const std::map<std::string, std::string>& cfg, const std::string& key, int lo, int hi, ValidationResult& r);
    int  StringToInt(const std::string& s);
    float StringToFloat(const std::string& s);
};