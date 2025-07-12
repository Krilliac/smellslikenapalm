// Server/telemetry/CustomMetrics.h
// Application-specific metrics definitions and collection helpers for the RS2V server
//
// Provides:
// - Type-safe metric definitions for all game subsystems
// - Thread-safe atomic counters and gauges
// - Convenient collection and update helpers
// - Integration with existing game logic components

#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>

namespace Telemetry {

// Forward declarations
class TelemetryManager;

// Base metric types for type safety and validation
enum class MetricType {
    COUNTER,    // Monotonically increasing value (packets sent, kills, etc.)
    GAUGE,      // Current value that can go up or down (player count, memory usage)
    HISTOGRAM,  // Distribution of values (latency, packet sizes)
    TIMER       // Duration measurements (frame time, processing time)
};

// Individual metric definition
struct MetricDefinition {
    std::string name;
    std::string description;
    MetricType type;
    std::string unit; // "bytes", "milliseconds", "count", "percent", etc.
    bool enabled = true;
};

// Game server metrics - connection and network related
struct NetworkMetrics {
    // Connection metrics
    std::atomic<uint64_t> totalConnections{0};
    std::atomic<uint64_t> activeConnections{0};
    std::atomic<uint64_t> authenticatedPlayers{0};
    std::atomic<uint64_t> failedAuthentications{0};
    std::atomic<uint64_t> connectionTimeouts{0};
    std::atomic<uint64_t> disconnections{0};
    
    // Packet metrics
    std::atomic<uint64_t> totalPacketsReceived{0};
    std::atomic<uint64_t> totalPacketsSent{0};
    std::atomic<uint64_t> packetsDropped{0};
    std::atomic<uint64_t> malformedPackets{0};
    std::atomic<uint64_t> duplicatePackets{0};
    std::atomic<uint64_t> outOfOrderPackets{0};
    
    // Bandwidth metrics
    std::atomic<uint64_t> bytesReceived{0};
    std::atomic<uint64_t> bytesSent{0};
    std::atomic<uint64_t> compressedBytes{0};
    std::atomic<uint64_t> uncompressedBytes{0};
    
    // Quality metrics
    std::atomic<double> averageLatencyMs{0.0};
    std::atomic<double> packetLossRate{0.0};
    std::atomic<double> jitterMs{0.0};
    std::atomic<uint64_t> highLatencyCount{0}; // Connections above threshold
    
    // Rate limiting metrics
    std::atomic<uint64_t> rateLimitedClients{0};
    std::atomic<uint64_t> bannedConnections{0};
    
    // Helper methods for thread-safe updates
    void IncrementConnection() { totalConnections.fetch_add(1, std::memory_order_relaxed); }
    void IncrementFailedAuth() { failedAuthentications.fetch_add(1, std::memory_order_relaxed); }
    void IncrementPacketReceived() { totalPacketsReceived.fetch_add(1, std::memory_order_relaxed); }
    void IncrementPacketSent() { totalPacketsSent.fetch_add(1, std::memory_order_relaxed); }
    void IncrementPacketDropped() { packetsDropped.fetch_add(1, std::memory_order_relaxed); }
    void IncrementMalformedPacket() { malformedPackets.fetch_add(1, std::memory_order_relaxed); }
    void AddBytesReceived(uint64_t bytes) { bytesReceived.fetch_add(bytes, std::memory_order_relaxed); }
    void AddBytesSent(uint64_t bytes) { bytesSent.fetch_add(bytes, std::memory_order_relaxed); }
    void UpdateLatency(double latencyMs) { averageLatencyMs.store(latencyMs, std::memory_order_relaxed); }
    void UpdatePacketLoss(double lossRate) { packetLossRate.store(lossRate, std::memory_order_relaxed); }
    void UpdatePlayerCounts(uint64_t connections, uint64_t authenticated) {
        activeConnections.store(connections, std::memory_order_relaxed);
        authenticatedPlayers.store(authenticated, std::memory_order_relaxed);
    }
};

// Game logic metrics - gameplay and match statistics
struct GameplayMetrics {
    // Server state
    std::atomic<uint64_t> currentTick{0};
    std::atomic<uint64_t> activeMatches{0};
    std::atomic<uint64_t> totalMatchesPlayed{0};
    std::atomic<uint64_t> matchesCompleted{0};
    std::atomic<uint64_t> matchesAborted{0};
    
    // Player actions
    std::atomic<uint64_t> totalKills{0};
    std::atomic<uint64_t> totalDeaths{0};
    std::atomic<uint64_t> totalAssists{0};
    std::atomic<uint64_t> totalSuicides{0};
    std::atomic<uint64_t> teamKills{0};
    
