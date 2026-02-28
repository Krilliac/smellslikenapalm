// src/Config/SecurityConfig.cpp

#include "Config/SecurityConfig.h"
#include "Utils/Logger.h"

SecurityConfig::SecurityConfig(const ServerConfig& cfg)
  : m_steamAuthEnabled(cfg.IsSteamAuthEnabled())
  , m_fallbackCustomAuth(cfg.IsFallbackCustomAuth())
  , m_customAuthTokensFile(cfg.GetCustomAuthTokensFile())
  , m_banManagerEnabled(cfg.IsBanManagerEnabled())
  , m_banListFile(cfg.GetBanListFile())
  , m_antiCheatEnabled(cfg.IsAntiCheatEnabled())
  , m_antiCheatMode(cfg.GetAntiCheatMode())
  , m_eacScannerConfigFile(cfg.GetEacScannerConfigFile())
{
    Logger::Trace("[SecurityConfig::SecurityConfig] Entry - constructing from ServerConfig");
    Logger::Info("[SecurityConfig::SecurityConfig] SecurityConfig initialized: steamAuth=%s, fallbackAuth=%s, banMgr=%s, antiCheat=%s, acMode='%s'",
                 m_steamAuthEnabled ? "true" : "false",
                 m_fallbackCustomAuth ? "true" : "false",
                 m_banManagerEnabled ? "true" : "false",
                 m_antiCheatEnabled ? "true" : "false",
                 m_antiCheatMode.c_str());
    Logger::Trace("[SecurityConfig::SecurityConfig] Exit");
}

bool SecurityConfig::IsSteamAuthEnabled() const {
    Logger::Trace("[SecurityConfig::IsSteamAuthEnabled] Entry");
    Logger::Trace("[SecurityConfig::IsSteamAuthEnabled] Exit - returning %s", m_steamAuthEnabled ? "true" : "false");
    return m_steamAuthEnabled;
}

bool SecurityConfig::IsFallbackCustomAuth() const {
    Logger::Trace("[SecurityConfig::IsFallbackCustomAuth] Entry");
    Logger::Trace("[SecurityConfig::IsFallbackCustomAuth] Exit - returning %s", m_fallbackCustomAuth ? "true" : "false");
    return m_fallbackCustomAuth;
}

const std::string& SecurityConfig::GetCustomAuthTokensFile() const {
    Logger::Trace("[SecurityConfig::GetCustomAuthTokensFile] Entry");
    Logger::Trace("[SecurityConfig::GetCustomAuthTokensFile] Exit - returning '%s'", m_customAuthTokensFile.c_str());
    return m_customAuthTokensFile;
}

bool SecurityConfig::IsBanManagerEnabled() const {
    Logger::Trace("[SecurityConfig::IsBanManagerEnabled] Entry");
    Logger::Trace("[SecurityConfig::IsBanManagerEnabled] Exit - returning %s", m_banManagerEnabled ? "true" : "false");
    return m_banManagerEnabled;
}

const std::string& SecurityConfig::GetBanListFile() const {
    Logger::Trace("[SecurityConfig::GetBanListFile] Entry");
    Logger::Trace("[SecurityConfig::GetBanListFile] Exit - returning '%s'", m_banListFile.c_str());
    return m_banListFile;
}

bool SecurityConfig::IsAntiCheatEnabled() const {
    Logger::Trace("[SecurityConfig::IsAntiCheatEnabled] Entry");
    Logger::Trace("[SecurityConfig::IsAntiCheatEnabled] Exit - returning %s", m_antiCheatEnabled ? "true" : "false");
    return m_antiCheatEnabled;
}

const std::string& SecurityConfig::GetAntiCheatMode() const {
    Logger::Trace("[SecurityConfig::GetAntiCheatMode] Entry");
    Logger::Trace("[SecurityConfig::GetAntiCheatMode] Exit - returning '%s'", m_antiCheatMode.c_str());
    return m_antiCheatMode;
}

const std::string& SecurityConfig::GetEacScannerConfigFile() const {
    Logger::Trace("[SecurityConfig::GetEacScannerConfigFile] Entry");
    Logger::Trace("[SecurityConfig::GetEacScannerConfigFile] Exit - returning '%s'", m_eacScannerConfigFile.c_str());
    return m_eacScannerConfigFile;
}
