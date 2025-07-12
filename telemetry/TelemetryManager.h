// Server/telemetry/TelemetryManager.h
// Comprehensive telemetry and metrics collection system for RS2V server
//
// Provides:
// - Real-time system metrics (CPU, memory, network)
// - Application-specific game metrics (players, tick rate, packets)
// - Thread-safe, low-overhead collection
// - Pluggable reporting backends (file, Prometheus, in-memory)
// - Automatic sampling with configurable intervals

#pragma once

#include <string>
#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <unordered_map>

namespace Telemetry {

// Forward declarations
class MetricsReporter;

// System and application metrics snapshot
struct MetricsSnapshot {
    // Timestamp
    std::chrono::system_clock::time_point timestamp;
    
    // System metrics
    double cpuUsagePercent;
    uint64_t memoryUsedBytes;
    uint64_t memoryTotalBytes;
    uint64_t networkBytesSent;
    uint64_t networkBytesReceived;
    uint64_t diskReadBytes;
    uint64_t diskWriteBytes;
    
    // Application metrics - Game Server
    uint64_t activeConnections;
    uint64_t authenticatedPlayers;
    uint64_t totalPacketsProcessed;
    uint64_t totalPacketsDropped;
    uint64_t currentTick;
    double averageLatencyMs;
    double packetLossRate;
    
    // Application metrics - Game Logic
    uint64_t activeMatches;
    uint64_t totalKills;
    uint64_t totalDeaths;
    uint64_t objectivesCaptured;
    uint64_t chatMessagesSent;
    
    // Performance metrics
    double frameTimeMs;
    double physicsTimeMs;
    double networkTimeMs;
    double gameLogicTimeMs;
    
    // Error and security metrics
    uint64_t securityViolations;
    uint64_t malformedPackets;
    uint64_t speedHackDetections;
    uint64_t kickedPlayers;
    uint64_t bannedPlayers;
    
    MetricsSnapshot() : timestamp(std::chrono::system_clock::now()),
                       cpuUsagePercent(0.0), memoryUsedBytes(0), memoryTotalBytes(0),
                       networkBytesSent(0), networkBytesReceived(0),
                       diskReadBytes(0), diskWriteBytes(0),
                       activeConnections(0), authenticatedPlayers(0),
                       totalPacketsProcessed(0), totalPacketsDropped(0),
                       currentTick(0), averageLatencyMs(0.0), packetLossRate(0.0),
                       activeMatches(0), totalKills(0), totalDeaths(0),
                       objectivesCaptured(0), chatMessagesSent(0),
                       frameTimeMs(0.0), physicsTimeMs(0.0), networkTimeMs(0.0),
                       gameLogicTimeMs(0.0), securityViolations(0),
                       malformedPackets(0), speedHackDetections(0),
                       kickedPlayers(0), bannedPlayers(0) {}
};

// Custom application metrics that can be updated from game logic
struct CustomMetrics {
    // Game state counters
    std::atomic<uint64_t> activeConnections{0};
    std::atomic<uint64_t> authenticatedPlayers{0};
    std::atomic<uint64_t> totalPacketsProcessed{0};
    std::atomic<uint64_t> totalPacketsDropped{0};
    std::atomic<uint64_t> currentTick{0};
    std::atomic<double> averageLatencyMs{0.0};
    std::atomic<double> packetLossRate{0.0};
    
    // Game logic counters
    std::atomic<uint64_t> activeMatches{0};
    std::atomic<uint64_t> totalKills{0};
    std::atomic<uint64_t> totalDeaths{0};
    std::atomic<uint64_t> objectivesCaptured{0};
    std::atomic<uint64_t> chatMessagesSent{0};
    
    // Performance timing
    std::atomic<double> frameTimeMs{0.0};
    std::atomic<double> physicsTimeMs{0.0};
    std::atomic<double> networkTimeMs{0.0};
    std::atomic<double> gameLogicTimeMs{0.0};
    
    // Security and error tracking
    std::atomic<uint64_t> securityViolations{0};
    std::atomic<uint64_t> malformedPackets{0};
    std::atomic<uint64_t> speedHackDetections{0};
    std::atomic<uint64_t> kickedPlayers{0};
    std::atomic<uint64_t> bannedPlayers{0};
    