    // Objective metrics
    std::atomic<uint64_t> objectivesCaptured{0};
    std::atomic<uint64_t> objectivesLost{0};
    std::atomic<uint64_t> objectivesContested{0};
    std::atomic<uint64_t> roundsWon{0};
    std::atomic<uint64_t> roundsLost{0};
    
    // Communication
    std::atomic<uint64_t> chatMessagesSent{0};
    std::atomic<uint64_t> voiceMessagesTransmitted{0};
    std::atomic<uint64_t> adminCommandsExecuted{0};
    
    // Map and rotation
    std::atomic<uint64_t> mapChanges{0};
    std::atomic<uint64_t> voteKicksInitiated{0};
    std::atomic<uint64_t> voteKicksSucceeded{0};
    std::atomic<uint64_t> mapVotesInitiated{0};
    
    // Weapons and equipment
    std::atomic<uint64_t> shotsFired{0};
    std::atomic<uint64_t> shotsHit{0};
    std::atomic<uint64_t> reloadsPerformed{0};
    std::atomic<uint64_t> equipmentUsed{0};
    
    // Helper methods
    void IncrementTick() { currentTick.fetch_add(1, std::memory_order_relaxed); }
    void IncrementKill() { totalKills.fetch_add(1, std::memory_order_relaxed); }
    void IncrementDeath() { totalDeaths.fetch_add(1, std::memory_order_relaxed); }
    void IncrementAssist() { totalAssists.fetch_add(1, std::memory_order_relaxed); }
    void IncrementObjectiveCapture() { objectivesCaptured.fetch_add(1, std::memory_order_relaxed); }
    void IncrementChatMessage() { chatMessagesSent.fetch_add(1, std::memory_order_relaxed); }
    void IncrementShotFired() { shotsFired.fetch_add(1, std::memory_order_relaxed); }
    void IncrementShotHit() { shotsHit.fetch_add(1, std::memory_order_relaxed); }
    void UpdateMatchCount(uint64_t active) { activeMatches.store(active, std::memory_order_relaxed); }
    double GetAccuracy() const {
        uint64_t fired = shotsFired.load(std::memory_order_relaxed);
        uint64_t hit = shotsHit.load(std::memory_order_relaxed);
        return fired > 0 ? (static_cast<double>(hit) / fired) * 100.0 : 0.0;
    }
};

// Performance metrics - server performance and timing
struct PerformanceMetrics {
    // Frame timing
    std::atomic<double> frameTimeMs{0.0};
    std::atomic<double> gameLogicTimeMs{0.0};
    std::atomic<double> physicsTimeMs{0.0};
    std::atomic<double> networkTimeMs{0.0};
    std::atomic<double> renderTimeMs{0.0};
    std::atomic<double> ioTimeMs{0.0};
    
    // Subsystem performance
    std::atomic<double> packetProcessingTimeMs{0.0};
    std::atomic<double> playerUpdateTimeMs{0.0};
    std::atomic<double> collisionDetectionTimeMs{0.0};
    std::atomic<double> aiUpdateTimeMs{0.0};
    std::atomic<double> scriptExecutionTimeMs{0.0};
    
    // Memory and resources
    std::atomic<uint64_t> memoryPoolAllocations{0};
    std::atomic<uint64_t> memoryPoolDeallocations{0};
    std::atomic<uint64_t> outstandingMemoryAllocations{0};
    std::atomic<uint64_t> garbageCollectionCount{0};
    std::atomic<double> garbageCollectionTimeMs{0.0};
    
    // Thread metrics
    std::atomic<uint64_t> activeThreads{0};
    std::atomic<uint64_t> threadPoolTasksQueued{0};
    std::atomic<uint64_t> threadPoolTasksCompleted{0};
    std::atomic<double> threadPoolUtilization{0.0};
    
    // Cache and database metrics
    std::atomic<uint64_t> cacheHits{0};
    std::atomic<uint64_t> cacheMisses{0};
    std::atomic<uint64_t> databaseQueries{0};
    std::atomic<double> avgDatabaseQueryTimeMs{0.0};
    
    // File I/O metrics
    std::atomic<uint64_t> filesOpened{0};
    std::atomic<uint64_t> filesClosed{0};
    std::atomic<uint64_t> diskReads{0};
    std::atomic<uint64_t> diskWrites{0};
    std::atomic<uint64_t> configReloads{0};
    
