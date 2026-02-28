#include "Config/ServerConfig.h"
#include "Utils/StringUtils.h"
#include "Utils/Logger.h"

ServerConfig::ServerConfig(const std::shared_ptr<ConfigManager>& mgr)
  : m_mgr(mgr)
{
    Logger::Trace("[ServerConfig::ServerConfig] Entry - constructing with ConfigManager ptr=%p", (void*)mgr.get());
    Logger::Info("[ServerConfig::ServerConfig] ServerConfig initialized");
    Logger::Trace("[ServerConfig::ServerConfig] Exit");
}

// General
std::string ServerConfig::GetServerName() const {
    Logger::Trace("[ServerConfig::GetServerName] Entry");
    auto result = m_mgr->GetString("General.server_name", "RS2V Custom Server");
    Logger::Trace("[ServerConfig::GetServerName] Exit - returning '%s'", result.c_str());
    return result;
}
int ServerConfig::GetMaxPlayers() const {
    Logger::Trace("[ServerConfig::GetMaxPlayers] Entry");
    int result = m_mgr->GetInt("General.max_players", 64);
    Logger::Trace("[ServerConfig::GetMaxPlayers] Exit - returning %d", result);
    return result;
}
std::string ServerConfig::GetMapRotationFile() const {
    Logger::Trace("[ServerConfig::GetMapRotationFile] Entry");
    auto result = m_mgr->GetString("General.map_rotation_file", "config/maps.ini");
    Logger::Trace("[ServerConfig::GetMapRotationFile] Exit - returning '%s'", result.c_str());
    return result;
}
std::string ServerConfig::GetGameModesFile() const {
    Logger::Trace("[ServerConfig::GetGameModesFile] Entry");
    auto result = m_mgr->GetString("General.game_modes_file", "config/game_modes.ini");
    Logger::Trace("[ServerConfig::GetGameModesFile] Exit - returning '%s'", result.c_str());
    return result;
}
std::string ServerConfig::GetMotdFile() const {
    Logger::Trace("[ServerConfig::GetMotdFile] Entry");
    auto result = m_mgr->GetString("General.motd_file", "config/motd.txt");
    Logger::Trace("[ServerConfig::GetMotdFile] Exit - returning '%s'", result.c_str());
    return result;
}
int ServerConfig::GetTickRate() const {
    Logger::Trace("[ServerConfig::GetTickRate] Entry");
    int result = m_mgr->GetInt("General.tick_rate", 60);
    Logger::Trace("[ServerConfig::GetTickRate] Exit - returning %d", result);
    return result;
}
int ServerConfig::GetTimeSyncInterval() const {
    Logger::Trace("[ServerConfig::GetTimeSyncInterval] Entry");
    int result = m_mgr->GetInt("General.timesync_interval_s", 30);
    Logger::Trace("[ServerConfig::GetTimeSyncInterval] Exit - returning %d", result);
    return result;
}
bool ServerConfig::IsAnnouncementsEnabled() const {
    Logger::Trace("[ServerConfig::IsAnnouncementsEnabled] Entry");
    bool result = m_mgr->GetBool("General.enable_announcements", true);
    Logger::Trace("[ServerConfig::IsAnnouncementsEnabled] Exit - returning %s", result ? "true" : "false");
    return result;
}
std::string ServerConfig::GetDataDirectory() const {
    Logger::Trace("[ServerConfig::GetDataDirectory] Entry");
    auto result = m_mgr->GetString("General.data_directory", "data/");
    Logger::Trace("[ServerConfig::GetDataDirectory] Exit - returning '%s'", result.c_str());
    return result;
}
std::string ServerConfig::GetLogDirectory() const {
    Logger::Trace("[ServerConfig::GetLogDirectory] Entry");
    auto result = m_mgr->GetString("General.log_directory", "logs/");
    Logger::Trace("[ServerConfig::GetLogDirectory] Exit - returning '%s'", result.c_str());
    return result;
}
bool ServerConfig::IsAdminRconOnly() const {
    Logger::Trace("[ServerConfig::IsAdminRconOnly] Entry");
    bool result = m_mgr->GetBool("General.admin_rcon_only", false);
    Logger::Trace("[ServerConfig::IsAdminRconOnly] Exit - returning %s", result ? "true" : "false");
    return result;
}
std::string ServerConfig::GetMapsDataPath() const {
    Logger::Trace("[ServerConfig::GetMapsDataPath] Entry");
    auto result = m_mgr->GetString("DataPaths.maps_path", "data/maps/");
    Logger::Trace("[ServerConfig::GetMapsDataPath] Exit - returning '%s'", result.c_str());
    return result;
}

