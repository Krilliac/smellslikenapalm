// src/Config/SecurityConfig.cpp

#include "Config/SecurityConfig.h"

SecurityConfig::SecurityConfig(const ServerConfig& cfg)
  : m_steamAuthEnabled(cfg.IsSteamAuthEnabled())
  , m_fallbackCustomAuth(cfg.IsFallbackCustomAuth())
  , m_customAuthTokensFile(cfg.GetCustomAuthTokensFile())
  , m_banManagerEnabled(cfg.IsBanManagerEnabled())
  , m_banListFile(cfg.GetBanListFile())
  , m_antiCheatEnabled(cfg.IsAntiCheatEnabled())
  , m_antiCheatMode(cfg.GetAntiCheatMode())
  , m_eacScannerConfigFile(cfg.GetEacScannerConfigFile())
{}

bool SecurityConfig::IsSteamAuthEnabled() const {
    return m_steamAuthEnabled;
}

bool SecurityConfig::IsFallbackCustomAuth() const {
    return m_fallbackCustomAuth;
}

const std::string& SecurityConfig::GetCustomAuthTokensFile() const {
    return m_customAuthTokensFile;
}

bool SecurityConfig::IsBanManagerEnabled() const {
    return m_banManagerEnabled;
}

const std::string& SecurityConfig::GetBanListFile() const {
    return m_banListFile;
}

bool SecurityConfig::IsAntiCheatEnabled() const {
    return m_antiCheatEnabled;
}

const std::string& SecurityConfig::GetAntiCheatMode() const {
    return m_antiCheatMode;
}

const std::string& SecurityConfig::GetEacScannerConfigFile() const {
    return m_eacScannerConfigFile;
}