    // Helper methods
    void UpdateFrameTiming(double frameMs, double gameMs, double physicsMs, double networkMs) {
        frameTimeMs.store(frameMs, std::memory_order_relaxed);
        gameLogicTimeMs.store(gameMs, std::memory_order_relaxed);
        physicsTimeMs.store(physicsMs, std::memory_order_relaxed);
        networkTimeMs.store(networkMs, std::memory_order_relaxed);
    }
    void IncrementMemoryAllocation() { memoryPoolAllocations.fetch_add(1, std::memory_order_relaxed); }
    void IncrementMemoryDeallocation() { memoryPoolDeallocations.fetch_add(1, std::memory_order_relaxed); }
    void IncrementCacheHit() { cacheHits.fetch_add(1, std::memory_order_relaxed); }
    void IncrementCacheMiss() { cacheMisses.fetch_add(1, std::memory_order_relaxed); }
    void IncrementDatabaseQuery() { databaseQueries.fetch_add(1, std::memory_order_relaxed); }
    void UpdateDatabaseQueryTime(double timeMs) { avgDatabaseQueryTimeMs.store(timeMs, std::memory_order_relaxed); }
    double GetCacheHitRate() const {
        uint64_t hits = cacheHits.load(std::memory_order_relaxed);
        uint64_t misses = cacheMisses.load(std::memory_order_relaxed);
        uint64_t total = hits + misses;
        return total > 0 ? (static_cast<double>(hits) / total) * 100.0 : 0.0;
    }
};

// Security and anti-cheat metrics
struct SecurityMetrics {
    // General security events
    std::atomic<uint64_t> securityViolations{0};
    std::atomic<uint64_t> suspiciousActivity{0};
    std::atomic<uint64_t> ipBans{0};
    std::atomic<uint64_t> steamIdBans{0};
    std::atomic<uint64_t> failedLoginAttempts{0};
    
    // Anti-cheat detections
    std::atomic<uint64_t> speedHackDetections{0};
    std::atomic<uint64_t> wallHackDetections{0};
    std::atomic<uint64_t> aimBotDetections{0};
    std::atomic<uint64_t> memoryHackDetections{0};
    std::atomic<uint64_t> packetManipulationDetections{0};
    
    // EAC (Easy Anti-Cheat) metrics
    std::atomic<uint64_t> eacViolations{0};
    std::atomic<uint64_t> eacTimeouts{0};
    std::atomic<uint64_t> eacAuthFailures{0};
    std::atomic<uint64_t> eacKicks{0};
    
    // Admin actions
    std::atomic<uint64_t> playersKicked{0};
    std::atomic<uint64_t> playersBanned{0};
    std::atomic<uint64_t> playersUnbanned{0};
    std::atomic<uint64_t> adminWarningsIssued{0};
    std::atomic<uint64_t> adminCommandsBlocked{0};
    
    // Data validation
    std::atomic<uint64_t> invalidPacketHeaders{0};
    std::atomic<uint64_t> oversizedPackets{0};
    std::atomic<uint64_t> checksumMismatches{0};
    std::atomic<uint64_t> timestampAnomalies{0};
    std::atomic<uint64_t> movementValidationFailures{0};
    
    // Helper methods
    void IncrementSecurityViolation() { securityViolations.fetch_add(1, std::memory_order_relaxed); }
    void IncrementSpeedHackDetection() { speedHackDetections.fetch_add(1, std::memory_order_relaxed); }
    void IncrementWallHackDetection() { wallHackDetections.fetch_add(1, std::memory_order_relaxed); }
    void IncrementAimBotDetection() { aimBotDetections.fetch_add(1, std::memory_order_relaxed); }
    void IncrementPlayerKick() { playersKicked.fetch_add(1, std::memory_order_relaxed); }
    void IncrementPlayerBan() { playersBanned.fetch_add(1, std::memory_order_relaxed); }
    void IncrementEACViolation() { eacViolations.fetch_add(1, std::memory_order_relaxed); }
    void IncrementInvalidPacket() { invalidPacketHeaders.fetch_add(1, std::memory_order_relaxed); }
    void IncrementMovementValidationFailure() { movementValidationFailures.fetch_add(1, std::memory_order_relaxed); }
};

// Error and diagnostic metrics
struct DiagnosticMetrics {
    // Error counters
    std::atomic<uint64_t> totalErrors{0};
    std::atomic<uint64_t> criticalErrors{0};
    std::atomic<uint64_t> warnings{0};
    std::atomic<uint64_t> exceptions{0};
    std::atomic<uint64_t> crashes{0};
    
    // Subsystem errors
    std::atomic<uint64_t> networkErrors{0};
    std::atomic<uint64_t> physicsErrors{0};
    std::atomic<uint64_t> renderingErrors{0};
    std::atomic<uint64_t> scriptingErrors{0};
    std::atomic<uint64_t> configErrors{0};
    std::atomic<uint64_t> databaseErrors{0};
    
