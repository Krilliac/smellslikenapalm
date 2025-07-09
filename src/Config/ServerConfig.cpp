#include "Config/ServerConfig.h"
#include "Utils/StringUtils.h"

ServerConfig::ServerConfig(const std::shared_ptr<ConfigManager>& mgr)
  : m_mgr(mgr)
{}

// General
std::string ServerConfig::GetServerName() const {
    return m_mgr->GetString("General.server_name", "RS2V Custom Server");
}
int ServerConfig::GetMaxPlayers() const {
    return m_mgr->GetInt("General.max_players", 64);
}
std::string ServerConfig::GetMapRotationFile() const {
    return m_mgr->GetString("General.map_rotation_file", "config/maps.ini");
}
std::string ServerConfig::GetGameModesFile() const {
    return m_mgr->GetString("General.game_modes_file", "config/game_modes.ini");
}
std::string ServerConfig::GetMotdFile() const {
    return m_mgr->GetString("General.motd_file", "config/motd.txt");
}
int ServerConfig::GetTickRate() const {
    return m_mgr->GetInt("General.tick_rate", 60);
}
int ServerConfig::GetTimeSyncInterval() const {
    return m_mgr->GetInt("General.timesync_interval_s", 30);
}
bool ServerConfig::IsAnnouncementsEnabled() const {
    return m_mgr->GetBool("General.enable_announcements", true);
}
std::string ServerConfig::GetDataDirectory() const {
    return m_mgr->GetString("General.data_directory", "data/");
}
std::string ServerConfig::GetLogDirectory() const {
    return m_mgr->GetString("General.log_directory", "logs/");
}
bool ServerConfig::IsAdminRconOnly() const {
    return m_mgr->GetBool("General.admin_rcon_only", false);
}

// Network
int ServerConfig::GetPort() const {
    return m_mgr->GetInt("Network.port", 7777);
}
std::string ServerConfig::GetBindAddress() const {
    return m_mgr->GetString("Network.bind_address", "");
}
int ServerConfig::GetMaxPacketSize() const {
    return m_mgr->GetInt("Network.max_packet_size", 1200);
}
int ServerConfig::GetClientIdleTimeout() const {
    return m_mgr->GetInt("Network.client_idle_timeout_s", 300);
}
int ServerConfig::GetHeartbeatInterval() const {
    return m_mgr->GetInt("Network.heartbeat_interval_s", 5);
}
bool ServerConfig::IsDualStack() const {
    return m_mgr->GetBool("Network.dual_stack", true);
}
bool ServerConfig::IsReliableTransport() const {
    return m_mgr->GetBool("Network.reliable_transport", true);
}

// Security
bool ServerConfig::IsSteamAuthEnabled() const {
    return m_mgr->GetBool("Security.enable_steam_auth", true);
}
bool ServerConfig::IsFallbackCustomAuth() const {
    return m_mgr->GetBool("Security.fallback_custom_auth", false);
}
std::string ServerConfig::GetCustomAuthTokensFile() const {
    return m_mgr->GetString("Security.custom_auth_tokens_file", "config/auth_tokens.txt");
}
bool ServerConfig::IsBanManagerEnabled() const {
    return m_mgr->GetBool("Security.enable_ban_manager", true);
}
std::string ServerConfig::GetBanListFile() const {
    return m_mgr->GetString("Security.ban_list_file", "config/ban_list.txt");
}
bool ServerConfig::IsAntiCheatEnabled() const {
    return m_mgr->GetBool("Security.enable_anti_cheat", true);
}
std::string ServerConfig::GetAntiCheatMode() const {
    return m_mgr->GetString("Security.anti_cheat_mode", "emulate");
}
std::string ServerConfig::GetEacScannerConfigFile() const {
    return m_mgr->GetString("Security.eac_scanner_config_file", "config/eac_scanner.json");
}

