#pragma once

#include <string>
#include <vector>
#include "Config/ServerConfig.h"

class DebugConfig {
public:
    explicit DebugConfig(const ServerConfig& cfg);

    bool  IsDebugEnabled() const;
    bool  IsLogToFile() const;
    const std::string& GetDebugLogPath() const;
    int   GetLogMaxSizeMb() const;
    int   GetLogMaxFiles() const;
    int   GetVerbosityLevel() const;
    bool  IsConsoleDebugOutput() const;

    // Module overrides
    bool  IsModuleEnabled(const std::string& module) const;
    int   GetModuleVerbosity(const std::string& module) const;

    // Dynamic toggles
    bool  GetToggle(const std::string& toggleName) const;
    std::vector<std::string> ListToggles() const;

private:
    ServerConfig m_cfg;

    // Cache prefix lookups
    std::vector<std::string> m_toggleKeys;
};