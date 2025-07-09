#pragma once

#include <string>
#include <vector>
#include "Config/ConfigManager.h"

class ServerConfig {
public:
    explicit ServerConfig(const std::shared_ptr<ConfigManager>& mgr);
    
    // General
    std::string GetServerName() const;
    int         GetMaxPlayers() const;
    std::string GetMapRotationFile() const;
    std::string GetGameModesFile() const;
    std::string GetMotdFile() const;
    int         GetTickRate() const;
    int         GetTimeSyncInterval() const;
    bool        IsAnnouncementsEnabled() const;
    std::string GetDataDirectory() const;
    std::string GetLogDirectory() const;
    bool        IsAdminRconOnly() const;

    // Network
    int         GetPort() const;
    std::string GetBindAddress() const;
    int         GetMaxPacketSize() const;
    int         GetClientIdleTimeout() const;
    int         GetHeartbeatInterval() const;
    bool        IsDualStack() const;
    bool        IsReliableTransport() const;

    // Security
    bool        IsSteamAuthEnabled() const;
    bool        IsFallbackCustomAuth() const;
    std::string GetCustomAuthTokensFile() const;
    bool        IsBanManagerEnabled() const;
    std::string GetBanListFile() const;
    bool        IsAntiCheatEnabled() const;
    std::string GetAntiCheatMode() const;
    std::string GetEacScannerConfigFile() const;

    // Logging
    std::string GetLogLevel() const;
    bool        IsConsoleLogging() const;
    bool        IsFileLogging() const;
    std::string GetLogFileName() const;
    int         GetLogMaxSizeMb() const;
    int         GetLogMaxFiles() const;
    std::string GetLogTimestampFormat() const;
    
    // Performance
    int         GetMaxCpuCores() const;
    int         GetCpuAffinityMask() const;
    bool        IsDynamicTuningEnabled() const;

    // ThreadPool
    int         GetWorkerThreadCount() const;
    int         GetMaxTaskQueueLength() const;
    int         GetSpillThreshold() const;

    // MemoryPool
    int         GetPreallocateChunks() const;
    int         GetMaxChunks() const;

    // Profiler
    bool        IsProfilingEnabled() const;
    int         GetProfilerMinRecordMs() const;
    int         GetProfilerBufferSize() const;
    int         GetProfilerFlushInterval() const;
    std::string GetProfilerOutputFormat() const;
    std::string GetProfilerOutputPath() const;

    // Telemetry
    int         GetTelemetryBatchSize() const;
    int         GetTelemetryFlushInterval() const;

    // Admin/RCON
    bool        IsRconEnabled() const;
    int         GetRconPort() const;
    std::string GetRconPassword() const;
    int         GetRconMinLevel() const;
    std::string GetAdminListFile() const;
    bool        IsChatAuthEnabled() const;

private:
    std::shared_ptr<ConfigManager> m_mgr;
};