// Logging
std::string ServerConfig::GetLogLevel() const {
    return m_mgr->GetString("Logging.log_level", "info");
}
bool ServerConfig::IsConsoleLogging() const {
    return m_mgr->GetBool("Logging.log_to_console", true);
}
bool ServerConfig::IsFileLogging() const {
    return m_mgr->GetBool("Logging.log_to_file", true);
}
std::string ServerConfig::GetLogFileName() const {
    return m_mgr->GetString("Logging.log_file", "server.log");
}
int ServerConfig::GetLogMaxSizeMb() const {
    return m_mgr->GetInt("Logging.log_max_size_mb", 10);
}
int ServerConfig::GetLogMaxFiles() const {
    return m_mgr->GetInt("Logging.log_max_files", 5);
}
std::string ServerConfig::GetLogTimestampFormat() const {
    return m_mgr->GetString("Logging.log_timestamp_format", "%Y-%m-%d %H:%M:%S");
}

// Performance
int ServerConfig::GetMaxCpuCores() const {
    return m_mgr->GetInt("Performance.max_cpu_cores", 0);
}
int ServerConfig::GetCpuAffinityMask() const {
    return m_mgr->GetInt("Performance.cpu_affinity_mask", 0);
}
bool ServerConfig::IsDynamicTuningEnabled() const {
    return m_mgr->GetBool("Performance.dynamic_tuning_enabled", true);
}

// ThreadPool
int ServerConfig::GetWorkerThreadCount() const {
    return m_mgr->GetInt("ThreadPool.worker_thread_count", 0);
}
int ServerConfig::GetMaxTaskQueueLength() const {
    return m_mgr->GetInt("ThreadPool.max_task_queue_length", 256);
}
int ServerConfig::GetSpillThreshold() const {
    return m_mgr->GetInt("ThreadPool.spill_threshold", 512);
}

// MemoryPool
int ServerConfig::GetPreallocateChunks() const {
    return m_mgr->GetInt("MemoryPool.preallocate_chunks", 4);
}
int ServerConfig::GetMaxChunks() const {
    return m_mgr->GetInt("MemoryPool.max_chunks", 0);
}

// Profiler
bool ServerConfig::IsProfilingEnabled() const {
    return m_mgr->GetBool("Profiler.enable_profiling", true);
}
int ServerConfig::GetProfilerMinRecordMs() const {
    return m_mgr->GetInt("Profiler.min_record_ms", 0);
}
int ServerConfig::GetProfilerBufferSize() const {
    return m_mgr->GetInt("Profiler.buffer_size", 1000);
}
int ServerConfig::GetProfilerFlushInterval() const {
    return m_mgr->GetInt("Profiler.flush_interval_s", 10);
}
std::string ServerConfig::GetProfilerOutputFormat() const {
    return m_mgr->GetString("Profiler.output_format", "json");
}
std::string ServerConfig::GetProfilerOutputPath() const {
    return m_mgr->GetString("Profiler.profiler_output_path", "logs/profiler.json");
}

// Telemetry
int ServerConfig::GetTelemetryBatchSize() const {
    return m_mgr->GetInt("Telemetry.batch_size", 50);
}
int ServerConfig::GetTelemetryFlushInterval() const {
    return m_mgr->GetInt("Telemetry.flush_interval_s", 5);
}

// Admin/RCON
bool ServerConfig::IsRconEnabled() const {
    return m_mgr->GetBool("Admin.enable_rcon", true);
}
int ServerConfig::GetRconPort() const {
    return m_mgr->GetInt("Admin.rcon_port", 27020);
}
std::string ServerConfig::GetRconPassword() const {
    return m_mgr->GetString("Admin.rcon_password", "ChangeMe123");
}
int ServerConfig::GetRconMinLevel() const {
    return m_mgr->GetInt("Admin.rcon_min_level", 2);
}
std::string ServerConfig::GetAdminListFile() const {
    return m_mgr->GetString("Admin.admin_list_file", "config/admin_list.txt");
}
bool ServerConfig::IsChatAuthEnabled() const {
    return m_mgr->GetBool("Admin.chat_auth_enabled", true);
}