// Network
int ServerConfig::GetPort() const {
    Logger::Trace("[ServerConfig::GetPort] Entry");
    int result = m_mgr->GetInt("Network.port", 7777);
    Logger::Trace("[ServerConfig::GetPort] Exit - returning %d", result);
    return result;
}
std::string ServerConfig::GetBindAddress() const {
    Logger::Trace("[ServerConfig::GetBindAddress] Entry");
    auto result = m_mgr->GetString("Network.bind_address", "");
    Logger::Trace("[ServerConfig::GetBindAddress] Exit - returning '%s'", result.c_str());
    return result;
}
int ServerConfig::GetMaxPacketSize() const {
    Logger::Trace("[ServerConfig::GetMaxPacketSize] Entry");
    int result = m_mgr->GetInt("Network.max_packet_size", 1200);
    Logger::Trace("[ServerConfig::GetMaxPacketSize] Exit - returning %d", result);
    return result;
}
int ServerConfig::GetClientIdleTimeout() const {
    Logger::Trace("[ServerConfig::GetClientIdleTimeout] Entry");
    int result = m_mgr->GetInt("Network.client_idle_timeout_s", 300);
    Logger::Trace("[ServerConfig::GetClientIdleTimeout] Exit - returning %d", result);
    return result;
}
int ServerConfig::GetHeartbeatInterval() const {
    Logger::Trace("[ServerConfig::GetHeartbeatInterval] Entry");
    int result = m_mgr->GetInt("Network.heartbeat_interval_s", 5);
    Logger::Trace("[ServerConfig::GetHeartbeatInterval] Exit - returning %d", result);
    return result;
}
bool ServerConfig::IsDualStack() const {
    Logger::Trace("[ServerConfig::IsDualStack] Entry");
    bool result = m_mgr->GetBool("Network.dual_stack", true);
    Logger::Trace("[ServerConfig::IsDualStack] Exit - returning %s", result ? "true" : "false");
    return result;
}
bool ServerConfig::IsReliableTransport() const {
    Logger::Trace("[ServerConfig::IsReliableTransport] Entry");
    bool result = m_mgr->GetBool("Network.reliable_transport", true);
    Logger::Trace("[ServerConfig::IsReliableTransport] Exit - returning %s", result ? "true" : "false");
    return result;
}

