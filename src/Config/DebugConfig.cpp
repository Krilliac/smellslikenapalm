#include "Config/DebugConfig.h"
#include "Utils/StringUtils.h"

DebugConfig::DebugConfig(const ServerConfig& cfg)
  : m_cfg(cfg)
{
    // Gather all toggle_<name> keys
    for (auto& k : cfg.GetManager()->GetSectionKeys("toggles")) {
        m_toggleKeys.push_back(k);
    }
}

bool DebugConfig::IsDebugEnabled() const {
    return m_cfg.GetManager()->GetBool("Global.debug_enabled", false);
}

bool DebugConfig::IsLogToFile() const {
    return m_cfg.GetManager()->GetBool("Global.log_to_file", true);
}

const std::string& DebugConfig::GetDebugLogPath() const {
    return m_cfg.GetManager()->GetString("Global.debug_log_path", "logs/debug.log");
}

int DebugConfig::GetLogMaxSizeMb() const {
    return m_cfg.GetManager()->GetInt("Global.log_max_size_mb", 10);
}

int DebugConfig::GetLogMaxFiles() const {
    return m_cfg.GetManager()->GetInt("Global.log_max_files", 5);
}

int DebugConfig::GetVerbosityLevel() const {
    return m_cfg.GetManager()->GetInt("Global.verbosity_level", 3);
}

bool DebugConfig::IsConsoleDebugOutput() const {
    return m_cfg.GetManager()->GetBool("Global.console_debug_output", true);
}

bool DebugConfig::IsModuleEnabled(const std::string& module) const {
    std::string key = module + ".enabled";
    return m_cfg.GetManager()->GetBool(key, false);
}

int DebugConfig::GetModuleVerbosity(const std::string& module) const {
    std::string key = module + ".verbosity_level";
    return m_cfg.GetManager()->GetInt(key, GetVerbosityLevel());
}

bool DebugConfig::GetToggle(const std::string& toggleName) const {
    std::string key = "toggles.toggle_" + toggleName;
    return m_cfg.GetManager()->GetBool(key, false);
}

std::vector<std::string> DebugConfig::ListToggles() const {
    return m_toggleKeys;
}