    // Increment helpers for thread-safe updates
    void IncrementPacketsProcessed() { totalPacketsProcessed.fetch_add(1, std::memory_order_relaxed); }
    void IncrementPacketsDropped() { totalPacketsDropped.fetch_add(1, std::memory_order_relaxed); }
    void IncrementSecurityViolation() { securityViolations.fetch_add(1, std::memory_order_relaxed); }
    void IncrementMalformedPacket() { malformedPackets.fetch_add(1, std::memory_order_relaxed); }
    void IncrementSpeedHackDetection() { speedHackDetections.fetch_add(1, std::memory_order_relaxed); }
    void IncrementKick() { kickedPlayers.fetch_add(1, std::memory_order_relaxed); }
    void IncrementBan() { bannedPlayers.fetch_add(1, std::memory_order_relaxed); }
    void IncrementKill() { totalKills.fetch_add(1, std::memory_order_relaxed); }
    void IncrementDeath() { totalDeaths.fetch_add(1, std::memory_order_relaxed); }
    void IncrementObjectiveCapture() { objectivesCaptured.fetch_add(1, std::memory_order_relaxed); }
    void IncrementChatMessage() { chatMessagesSent.fetch_add(1, std::memory_order_relaxed); }
    
    void UpdateLatency(double latencyMs) { 
        averageLatencyMs.store(latencyMs, std::memory_order_relaxed); 
    }
    void UpdatePacketLoss(double lossRate) { 
        packetLossRate.store(lossRate, std::memory_order_relaxed); 
    }
    void UpdatePlayerCounts(uint64_t connections, uint64_t authenticated) {
        activeConnections.store(connections, std::memory_order_relaxed);
        authenticatedPlayers.store(authenticated, std::memory_order_relaxed);
    }
    void UpdateTick(uint64_t tick) { 
        currentTick.store(tick, std::memory_order_relaxed); 
    }
    void UpdateFrameTiming(double frameMs, double physicsMs, double networkMs, double gameMs) {
        frameTimeMs.store(frameMs, std::memory_order_relaxed);
        physicsTimeMs.store(physicsMs, std::memory_order_relaxed);
        networkTimeMs.store(networkMs, std::memory_order_relaxed);
        gameLogicTimeMs.store(gameMs, std::memory_order_relaxed);
    }
};

// Configuration for telemetry system
struct TelemetryConfig {
    bool enabled = true;
    std::chrono::milliseconds samplingInterval = std::chrono::milliseconds(1000); // 1 second
    std::string metricsDirectory = "telemetry";
    bool enableFileReporter = true;
    bool enablePrometheusReporter = false;
    int prometheusPort = 9090;
    size_t maxSamplesInMemory = 3600; // 1 hour at 1 second intervals
    bool enableSystemMetrics = true;
    bool enableApplicationMetrics = true;
    bool enablePerformanceMetrics = true;
    bool enableSecurityMetrics = true;
};

// Main telemetry manager - singleton pattern
class TelemetryManager {
public:
    // Get singleton instance
    static TelemetryManager& Instance();
    
    // Lifecycle management
    bool Initialize(const TelemetryConfig& config = TelemetryConfig{});
    void Shutdown();
    bool IsRunning() const { return m_running.load(); }
    
    // Reporter management
    void AddReporter(std::unique_ptr<MetricsReporter> reporter);
    void RemoveAllReporters();
    
    // Metrics collection control
    void StartSampling();
    void StopSampling();
    void ForceSample(); // Immediate snapshot
    
    // Custom metrics access - thread-safe
    CustomMetrics& GetCustomMetrics() { return m_customMetrics; }
    const CustomMetrics& GetCustomMetrics() const { return m_customMetrics; }
    
    // Get latest snapshot
    MetricsSnapshot GetLatestSnapshot() const;
    std::vector<MetricsSnapshot> GetRecentSnapshots(size_t count) const;
    
    // Configuration
    void UpdateConfig(const TelemetryConfig& config);
    TelemetryConfig GetConfig() const { return m_config; }
    
    // Statistics
    uint64_t GetTotalSamplesTaken() const { return m_totalSamples.load(); }
    std::chrono::steady_clock::time_point GetStartTime() const { return m_startTime; }
    
    // Error handling
    std::vector<std::string> GetLastErrors() const;
    void ClearErrors();

private:
    TelemetryManager() = default;
    ~TelemetryManager();
    
    // Delete copy constructor and assignment operator
    TelemetryManager(const TelemetryManager&) = delete;
    TelemetryManager& operator=(const TelemetryManager&) = delete;
    
    // Internal sampling loop
    void SamplingLoop();
    