// Security
bool ServerConfig::IsSteamAuthEnabled() const {
    Logger::Trace("[ServerConfig::IsSteamAuthEnabled] Entry");
    bool result = m_mgr->GetBool("Security.enable_steam_auth", true);
    Logger::Trace("[ServerConfig::IsSteamAuthEnabled] Exit - returning %s", result ? "true" : "false");
    return result;
}
bool ServerConfig::IsFallbackCustomAuth() const {
    Logger::Trace("[ServerConfig::IsFallbackCustomAuth] Entry");
    bool result = m_mgr->GetBool("Security.fallback_custom_auth", false);
    Logger::Trace("[ServerConfig::IsFallbackCustomAuth] Exit - returning %s", result ? "true" : "false");
    return result;
}
std::string ServerConfig::GetCustomAuthTokensFile() const {
    Logger::Trace("[ServerConfig::GetCustomAuthTokensFile] Entry");
    auto result = m_mgr->GetString("Security.custom_auth_tokens_file", "config/auth_tokens.txt");
    Logger::Trace("[ServerConfig::GetCustomAuthTokensFile] Exit - returning '%s'", result.c_str());
    return result;
}
bool ServerConfig::IsBanManagerEnabled() const {
    Logger::Trace("[ServerConfig::IsBanManagerEnabled] Entry");
    bool result = m_mgr->GetBool("Security.enable_ban_manager", true);
    Logger::Trace("[ServerConfig::IsBanManagerEnabled] Exit - returning %s", result ? "true" : "false");
    return result;
}
std::string ServerConfig::GetBanListFile() const {
    Logger::Trace("[ServerConfig::GetBanListFile] Entry");
    auto result = m_mgr->GetString("Security.ban_list_file", "config/ban_list.txt");
    Logger::Trace("[ServerConfig::GetBanListFile] Exit - returning '%s'", result.c_str());
    return result;
}
bool ServerConfig::IsAntiCheatEnabled() const {
    Logger::Trace("[ServerConfig::IsAntiCheatEnabled] Entry");
    bool result = m_mgr->GetBool("Security.enable_anti_cheat", true);
    Logger::Trace("[ServerConfig::IsAntiCheatEnabled] Exit - returning %s", result ? "true" : "false");
    return result;
}
std::string ServerConfig::GetAntiCheatMode() const {
    Logger::Trace("[ServerConfig::GetAntiCheatMode] Entry");
    auto result = m_mgr->GetString("Security.anti_cheat_mode", "emulate");
    Logger::Trace("[ServerConfig::GetAntiCheatMode] Exit - returning '%s'", result.c_str());
    return result;
}
std::string ServerConfig::GetEacScannerConfigFile() const {
    Logger::Trace("[ServerConfig::GetEacScannerConfigFile] Entry");
    auto result = m_mgr->GetString("Security.eac_scanner_config_file", "config/eac_scanner.json");
    Logger::Trace("[ServerConfig::GetEacScannerConfigFile] Exit - returning '%s'", result.c_str());
    return result;
}

// Logging
std::string ServerConfig::GetLogLevel() const {
    Logger::Trace("[ServerConfig::GetLogLevel] Entry");
    auto result = m_mgr->GetString("Logging.log_level", "info");
    Logger::Trace("[ServerConfig::GetLogLevel] Exit - returning '%s'", result.c_str());
    return result;
}
bool ServerConfig::IsConsoleLogging() const {
    Logger::Trace("[ServerConfig::IsConsoleLogging] Entry");
    bool result = m_mgr->GetBool("Logging.log_to_console", true);
    Logger::Trace("[ServerConfig::IsConsoleLogging] Exit - returning %s", result ? "true" : "false");
    return result;
}
bool ServerConfig::IsFileLogging() const {
    Logger::Trace("[ServerConfig::IsFileLogging] Entry");
    bool result = m_mgr->GetBool("Logging.log_to_file", true);
    Logger::Trace("[ServerConfig::IsFileLogging] Exit - returning %s", result ? "true" : "false");
    return result;
}
std::string ServerConfig::GetLogFileName() const {
    Logger::Trace("[ServerConfig::GetLogFileName] Entry");
    auto result = m_mgr->GetString("Logging.log_file", "server.log");
    Logger::Trace("[ServerConfig::GetLogFileName] Exit - returning '%s'", result.c_str());
    return result;
}
int ServerConfig::GetLogMaxSizeMb() const {
    Logger::Trace("[ServerConfig::GetLogMaxSizeMb] Entry");
    int result = m_mgr->GetInt("Logging.log_max_size_mb", 10);
    Logger::Trace("[ServerConfig::GetLogMaxSizeMb] Exit - returning %d", result);
    return result;
}
int ServerConfig::GetLogMaxFiles() const {
    Logger::Trace("[ServerConfig::GetLogMaxFiles] Entry");
    int result = m_mgr->GetInt("Logging.log_max_files", 5);
    Logger::Trace("[ServerConfig::GetLogMaxFiles] Exit - returning %d", result);
    return result;
}
std::string ServerConfig::GetLogTimestampFormat() const {
    Logger::Trace("[ServerConfig::GetLogTimestampFormat] Entry");
    auto result = m_mgr->GetString("Logging.log_timestamp_format", "%Y-%m-%d %H:%M:%S");
    Logger::Trace("[ServerConfig::GetLogTimestampFormat] Exit - returning '%s'", result.c_str());
    return result;
}