    // Recovery actions
    std::atomic<uint64_t> automaticRecoveries{0};
    std::atomic<uint64_t> subsystemRestarts{0};
    std::atomic<uint64_t> connectionRecoveries{0};
    std::atomic<uint64_t> memoryLeakDetections{0};
    
    // Health checks
    std::atomic<uint64_t> healthChecksPassed{0};
    std::atomic<uint64_t> healthChecksFailed{0};
    std::atomic<double> lastHealthCheckScore{100.0};
    
    // Helper methods
    void IncrementError() { totalErrors.fetch_add(1, std::memory_order_relaxed); }
    void IncrementCriticalError() { criticalErrors.fetch_add(1, std::memory_order_relaxed); }
    void IncrementWarning() { warnings.fetch_add(1, std::memory_order_relaxed); }
    void IncrementException() { exceptions.fetch_add(1, std::memory_order_relaxed); }
    void IncrementNetworkError() { networkErrors.fetch_add(1, std::memory_order_relaxed); }
    void IncrementPhysicsError() { physicsErrors.fetch_add(1, std::memory_order_relaxed); }
    void IncrementAutomaticRecovery() { automaticRecoveries.fetch_add(1, std::memory_order_relaxed); }
    void UpdateHealthScore(double score) { lastHealthCheckScore.store(score, std::memory_order_relaxed); }
};

// Main custom metrics container
class CustomMetrics {
public:
    CustomMetrics() = default;
    ~CustomMetrics() = default;
    
    // Delete copy constructor and assignment operator for safety
    CustomMetrics(const CustomMetrics&) = delete;
    CustomMetrics& operator=(const CustomMetrics&) = delete;
    
    // Metric categories
    NetworkMetrics network;
    GameplayMetrics gameplay;
    PerformanceMetrics performance;
    SecurityMetrics security;
    DiagnosticMetrics diagnostics;
    
    // Snapshot functionality
    struct Snapshot {
        std::chrono::system_clock::time_point timestamp;
        NetworkMetrics network;
        GameplayMetrics gameplay;
        PerformanceMetrics performance;
        SecurityMetrics security;
        DiagnosticMetrics diagnostics;
    };
    
    Snapshot CreateSnapshot() const;
    void LoadFromSnapshot(const Snapshot& snapshot);
    
    // Reset all metrics (useful for testing or periodic resets)
    void Reset();
    
    // Get formatted summary for logging
    std::string GetSummaryString() const;
    
    // Metric registration and metadata
    std::vector<MetricDefinition> GetMetricDefinitions() const;
    bool IsMetricEnabled(const std::string& metricName) const;
    void SetMetricEnabled(const std::string& metricName, bool enabled);

private:
    mutable std::mutex m_configMutex;
    std::unordered_map<std::string, bool> m_metricEnabledState;
};

// Convenience macros for easy metric updates throughout the codebase
#define METRICS_NETWORK() Telemetry::TelemetryManager::Instance().GetCustomMetrics().network
#define METRICS_GAMEPLAY() Telemetry::TelemetryManager::Instance().GetCustomMetrics().gameplay
#define METRICS_PERFORMANCE() Telemetry::TelemetryManager::Instance().GetCustomMetrics().performance
#define METRICS_SECURITY() Telemetry::TelemetryManager::Instance().GetCustomMetrics().security
#define METRICS_DIAGNOSTICS() Telemetry::TelemetryManager::Instance().GetCustomMetrics().diagnostics

// Scoped timer helper for automatic performance measurement
class ScopedTimer {
public:
    explicit ScopedTimer(std::atomic<double>& target)
        : m_target(target), m_start(std::chrono::high_resolution_clock::now()) {}
    
    ~ScopedTimer() {
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::milli>(end - m_start);
        m_target.store(duration.count(), std::memory_order_relaxed);
    }

private:
    std::atomic<double>& m_target;
    std::chrono::high_resolution_clock::time_point m_start;
};

// Macro for easy scoped timing
#define METRICS_SCOPED_TIMER(metric) \
    Telemetry::ScopedTimer _timer(metric)

// Frame timing helper
#define METRICS_TIME_FRAME() \
    METRICS_SCOPED_TIMER(METRICS_PERFORMANCE().frameTimeMs)

#define METRICS_TIME_PHYSICS() \
    METRICS_SCOPED_TIMER(METRICS_PERFORMANCE().physicsTimeMs)

#define METRICS_TIME_NETWORK() \
    METRICS_SCOPED_TIMER(METRICS_PERFORMANCE().networkTimeMs)

#define METRICS_TIME_GAME_LOGIC() \
    METRICS_SCOPED_TIMER(METRICS_PERFORMANCE().gameLogicTimeMs)

} // namespace Telemetry