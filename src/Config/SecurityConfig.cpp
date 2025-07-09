// src/Config/SecurityConfig.cpp - Complete implementation for RS2V Server security configuration
#include "Config/SecurityConfig.h"
#include "Utils/Logger.h"
#include "Utils/StringUtils.h"
#include "Utils/FileUtils.h"
#include <fstream>
#include <filesystem>

SecurityConfig::SecurityConfig() {
    Logger::Info("SecurityConfig initialized");
}

SecurityConfig::~SecurityConfig() = default;

bool SecurityConfig::Initialize(std::shared_ptr<ConfigManager> configManager) {
    Logger::Info("Initializing SecurityConfig...");
    m_configManager = configManager;
    if (!m_configManager) {
        Logger::Error("ConfigManager is null");
        return false;
    }
    return Load();
}

bool SecurityConfig::Load() {
    Logger::Debug("Loading security configuration...");

    m_secureMode            = m_configManager->GetBool("Security.SecureMode", true);
    m_privacyMode           = m_configManager->GetBool("Security.PrivacyMode", true);
    m_allowTelemetry        = m_configManager->GetBool("Security.AllowTelemetry", false);
    m_enableEncryption      = m_configManager->GetBool("Security.EnableEncryption", true);
    m_adminPassword         = m_configManager->GetString("Security.AdminPassword", "");
    m_maxLoginAttempts      = m_configManager->GetInt("Security.MaxLoginAttempts", 5);
    m_banDurationMinutes    = m_configManager->GetInt("Security.BanDurationMinutes", 60);
    m_useIPWhitelist        = m_configManager->GetBool("Security.UseIPWhitelist", false);
    m_useIPBlacklist        = m_configManager->GetBool("Security.UseIPBlacklist", false);

    m_ipWhitelist           = m_configManager->GetArray("Security.IPWhitelist", {});
    m_ipBlacklist           = m_configManager->GetArray("Security.IPBlacklist", {});

    // Validate admin password strength
    if (m_adminPassword.empty()) {
        Logger::Warn("SecurityConfig: AdminPassword is empty");
    } else if (!ValidatePasswordStrength(m_adminPassword)) {
        Logger::Warn("SecurityConfig: AdminPassword is weak");
    }

    Logger::Info("SecurityConfig loaded: SecureMode=%s, PrivacyMode=%s, Telemetry=%s, Encryption=%s",
        m_secureMode ? "ON" : "OFF",
        m_privacyMode ? "ON" : "OFF",
        m_allowTelemetry ? "ENABLED" : "DISABLED",
        m_enableEncryption ? "ENABLED" : "DISABLED"
    );

    return true;
}

bool SecurityConfig::Save(const std::string& configFile) const {
    Logger::Info("Saving security configuration to %s", configFile.c_str());
    std::filesystem::path path(configFile);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream file(configFile, std::ios::trunc);
    if (!file.is_open()) {
        Logger::Error("SecurityConfig: Failed to open file %s for writing", configFile.c_str());
        return false;
    }

    file << "# RS2V Security Configuration\n\n";
    file << "[Security]\n";
    file << "SecureMode="         << (m_secureMode         ? "true" : "false") << "\n";
    file << "PrivacyMode="        << (m_privacyMode        ? "true" : "false") << "\n";
    file << "AllowTelemetry="     << (m_allowTelemetry     ? "true" : "false") << "\n";
    file << "EnableEncryption="   << (m_enableEncryption   ? "true" : "false") << "\n";
    file << "AdminPassword="      << m_adminPassword      << "\n";
    file << "MaxLoginAttempts="   << m_maxLoginAttempts   << "\n";
    file << "BanDurationMinutes=" << m_banDurationMinutes << "\n";
    file << "UseIPWhitelist="     << (m_useIPWhitelist     ? "true" : "false") << "\n";
    file << "UseIPBlacklist="     << (m_useIPBlacklist     ? "true" : "false") << "\n";

    if (!m_ipWhitelist.empty()) {
        file << "IPWhitelist=" << StringUtils::Join(m_ipWhitelist, ',') << "\n";
    }
    if (!m_ipBlacklist.empty()) {
        file << "IPBlacklist=" << StringUtils::Join(m_ipBlacklist, ',') << "\n";
    }

    file.close();
    Logger::Info("Security configuration saved");
    return true;
}

bool SecurityConfig::Reload() {
    Logger::Info("Reloading security configuration...");
    return Load();
}

bool SecurityConfig::ValidatePasswordStrength(const std::string& pwd) const {
    if (pwd.length() < 8) return false;
    bool hasLower=false, hasUpper=false, hasDigit=false, hasSpecial=false;
    for (char c : pwd) {
        if (std::islower(c)) hasLower=true;
        else if (std::isupper(c)) hasUpper=true;
        else if (std::isdigit(c)) hasDigit=true;
        else hasSpecial=true;
    }
    return hasLower && hasUpper && hasDigit && hasSpecial;
}

// Getters
bool SecurityConfig::IsSecureMode()     const { return m_secureMode; }
bool SecurityConfig::IsPrivacyMode()    const { return m_privacyMode; }
bool SecurityConfig::AllowTelemetry()   const { return m_allowTelemetry; }
bool SecurityConfig::EnableEncryption() const { return m_enableEncryption; }
const std::string& SecurityConfig::GetAdminPassword() const { return m_adminPassword; }
int  SecurityConfig::GetMaxLoginAttempts()   const { return m_maxLoginAttempts; }
int  SecurityConfig::GetBanDurationMinutes() const { return m_banDurationMinutes; }
bool SecurityConfig::UseIPWhitelist()        const { return m_useIPWhitelist; }
bool SecurityConfig::UseIPBlacklist()        const { return m_useIPBlacklist; }
const std::vector<std::string>& SecurityConfig::GetIPWhitelist() const { return m_ipWhitelist; }
const std::vector<std::string>& SecurityConfig::GetIPBlacklist() const { return m_ipBlacklist; }

void SecurityConfig::LogSummary() const {
    Logger::Info("=== SecurityConfig Summary ===");
    Logger::Info("SecureMode:         %s", m_secureMode ? "ON" : "OFF");
    Logger::Info("PrivacyMode:        %s", m_privacyMode ? "ON" : "OFF");
    Logger::Info("AllowTelemetry:     %s", m_allowTelemetry ? "YES" : "NO");
    Logger::Info("EnableEncryption:   %s", m_enableEncryption ? "YES" : "NO");
    Logger::Info("MaxLoginAttempts:   %d", m_maxLoginAttempts);
    Logger::Info("BanDurationMinutes: %d", m_banDurationMinutes);
    Logger::Info("UseIPWhitelist:     %s", m_useIPWhitelist ? "YES" : "NO");
    Logger::Info("UseIPBlacklist:     %s", m_useIPBlacklist ? "YES" : "NO");
    if (m_useIPWhitelist) {
        Logger::Info("IPWhitelist:        %s", StringUtils::Join(m_ipWhitelist, ',').c_str());
    }
    if (m_useIPBlacklist) {
        Logger::Info("IPBlacklist:        %s", StringUtils::Join(m_ipBlacklist, ',').c_str());
    }
    Logger::Info("================================");
}