// Performance
int ServerConfig::GetMaxCpuCores() const {
    Logger::Trace("[ServerConfig::GetMaxCpuCores] Entry");
    int result = m_mgr->GetInt("Performance.max_cpu_cores", 0);
    Logger::Trace("[ServerConfig::GetMaxCpuCores] Exit - returning %d", result);
    return result;
}
int ServerConfig::GetCpuAffinityMask() const {
    Logger::Trace("[ServerConfig::GetCpuAffinityMask] Entry");
    int result = m_mgr->GetInt("Performance.cpu_affinity_mask", 0);
    Logger::Trace("[ServerConfig::GetCpuAffinityMask] Exit - returning %d", result);
    return result;
}
bool ServerConfig::IsDynamicTuningEnabled() const {
    Logger::Trace("[ServerConfig::IsDynamicTuningEnabled] Entry");
    bool result = m_mgr->GetBool("Performance.dynamic_tuning_enabled", true);
    Logger::Trace("[ServerConfig::IsDynamicTuningEnabled] Exit - returning %s", result ? "true" : "false");
    return result;
}

// ThreadPool
int ServerConfig::GetWorkerThreadCount() const {
    Logger::Trace("[ServerConfig::GetWorkerThreadCount] Entry");
    int result = m_mgr->GetInt("ThreadPool.worker_thread_count", 0);
    Logger::Trace("[ServerConfig::GetWorkerThreadCount] Exit - returning %d", result);
    return result;
}
int ServerConfig::GetMaxTaskQueueLength() const {
    Logger::Trace("[ServerConfig::GetMaxTaskQueueLength] Entry");
    int result = m_mgr->GetInt("ThreadPool.max_task_queue_length", 256);
    Logger::Trace("[ServerConfig::GetMaxTaskQueueLength] Exit - returning %d", result);
    return result;
}
int ServerConfig::GetSpillThreshold() const {
    Logger::Trace("[ServerConfig::GetSpillThreshold] Entry");
    int result = m_mgr->GetInt("ThreadPool.spill_threshold", 512);
    Logger::Trace("[ServerConfig::GetSpillThreshold] Exit - returning %d", result);
    return result;
}

// MemoryPool
int ServerConfig::GetPreallocateChunks() const {
    Logger::Trace("[ServerConfig::GetPreallocateChunks] Entry");
    int result = m_mgr->GetInt("MemoryPool.preallocate_chunks", 4);
    Logger::Trace("[ServerConfig::GetPreallocateChunks] Exit - returning %d", result);
    return result;
}
int ServerConfig::GetMaxChunks() const {
    Logger::Trace("[ServerConfig::GetMaxChunks] Entry");
    int result = m_mgr->GetInt("MemoryPool.max_chunks", 0);
    Logger::Trace("[ServerConfig::GetMaxChunks] Exit - returning %d", result);
    return result;
}

// Profiler
bool ServerConfig::IsProfilingEnabled() const {
    Logger::Trace("[ServerConfig::IsProfilingEnabled] Entry");
    bool result = m_mgr->GetBool("Profiler.enable_profiling", true);
    Logger::Trace("[ServerConfig::IsProfilingEnabled] Exit - returning %s", result ? "true" : "false");
    return result;
}
int ServerConfig::GetProfilerMinRecordMs() const {
    Logger::Trace("[ServerConfig::GetProfilerMinRecordMs] Entry");
    int result = m_mgr->GetInt("Profiler.min_record_ms", 0);
    Logger::Trace("[ServerConfig::GetProfilerMinRecordMs] Exit - returning %d", result);
    return result;
}
int ServerConfig::GetProfilerBufferSize() const {
    Logger::Trace("[ServerConfig::GetProfilerBufferSize] Entry");
    int result = m_mgr->GetInt("Profiler.buffer_size", 1000);
    Logger::Trace("[ServerConfig::GetProfilerBufferSize] Exit - returning %d", result);
    return result;
}
int ServerConfig::GetProfilerFlushInterval() const {
    Logger::Trace("[ServerConfig::GetProfilerFlushInterval] Entry");
    int result = m_mgr->GetInt("Profiler.flush_interval_s", 10);
    Logger::Trace("[ServerConfig::GetProfilerFlushInterval] Exit - returning %d", result);
    return result;
}
std::string ServerConfig::GetProfilerOutputFormat() const {
    Logger::Trace("[ServerConfig::GetProfilerOutputFormat] Entry");
    auto result = m_mgr->GetString("Profiler.output_format", "json");
    Logger::Trace("[ServerConfig::GetProfilerOutputFormat] Exit - returning '%s'", result.c_str());
    return result;
}
std::string ServerConfig::GetProfilerOutputPath() const {
    Logger::Trace("[ServerConfig::GetProfilerOutputPath] Entry");
    auto result = m_mgr->GetString("Profiler.profiler_output_path", "logs/profiler.json");
    Logger::Trace("[ServerConfig::GetProfilerOutputPath] Exit - returning '%s'", result.c_str());
    return result;
}