    // Metrics collection methods
    MetricsSnapshot CollectSnapshot();
    void CollectSystemMetrics(MetricsSnapshot& snapshot);
    void CollectApplicationMetrics(MetricsSnapshot& snapshot);
    
    // System-specific metric collection
    double GetCPUUsage();
    std::pair<uint64_t, uint64_t> GetMemoryUsage(); // used, total
    std::pair<uint64_t, uint64_t> GetNetworkStats(); // sent, received
    std::pair<uint64_t, uint64_t> GetDiskStats(); // read, write
    
    // Error reporting
    void ReportError(const std::string& error);
    
    // Configuration and state
    TelemetryConfig m_config;
    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_running{false};
    std::chrono::steady_clock::time_point m_startTime;
    
    // Sampling thread
    std::thread m_samplingThread;
    
    // Metrics storage
    CustomMetrics m_customMetrics;
    mutable std::mutex m_snapshotMutex;
    std::vector<MetricsSnapshot> m_snapshots;
    size_t m_snapshotIndex = 0;
    
    // Statistics
    std::atomic<uint64_t> m_totalSamples{0};
    
    // Reporters
    std::vector<std::unique_ptr<MetricsReporter>> m_reporters;
    mutable std::mutex m_reporterMutex;
    
    // Error tracking
    mutable std::mutex m_errorMutex;
    std::vector<std::string> m_errors;
    static constexpr size_t MAX_ERRORS = 100;
    
    // CPU usage tracking (for delta calculations)
    struct CPUTimes {
        uint64_t user = 0;
        uint64_t nice = 0;
        uint64_t system = 0;
        uint64_t idle = 0;
        uint64_t iowait = 0;
        uint64_t irq = 0;
        uint64_t softirq = 0;
        uint64_t steal = 0;
    };
    CPUTimes m_lastCPUTimes;
    bool m_hasPreviousCPUTimes = false;
    
    // Network usage tracking
    struct NetworkStats {
        uint64_t bytesSent = 0;
        uint64_t bytesReceived = 0;
    };
    NetworkStats m_lastNetworkStats;
    bool m_hasPreviousNetworkStats = false;
    
    // Disk usage tracking
    struct DiskStats {
        uint64_t readBytes = 0;
        uint64_t writeBytes = 0;
    };
    DiskStats m_lastDiskStats;
    bool m_hasPreviousDiskStats = false;
};

// Convenience macros for common telemetry operations
#define TELEMETRY_INCREMENT_PACKETS_PROCESSED() \
    Telemetry::TelemetryManager::Instance().GetCustomMetrics().IncrementPacketsProcessed()

#define TELEMETRY_INCREMENT_PACKETS_DROPPED() \
    Telemetry::TelemetryManager::Instance().GetCustomMetrics().IncrementPacketsDropped()

#define TELEMETRY_INCREMENT_SECURITY_VIOLATION() \
    Telemetry::TelemetryManager::Instance().GetCustomMetrics().IncrementSecurityViolation()

#define TELEMETRY_INCREMENT_MALFORMED_PACKET() \
    Telemetry::TelemetryManager::Instance().GetCustomMetrics().IncrementMalformedPacket()

#define TELEMETRY_INCREMENT_SPEED_HACK() \
    Telemetry::TelemetryManager::Instance().GetCustomMetrics().IncrementSpeedHackDetection()

#define TELEMETRY_INCREMENT_KICK() \
    Telemetry::TelemetryManager::Instance().GetCustomMetrics().IncrementKick()

#define TELEMETRY_INCREMENT_BAN() \
    Telemetry::TelemetryManager::Instance().GetCustomMetrics().IncrementBan()

#define TELEMETRY_INCREMENT_KILL() \
    Telemetry::TelemetryManager::Instance().GetCustomMetrics().IncrementKill()

#define TELEMETRY_INCREMENT_DEATH() \
    Telemetry::TelemetryManager::Instance().GetCustomMetrics().IncrementDeath()

#define TELEMETRY_UPDATE_PLAYER_COUNTS(connections, authenticated) \
    Telemetry::TelemetryManager::Instance().GetCustomMetrics().UpdatePlayerCounts(connections, authenticated)

#define TELEMETRY_UPDATE_TICK(tick) \
    Telemetry::TelemetryManager::Instance().GetCustomMetrics().UpdateTick(tick)

#define TELEMETRY_UPDATE_LATENCY(latencyMs) \
    Telemetry::TelemetryManager::Instance().GetCustomMetrics().UpdateLatency(latencyMs)

} // namespace Telemetry