#include "Config/DebugConfig.h"
#include "Utils/StringUtils.h"
#include "Utils/Logger.h"

DebugConfig::DebugConfig(const ServerConfig& cfg)
  : m_cfg(cfg)
{
    Logger::Trace("[DebugConfig::DebugConfig] Entry - constructing from ServerConfig");
    // Gather all toggle_<name> keys
    for (auto& k : cfg.GetManager()->GetSectionKeys("toggles")) {
        m_toggleKeys.push_back(k);
        Logger::Trace("[DebugConfig::DebugConfig] Registered toggle key: '%s'", k.c_str());
    }
    Logger::Info("[DebugConfig::DebugConfig] DebugConfig initialized with %zu toggle keys", m_toggleKeys.size());
    Logger::Trace("[DebugConfig::DebugConfig] Exit");
}

bool DebugConfig::IsDebugEnabled() const {
    Logger::Trace("[DebugConfig::IsDebugEnabled] Entry");
    bool result = m_cfg.GetManager()->GetBool("Global.debug_enabled", false);
    Logger::Trace("[DebugConfig::IsDebugEnabled] Exit - returning %s", result ? "true" : "false");
    return result;
}

bool DebugConfig::IsLogToFile() const {
    Logger::Trace("[DebugConfig::IsLogToFile] Entry");
    bool result = m_cfg.GetManager()->GetBool("Global.log_to_file", true);
    Logger::Trace("[DebugConfig::IsLogToFile] Exit - returning %s", result ? "true" : "false");
    return result;
}

const std::string& DebugConfig::GetDebugLogPath() const {
    Logger::Trace("[DebugConfig::GetDebugLogPath] Entry");
    const auto& result = m_cfg.GetManager()->GetString("Global.debug_log_path", "logs/debug.log");
    Logger::Trace("[DebugConfig::GetDebugLogPath] Exit - returning '%s'", result.c_str());
    return result;
}

int DebugConfig::GetLogMaxSizeMb() const {
    Logger::Trace("[DebugConfig::GetLogMaxSizeMb] Entry");
    int result = m_cfg.GetManager()->GetInt("Global.log_max_size_mb", 10);
    Logger::Trace("[DebugConfig::GetLogMaxSizeMb] Exit - returning %d", result);
    return result;
}

int DebugConfig::GetLogMaxFiles() const {
    Logger::Trace("[DebugConfig::GetLogMaxFiles] Entry");
    int result = m_cfg.GetManager()->GetInt("Global.log_max_files", 5);
    Logger::Trace("[DebugConfig::GetLogMaxFiles] Exit - returning %d", result);
    return result;
}

int DebugConfig::GetVerbosityLevel() const {
    Logger::Trace("[DebugConfig::GetVerbosityLevel] Entry");
    int result = m_cfg.GetManager()->GetInt("Global.verbosity_level", 3);
    Logger::Trace("[DebugConfig::GetVerbosityLevel] Exit - returning %d", result);
    return result;
}

bool DebugConfig::IsConsoleDebugOutput() const {
    Logger::Trace("[DebugConfig::IsConsoleDebugOutput] Entry");
    bool result = m_cfg.GetManager()->GetBool("Global.console_debug_output", true);
    Logger::Trace("[DebugConfig::IsConsoleDebugOutput] Exit - returning %s", result ? "true" : "false");
    return result;
}

bool DebugConfig::IsModuleEnabled(const std::string& module) const {
    Logger::Trace("[DebugConfig::IsModuleEnabled] Entry - module='%s'", module.c_str());
    std::string key = module + ".enabled";
    Logger::Debug("[DebugConfig::IsModuleEnabled] Looking up key='%s'", key.c_str());
    bool result = m_cfg.GetManager()->GetBool(key, false);
    Logger::Trace("[DebugConfig::IsModuleEnabled] Exit - module='%s' returning %s", module.c_str(), result ? "true" : "false");
    return result;
}

int DebugConfig::GetModuleVerbosity(const std::string& module) const {
    Logger::Trace("[DebugConfig::GetModuleVerbosity] Entry - module='%s'", module.c_str());
    std::string key = module + ".verbosity_level";
    Logger::Debug("[DebugConfig::GetModuleVerbosity] Looking up key='%s', fallback to global verbosity", key.c_str());
    int result = m_cfg.GetManager()->GetInt(key, GetVerbosityLevel());
    Logger::Trace("[DebugConfig::GetModuleVerbosity] Exit - module='%s' returning %d", module.c_str(), result);
    return result;
}

bool DebugConfig::GetToggle(const std::string& toggleName) const {
    Logger::Trace("[DebugConfig::GetToggle] Entry - toggleName='%s'", toggleName.c_str());
    std::string key = "toggles.toggle_" + toggleName;
    Logger::Debug("[DebugConfig::GetToggle] Looking up key='%s'", key.c_str());
    bool result = m_cfg.GetManager()->GetBool(key, false);
    Logger::Trace("[DebugConfig::GetToggle] Exit - toggle='%s' returning %s", toggleName.c_str(), result ? "true" : "false");
    return result;
}

std::vector<std::string> DebugConfig::ListToggles() const {
    Logger::Trace("[DebugConfig::ListToggles] Entry");
    Logger::Debug("[DebugConfig::ListToggles] Returning %zu toggle keys", m_toggleKeys.size());
    Logger::Trace("[DebugConfig::ListToggles] Exit - returning %zu toggles", m_toggleKeys.size());
    return m_toggleKeys;
}