// Telemetry
int ServerConfig::GetTelemetryBatchSize() const {
    Logger::Trace("[ServerConfig::GetTelemetryBatchSize] Entry");
    int result = m_mgr->GetInt("Telemetry.batch_size", 50);
    Logger::Trace("[ServerConfig::GetTelemetryBatchSize] Exit - returning %d", result);
    return result;
}
int ServerConfig::GetTelemetryFlushInterval() const {
    Logger::Trace("[ServerConfig::GetTelemetryFlushInterval] Entry");
    int result = m_mgr->GetInt("Telemetry.flush_interval_s", 5);
    Logger::Trace("[ServerConfig::GetTelemetryFlushInterval] Exit - returning %d", result);
    return result;
}

// Admin/RCON
bool ServerConfig::IsRconEnabled() const {
    Logger::Trace("[ServerConfig::IsRconEnabled] Entry");
    bool result = m_mgr->GetBool("Admin.enable_rcon", true);
    Logger::Trace("[ServerConfig::IsRconEnabled] Exit - returning %s", result ? "true" : "false");
    return result;
}
int ServerConfig::GetRconPort() const {
    Logger::Trace("[ServerConfig::GetRconPort] Entry");
    int result = m_mgr->GetInt("Admin.rcon_port", 27020);
    Logger::Trace("[ServerConfig::GetRconPort] Exit - returning %d", result);
    return result;
}
std::string ServerConfig::GetRconPassword() const {
    Logger::Trace("[ServerConfig::GetRconPassword] Entry");
    auto result = m_mgr->GetString("Admin.rcon_password", "ChangeMe123");
    Logger::Trace("[ServerConfig::GetRconPassword] Exit - returning '[REDACTED]'");
    return result;
}
int ServerConfig::GetRconMinLevel() const {
    Logger::Trace("[ServerConfig::GetRconMinLevel] Entry");
    int result = m_mgr->GetInt("Admin.rcon_min_level", 2);
    Logger::Trace("[ServerConfig::GetRconMinLevel] Exit - returning %d", result);
    return result;
}
std::string ServerConfig::GetAdminListFile() const {
    Logger::Trace("[ServerConfig::GetAdminListFile] Entry");
    auto result = m_mgr->GetString("Admin.admin_list_file", "config/admin_list.txt");
    Logger::Trace("[ServerConfig::GetAdminListFile] Exit - returning '%s'", result.c_str());
    return result;
}
bool ServerConfig::IsChatAuthEnabled() const {
    Logger::Trace("[ServerConfig::IsChatAuthEnabled] Entry");
    bool result = m_mgr->GetBool("Admin.chat_auth_enabled", true);
    Logger::Trace("[ServerConfig::IsChatAuthEnabled] Exit - returning %s", result ? "true" : "false");
    return result;
}

// EAC
int ServerConfig::GetEACListenPort() const {
    Logger::Trace("[ServerConfig::GetEACListenPort] Entry");
    int result = m_mgr->GetInt("EAC.listen_port", 7957);
    Logger::Trace("[ServerConfig::GetEACListenPort] Exit - returning %d", result);
    return result;
}

// Access underlying ConfigManager
std::shared_ptr<ConfigManager> ServerConfig::GetManager() const {
    Logger::Trace("[ServerConfig::GetManager] Entry");
    Logger::Trace("[ServerConfig::GetManager] Exit - returning ConfigManager ptr=%p", (void*)m_mgr.get());
    return m_mgr;
}
