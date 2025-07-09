// src/Config/SecurityConfig.h

#pragma once

#include <string>
#include "Config/ServerConfig.h"

class SecurityConfig {
public:
    explicit SecurityConfig(const ServerConfig& cfg);

    bool        IsSteamAuthEnabled() const;
    bool        IsFallbackCustomAuth() const;
    const std::string& GetCustomAuthTokensFile() const;
    bool        IsBanManagerEnabled() const;
    const std::string& GetBanListFile() const;
    bool        IsAntiCheatEnabled() const;
    const std::string& GetAntiCheatMode() const;
    const std::string& GetEacScannerConfigFile() const;

private:
    bool        m_steamAuthEnabled;
    bool        m_fallbackCustomAuth;
    std::string m_customAuthTokensFile;
    bool        m_banManagerEnabled;
    std::string m_banListFile;
    bool        m_antiCheatEnabled;
    std::string m_antiCheatMode;
    std::string m_eacScannerConfigFile;
};