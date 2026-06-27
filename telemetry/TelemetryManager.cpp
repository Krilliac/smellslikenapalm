// Server/telemetry/TelemetryManager.cpp
// Implementation of comprehensive telemetry system for the RS2V server

#include "TelemetryManager.h"
#include "MetricsReporter.h"
#include "Utils/Logger.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iomanip>

// Platform-specific includes for system metrics
#ifdef _WIN32
    #include <windows.h>
    #include <psapi.h>
    #include <pdh.h>
    #pragma comment(lib, "pdh.lib")
    #pragma comment(lib, "psapi.lib")
#elif defined(__linux__)
    #include <unistd.h>
    #include <sys/types.h>
    #include <sys/stat.h>
    #include <sys/sysinfo.h>
    #include <ifaddrs.h>
    #include <net/if.h>
#elif defined(__APPLE__)
    #include <sys/types.h>
    #include <sys/sysctl.h>
    #include <mach/mach.h>
    #include <mach/processor_info.h>
    #include <mach/mach_host.h>
#endif

namespace Telemetry {

// Static instance
TelemetryManager& TelemetryManager::Instance() {
    Logger::Trace("[TelemetryManager::Instance] Retrieving singleton instance");
    static TelemetryManager instance;
    Logger::Trace("[TelemetryManager::Instance] Returning singleton instance at address %p", (void*)&instance);
    return instance;
}

TelemetryManager::~TelemetryManager() {
    Logger::Trace("[TelemetryManager::~TelemetryManager] Destructor invoked, initiating shutdown sequence");
    Shutdown();
    Logger::Trace("[TelemetryManager::~TelemetryManager] Destructor completed");
}

bool TelemetryManager::Initialize(const TelemetryConfig& config) {
    Logger::Trace("[TelemetryManager::Initialize] Entry - config.enabled=%d, config.enableFileReporter=%d, config.enableSystemMetrics=%d, config.enableApplicationMetrics=%d, config.maxSamplesInMemory=%zu, config.samplingInterval=%lldms, config.metricsDirectory=%s",
                 config.enabled, config.enableFileReporter, config.enableSystemMetrics, config.enableApplicationMetrics,
                 config.maxSamplesInMemory, config.samplingInterval.count(), config.metricsDirectory.c_str());

    if (m_initialized.exchange(true)) {
        Logger::Warn("TelemetryManager already initialized");
        Logger::Debug("[TelemetryManager::Initialize] Skipping initialization because m_initialized was already true");
        Logger::Trace("[TelemetryManager::Initialize] Exit - returning true (already initialized)");
        return true;
    }

    Logger::Info("Initializing TelemetryManager...");

    m_config = config;
    m_startTime = std::chrono::steady_clock::now();
    Logger::Debug("[TelemetryManager::Initialize] Configuration stored, start time recorded");

    // Create metrics directory if enabled
    if (m_config.enableFileReporter) {
        Logger::Debug("[TelemetryManager::Initialize] File reporter is enabled, creating metrics directory: %s", m_config.metricsDirectory.c_str());
        try {
            std::filesystem::create_directories(m_config.metricsDirectory);
            Logger::Info("Created telemetry directory: %s", m_config.metricsDirectory.c_str());
        } catch (const std::exception& ex) {
            Logger::Error("[TelemetryManager::Initialize] Exception while creating metrics directory '%s': %s", m_config.metricsDirectory.c_str(), ex.what());
            ReportError("Failed to create metrics directory: " + std::string(ex.what()));
            Logger::Trace("[TelemetryManager::Initialize] Exit - returning false (directory creation failed)");
            return false;
        }
    } else {
        Logger::Debug("[TelemetryManager::Initialize] File reporter is disabled, skipping directory creation");
    }

    // Initialize snapshot storage
    {
        Logger::Debug("[TelemetryManager::Initialize] Initializing snapshot storage with capacity %zu", m_config.maxSamplesInMemory);
        std::lock_guard<std::mutex> lock(m_snapshotMutex);
        m_snapshots.clear();
        m_snapshots.reserve(m_config.maxSamplesInMemory);
        m_snapshotIndex = 0;
        Logger::Trace("[TelemetryManager::Initialize] Snapshot storage cleared and reserved, snapshotIndex reset to 0");
    }

    // Initialize reporters
    {
        std::lock_guard<std::mutex> lock(m_reporterMutex);
        Logger::Debug("[TelemetryManager::Initialize] Initializing %zu registered reporters", m_reporters.size());
        for (size_t i = 0; i < m_reporters.size(); ++i) {
            auto& reporter = m_reporters[i];
            try {
                Logger::Trace("[TelemetryManager::Initialize] Initializing reporter %zu with directory '%s'", i, m_config.metricsDirectory.c_str());
                reporter->Initialize(m_config.metricsDirectory);
                Logger::Info("Initialized telemetry reporter");
                Logger::Debug("[TelemetryManager::Initialize] Reporter %zu initialized successfully", i);
            } catch (const std::exception& ex) {
                Logger::Error("[TelemetryManager::Initialize] Exception initializing reporter %zu: %s", i, ex.what());
                ReportError("Failed to initialize reporter: " + std::string(ex.what()));
            }
        }
    }

    Logger::Info("TelemetryManager initialized successfully");
    Logger::Trace("[TelemetryManager::Initialize] Exit - returning true (success)");
    return true;
}

void TelemetryManager::Shutdown() {
    Logger::Trace("[TelemetryManager::Shutdown] Entry");

    if (!m_initialized.exchange(false)) {
        Logger::Debug("[TelemetryManager::Shutdown] Not initialized, nothing to shut down");
        Logger::Trace("[TelemetryManager::Shutdown] Exit - early return (was not initialized)");
        return;
    }

    Logger::Info("Shutting down TelemetryManager...");

    // Stop sampling
    Logger::Debug("[TelemetryManager::Shutdown] Stopping sampling thread");
    StopSampling();

    // Take final snapshot
    if (m_config.enabled) {
        Logger::Debug("[TelemetryManager::Shutdown] Telemetry is enabled, taking final snapshot");
        try {
            ForceSample();
            Logger::Info("[TelemetryManager::Shutdown] Final telemetry snapshot captured successfully");
        } catch (const std::exception& ex) {
            Logger::Error("Failed to take final telemetry sample: %s", ex.what());
            Logger::Error("[TelemetryManager::Shutdown] Exception details during final sample: %s", ex.what());
        }
    } else {
        Logger::Debug("[TelemetryManager::Shutdown] Telemetry is disabled, skipping final snapshot");
    }

    // Shutdown reporters
    {
        std::lock_guard<std::mutex> lock(m_reporterMutex);
        Logger::Debug("[TelemetryManager::Shutdown] Shutting down %zu reporters", m_reporters.size());
        for (size_t i = 0; i < m_reporters.size(); ++i) {
            auto& reporter = m_reporters[i];
            try {
                Logger::Trace("[TelemetryManager::Shutdown] Shutting down reporter %zu", i);
                reporter->Shutdown();
                Logger::Debug("[TelemetryManager::Shutdown] Reporter %zu shut down successfully", i);
            } catch (const std::exception& ex) {
                Logger::Error("Error shutting down telemetry reporter: %s", ex.what());
                Logger::Error("[TelemetryManager::Shutdown] Exception shutting down reporter %zu: %s", i, ex.what());
            }
        }
        m_reporters.clear();
        Logger::Debug("[TelemetryManager::Shutdown] All reporters cleared");
    }

    // Clear snapshots
    {
        std::lock_guard<std::mutex> lock(m_snapshotMutex);
        Logger::Debug("[TelemetryManager::Shutdown] Clearing %zu stored snapshots", m_snapshots.size());
        m_snapshots.clear();
    }

    Logger::Info("TelemetryManager shutdown complete");
    Logger::Trace("[TelemetryManager::Shutdown] Exit");
}

void TelemetryManager::AddReporter(std::unique_ptr<MetricsReporter> reporter) {
    Logger::Trace("[TelemetryManager::AddReporter] Entry - reporter=%p", (void*)reporter.get());
    std::lock_guard<std::mutex> lock(m_reporterMutex);
    if (m_initialized.load()) {
        Logger::Debug("[TelemetryManager::AddReporter] Manager is initialized, initializing new reporter with directory '%s'", m_config.metricsDirectory.c_str());
        try {
            reporter->Initialize(m_config.metricsDirectory);
            Logger::Debug("[TelemetryManager::AddReporter] New reporter initialized successfully");
        } catch (const std::exception& ex) {
            Logger::Error("[TelemetryManager::AddReporter] Exception initializing new reporter: %s", ex.what());
            ReportError("Failed to initialize new reporter: " + std::string(ex.what()));
            Logger::Trace("[TelemetryManager::AddReporter] Exit - early return (reporter init failed)");
            return;
        }
    } else {
        Logger::Debug("[TelemetryManager::AddReporter] Manager not yet initialized, deferring reporter initialization");
    }
    m_reporters.push_back(std::move(reporter));
    Logger::Info("Added telemetry reporter");
    Logger::Debug("[TelemetryManager::AddReporter] Total reporters now: %zu", m_reporters.size());
    Logger::Trace("[TelemetryManager::AddReporter] Exit");
}

void TelemetryManager::RemoveAllReporters() {
    Logger::Trace("[TelemetryManager::RemoveAllReporters] Entry");
    std::lock_guard<std::mutex> lock(m_reporterMutex);
    Logger::Debug("[TelemetryManager::RemoveAllReporters] Shutting down and removing %zu reporters", m_reporters.size());
    for (size_t i = 0; i < m_reporters.size(); ++i) {
        auto& reporter = m_reporters[i];
        try {
            Logger::Trace("[TelemetryManager::RemoveAllReporters] Shutting down reporter %zu", i);
            reporter->Shutdown();
            Logger::Debug("[TelemetryManager::RemoveAllReporters] Reporter %zu shut down successfully", i);
        } catch (const std::exception& ex) {
            Logger::Error("Error shutting down telemetry reporter: %s", ex.what());
            Logger::Error("[TelemetryManager::RemoveAllReporters] Exception shutting down reporter %zu: %s", i, ex.what());
        }
    }
    m_reporters.clear();
    Logger::Info("Removed all telemetry reporters");
    Logger::Trace("[TelemetryManager::RemoveAllReporters] Exit");
}

void TelemetryManager::StartSampling() {
    Logger::Trace("[TelemetryManager::StartSampling] Entry");
    if (!m_config.enabled) {
        Logger::Info("Telemetry disabled, not starting sampling");
        Logger::Debug("[TelemetryManager::StartSampling] config.enabled is false, skipping sampling start");
        Logger::Trace("[TelemetryManager::StartSampling] Exit - early return (telemetry disabled)");
        return;
    }

    if (m_running.exchange(true)) {
        Logger::Warn("Telemetry sampling already running");
        Logger::Debug("[TelemetryManager::StartSampling] m_running was already true, skipping");
        Logger::Trace("[TelemetryManager::StartSampling] Exit - early return (already running)");
        return;
    }

    Logger::Info("Starting telemetry sampling (interval: %lldms)",
                m_config.samplingInterval.count());
    Logger::Debug("[TelemetryManager::StartSampling] Launching sampling thread");

    m_samplingThread = std::thread(&TelemetryManager::SamplingLoop, this);
    Logger::Debug("[TelemetryManager::StartSampling] Sampling thread launched successfully");
    Logger::Trace("[TelemetryManager::StartSampling] Exit");
}

void TelemetryManager::StopSampling() {
    Logger::Trace("[TelemetryManager::StopSampling] Entry");
    if (!m_running.exchange(false)) {
        Logger::Debug("[TelemetryManager::StopSampling] Sampling was not running, nothing to stop");
        Logger::Trace("[TelemetryManager::StopSampling] Exit - early return (was not running)");
        return;
    }

    Logger::Info("Stopping telemetry sampling...");

    if (m_samplingThread.joinable()) {
        Logger::Debug("[TelemetryManager::StopSampling] Waiting for sampling thread to join");
        m_samplingThread.join();
        Logger::Debug("[TelemetryManager::StopSampling] Sampling thread joined successfully");
    } else {
        Logger::Debug("[TelemetryManager::StopSampling] Sampling thread is not joinable");
    }

    Logger::Info("Telemetry sampling stopped");
    Logger::Trace("[TelemetryManager::StopSampling] Exit");
}

void TelemetryManager::ForceSample() {
    Logger::Trace("[TelemetryManager::ForceSample] Entry");
    if (!m_config.enabled) {
        Logger::Debug("[TelemetryManager::ForceSample] Telemetry disabled, skipping forced sample");
        Logger::Trace("[TelemetryManager::ForceSample] Exit - early return (disabled)");
        return;
    }

    try {
        Logger::Debug("[TelemetryManager::ForceSample] Collecting metrics snapshot");
        MetricsSnapshot snapshot = CollectSnapshot();
        Logger::Trace("[TelemetryManager::ForceSample] Snapshot collected successfully");

        // Store snapshot
        {
            std::lock_guard<std::mutex> lock(m_snapshotMutex);
            if (m_snapshots.size() < m_config.maxSamplesInMemory) {
                m_snapshots.push_back(snapshot);
                Logger::Trace("[TelemetryManager::ForceSample] Snapshot appended to storage (size now %zu/%zu)", m_snapshots.size(), m_config.maxSamplesInMemory);
            } else {
                m_snapshots[m_snapshotIndex] = snapshot;
                Logger::Trace("[TelemetryManager::ForceSample] Snapshot stored at circular buffer index %zu (buffer full, overwriting)", m_snapshotIndex);
                m_snapshotIndex = (m_snapshotIndex + 1) % m_config.maxSamplesInMemory;
                Logger::Trace("[TelemetryManager::ForceSample] Circular buffer index advanced to %zu", m_snapshotIndex);
            }
        }

        // Report to all reporters
        {
            std::lock_guard<std::mutex> lock(m_reporterMutex);
            Logger::Debug("[TelemetryManager::ForceSample] Reporting snapshot to %zu reporters", m_reporters.size());
            for (size_t i = 0; i < m_reporters.size(); ++i) {
                auto& reporter = m_reporters[i];
                try {
                    Logger::Trace("[TelemetryManager::ForceSample] Sending snapshot to reporter %zu", i);
                    reporter->Report(snapshot);
                    Logger::Trace("[TelemetryManager::ForceSample] Reporter %zu accepted snapshot successfully", i);
                } catch (const std::exception& ex) {
                    Logger::Error("[TelemetryManager::ForceSample] Reporter %zu failed with exception: %s", i, ex.what());
                    ReportError("Reporter failed: " + std::string(ex.what()));
                }
            }
        }

        auto totalNow = m_totalSamples.fetch_add(1, std::memory_order_relaxed) + 1;
        Logger::Debug("[TelemetryManager::ForceSample] Total samples collected: %llu", (unsigned long long)totalNow);
        Logger::Info("[TelemetryManager::ForceSample] Metrics snapshot #%llu collected and reported successfully", (unsigned long long)totalNow);

    } catch (const std::exception& ex) {
        Logger::Error("[TelemetryManager::ForceSample] Exception during snapshot collection: %s", ex.what());
        ReportError("Failed to collect metrics snapshot: " + std::string(ex.what()));
    }
    Logger::Trace("[TelemetryManager::ForceSample] Exit");
}

void TelemetryManager::SamplingLoop() {
    Logger::Trace("[TelemetryManager::SamplingLoop] Entry - sampling interval: %lldms", m_config.samplingInterval.count());
    Logger::Info("Telemetry sampling loop started");

    uint64_t iterationCount = 0;
    while (m_running.load()) {
        auto loopStart = std::chrono::steady_clock::now();
        iterationCount++;
        Logger::Trace("[TelemetryManager::SamplingLoop] Iteration %llu starting", (unsigned long long)iterationCount);

        try {
            ForceSample();
        } catch (const std::exception& ex) {
            Logger::Error("[TelemetryManager::SamplingLoop] Exception during iteration %llu: %s", (unsigned long long)iterationCount, ex.what());
            ReportError("Sampling loop error: " + std::string(ex.what()));
        } catch (...) {
            // A non-std exception must not escape this thread function into
            // std::terminate. Record it and keep sampling.
            Logger::Error("[TelemetryManager::SamplingLoop] Non-std exception during iteration %llu", (unsigned long long)iterationCount);
            ReportError("Sampling loop error: non-std exception");
        }

        // Calculate sleep time to maintain consistent interval
        auto loopEnd = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(loopEnd - loopStart);
        auto sleepTime = m_config.samplingInterval - elapsed;

        if (sleepTime > std::chrono::milliseconds(0)) {
            Logger::Trace("[TelemetryManager::SamplingLoop] Iteration %llu took %lldms, sleeping for %lldms", (unsigned long long)iterationCount, elapsed.count(), sleepTime.count());
            std::this_thread::sleep_for(sleepTime);
        } else if (elapsed > m_config.samplingInterval * 2) {
            // Warn if sampling is taking too long
            Logger::Warn("Telemetry sampling took %lldms (interval: %lldms)",
                        elapsed.count(), m_config.samplingInterval.count());
            Logger::Debug("[TelemetryManager::SamplingLoop] Iteration %llu exceeded 2x interval: elapsed=%lldms, interval=%lldms", (unsigned long long)iterationCount, elapsed.count(), m_config.samplingInterval.count());
        } else {
            Logger::Debug("[TelemetryManager::SamplingLoop] Iteration %llu took %lldms, no sleep needed (exceeded interval)", (unsigned long long)iterationCount, elapsed.count());
        }
    }

    Logger::Info("Telemetry sampling loop stopped");
    Logger::Debug("[TelemetryManager::SamplingLoop] Completed %llu iterations total", (unsigned long long)iterationCount);
    Logger::Trace("[TelemetryManager::SamplingLoop] Exit");
}

MetricsSnapshot TelemetryManager::CollectSnapshot() {
    Logger::Trace("[TelemetryManager::CollectSnapshot] Entry");
    MetricsSnapshot snapshot;
    snapshot.timestamp = std::chrono::system_clock::now();
    Logger::Trace("[TelemetryManager::CollectSnapshot] Timestamp set for snapshot");

    try {
        if (m_config.enableSystemMetrics) {
            Logger::Debug("[TelemetryManager::CollectSnapshot] System metrics collection enabled, collecting system metrics");
            CollectSystemMetrics(snapshot);
            Logger::Debug("[TelemetryManager::CollectSnapshot] System metrics collected successfully");
        } else {
            Logger::Debug("[TelemetryManager::CollectSnapshot] System metrics collection is disabled, skipping");
        }

        if (m_config.enableApplicationMetrics) {
            Logger::Debug("[TelemetryManager::CollectSnapshot] Application metrics collection enabled, collecting application metrics");
            CollectApplicationMetrics(snapshot);
            Logger::Debug("[TelemetryManager::CollectSnapshot] Application metrics collected successfully");
        } else {
            Logger::Debug("[TelemetryManager::CollectSnapshot] Application metrics collection is disabled, skipping");
        }

    } catch (const std::exception& ex) {
        Logger::Error("[TelemetryManager::CollectSnapshot] Exception during metrics collection: %s", ex.what());
        ReportError("Error collecting metrics: " + std::string(ex.what()));
    }

    Logger::Trace("[TelemetryManager::CollectSnapshot] Exit - returning snapshot");
    return snapshot;
}

void TelemetryManager::CollectSystemMetrics(MetricsSnapshot& snapshot) {
    Logger::Trace("[TelemetryManager::CollectSystemMetrics] Entry");

    // CPU Usage
    snapshot.cpuUsagePercent = GetCPUUsage();
    Logger::Trace("[TelemetryManager::CollectSystemMetrics] CPU usage: %.2f%%", snapshot.cpuUsagePercent);

    // Memory Usage
    auto [memUsed, memTotal] = GetMemoryUsage();
    snapshot.memoryUsedBytes = memUsed;
    snapshot.memoryTotalBytes = memTotal;
    Logger::Trace("[TelemetryManager::CollectSystemMetrics] Memory: used=%llu bytes, total=%llu bytes", (unsigned long long)memUsed, (unsigned long long)memTotal);

    // Network Stats
    auto [netSent, netRecv] = GetNetworkStats();
    snapshot.networkBytesSent = netSent;
    snapshot.networkBytesReceived = netRecv;
    Logger::Trace("[TelemetryManager::CollectSystemMetrics] Network: sent=%llu bytes, received=%llu bytes", (unsigned long long)netSent, (unsigned long long)netRecv);

    // Disk Stats
    auto [diskRead, diskWrite] = GetDiskStats();
    snapshot.diskReadBytes = diskRead;
    snapshot.diskWriteBytes = diskWrite;
    Logger::Trace("[TelemetryManager::CollectSystemMetrics] Disk: read=%llu bytes, write=%llu bytes", (unsigned long long)diskRead, (unsigned long long)diskWrite);

    Logger::Debug("[TelemetryManager::CollectSystemMetrics] All system metrics collected: cpu=%.2f%%, memUsed=%llu, memTotal=%llu, netSent=%llu, netRecv=%llu, diskRead=%llu, diskWrite=%llu",
                 snapshot.cpuUsagePercent, (unsigned long long)memUsed, (unsigned long long)memTotal,
                 (unsigned long long)netSent, (unsigned long long)netRecv,
                 (unsigned long long)diskRead, (unsigned long long)diskWrite);
    Logger::Trace("[TelemetryManager::CollectSystemMetrics] Exit");
}

void TelemetryManager::CollectApplicationMetrics(MetricsSnapshot& snapshot) {
    Logger::Trace("[TelemetryManager::CollectApplicationMetrics] Entry");

    // Copy atomic values to snapshot
    snapshot.activeConnections = m_customMetrics.activeConnections.load(std::memory_order_relaxed);
    snapshot.authenticatedPlayers = m_customMetrics.authenticatedPlayers.load(std::memory_order_relaxed);
    snapshot.totalPacketsProcessed = m_customMetrics.totalPacketsProcessed.load(std::memory_order_relaxed);
    snapshot.totalPacketsDropped = m_customMetrics.totalPacketsDropped.load(std::memory_order_relaxed);
    snapshot.currentTick = m_customMetrics.currentTick.load(std::memory_order_relaxed);
    snapshot.averageLatencyMs = m_customMetrics.averageLatencyMs.load(std::memory_order_relaxed);
    snapshot.packetLossRate = m_customMetrics.packetLossRate.load(std::memory_order_relaxed);

    Logger::Trace("[TelemetryManager::CollectApplicationMetrics] Network metrics: activeConnections=%llu, authenticatedPlayers=%llu, totalPacketsProcessed=%llu, totalPacketsDropped=%llu, currentTick=%llu, avgLatency=%.2fms, packetLossRate=%.4f",
                 (unsigned long long)snapshot.activeConnections, (unsigned long long)snapshot.authenticatedPlayers,
                 (unsigned long long)snapshot.totalPacketsProcessed, (unsigned long long)snapshot.totalPacketsDropped,
                 (unsigned long long)snapshot.currentTick, snapshot.averageLatencyMs, snapshot.packetLossRate);

    snapshot.activeMatches = m_customMetrics.activeMatches.load(std::memory_order_relaxed);
    snapshot.totalKills = m_customMetrics.totalKills.load(std::memory_order_relaxed);
    snapshot.totalDeaths = m_customMetrics.totalDeaths.load(std::memory_order_relaxed);
    snapshot.objectivesCaptured = m_customMetrics.objectivesCaptured.load(std::memory_order_relaxed);
    snapshot.chatMessagesSent = m_customMetrics.chatMessagesSent.load(std::memory_order_relaxed);

    Logger::Trace("[TelemetryManager::CollectApplicationMetrics] Gameplay metrics: activeMatches=%llu, totalKills=%llu, totalDeaths=%llu, objectivesCaptured=%llu, chatMessages=%llu",
                 (unsigned long long)snapshot.activeMatches, (unsigned long long)snapshot.totalKills, (unsigned long long)snapshot.totalDeaths,
                 (unsigned long long)snapshot.objectivesCaptured, (unsigned long long)snapshot.chatMessagesSent);

    snapshot.frameTimeMs = m_customMetrics.frameTimeMs.load(std::memory_order_relaxed);
    snapshot.physicsTimeMs = m_customMetrics.physicsTimeMs.load(std::memory_order_relaxed);
    snapshot.networkTimeMs = m_customMetrics.networkTimeMs.load(std::memory_order_relaxed);
    snapshot.gameLogicTimeMs = m_customMetrics.gameLogicTimeMs.load(std::memory_order_relaxed);

    Logger::Trace("[TelemetryManager::CollectApplicationMetrics] Performance metrics: frameTime=%.2fms, physicsTime=%.2fms, networkTime=%.2fms, gameLogicTime=%.2fms",
                 snapshot.frameTimeMs, snapshot.physicsTimeMs, snapshot.networkTimeMs, snapshot.gameLogicTimeMs);

    snapshot.securityViolations = m_customMetrics.securityViolations.load(std::memory_order_relaxed);
    snapshot.malformedPackets = m_customMetrics.malformedPackets.load(std::memory_order_relaxed);
    snapshot.speedHackDetections = m_customMetrics.speedHackDetections.load(std::memory_order_relaxed);
    snapshot.kickedPlayers = m_customMetrics.kickedPlayers.load(std::memory_order_relaxed);
    snapshot.bannedPlayers = m_customMetrics.bannedPlayers.load(std::memory_order_relaxed);

    Logger::Trace("[TelemetryManager::CollectApplicationMetrics] Security metrics: violations=%llu, malformedPackets=%llu, speedHackDetections=%llu, kicked=%llu, banned=%llu",
                 (unsigned long long)snapshot.securityViolations, (unsigned long long)snapshot.malformedPackets,
                 (unsigned long long)snapshot.speedHackDetections, (unsigned long long)snapshot.kickedPlayers,
                 (unsigned long long)snapshot.bannedPlayers);

    Logger::Debug("[TelemetryManager::CollectApplicationMetrics] All application metrics collected successfully");
    Logger::Trace("[TelemetryManager::CollectApplicationMetrics] Exit");
}

double TelemetryManager::GetCPUUsage() {
    Logger::Trace("[TelemetryManager::GetCPUUsage] Entry");
#ifdef __linux__
    Logger::Trace("[TelemetryManager::GetCPUUsage] Platform: Linux, reading /proc/stat");
    std::ifstream stat("/proc/stat");
    if (!stat.is_open()) {
        Logger::Warn("[TelemetryManager::GetCPUUsage] Failed to open /proc/stat");
        Logger::Trace("[TelemetryManager::GetCPUUsage] Exit - returning 0.0 (cannot open /proc/stat)");
        return 0.0;
    }

    CPUTimes current;
    std::string cpu;
    stat >> cpu >> current.user >> current.nice >> current.system >> current.idle
         >> current.iowait >> current.irq >> current.softirq >> current.steal;
    stat.close();
    Logger::Trace("[TelemetryManager::GetCPUUsage] Read CPU times: user=%llu, nice=%llu, system=%llu, idle=%llu, iowait=%llu, irq=%llu, softirq=%llu, steal=%llu",
                 (unsigned long long)current.user, (unsigned long long)current.nice, (unsigned long long)current.system,
                 (unsigned long long)current.idle, (unsigned long long)current.iowait, (unsigned long long)current.irq,
                 (unsigned long long)current.softirq, (unsigned long long)current.steal);

    if (!m_hasPreviousCPUTimes) {
        Logger::Debug("[TelemetryManager::GetCPUUsage] No previous CPU times available, storing baseline and returning 0.0");
        m_lastCPUTimes = current;
        m_hasPreviousCPUTimes = true;
        Logger::Trace("[TelemetryManager::GetCPUUsage] Exit - returning 0.0 (first sample baseline)");
        return 0.0;
    }

    uint64_t totalDelta = (current.user + current.nice + current.system + current.idle +
                          current.iowait + current.irq + current.softirq + current.steal) -
                         (m_lastCPUTimes.user + m_lastCPUTimes.nice + m_lastCPUTimes.system +
                          m_lastCPUTimes.idle + m_lastCPUTimes.iowait + m_lastCPUTimes.irq +
                          m_lastCPUTimes.softirq + m_lastCPUTimes.steal);

    uint64_t idleDelta = current.idle - m_lastCPUTimes.idle;
    Logger::Trace("[TelemetryManager::GetCPUUsage] Computed deltas: totalDelta=%llu, idleDelta=%llu", (unsigned long long)totalDelta, (unsigned long long)idleDelta);

    m_lastCPUTimes = current;

    if (totalDelta == 0) {
        Logger::Debug("[TelemetryManager::GetCPUUsage] Total delta is zero, no CPU activity detected");
        Logger::Trace("[TelemetryManager::GetCPUUsage] Exit - returning 0.0 (zero delta)");
        return 0.0;
    }

    double cpuUsage = 100.0 * (totalDelta - idleDelta) / totalDelta;
    Logger::Trace("[TelemetryManager::GetCPUUsage] Exit - returning %.2f%%", cpuUsage);
    return cpuUsage;

#elif defined(_WIN32)
    Logger::Trace("[TelemetryManager::GetCPUUsage] Platform: Windows, using PDH counters");
    static PDH_HQUERY query = nullptr;
    static PDH_HCOUNTER counter = nullptr;
    static bool initialized = false;

    if (!initialized) {
        Logger::Debug("[TelemetryManager::GetCPUUsage] PDH not yet initialized, initializing CPU counter");
        if (PdhOpenQuery(nullptr, 0, &query) == ERROR_SUCCESS) {
            if (PdhAddEnglishCounter(query, "\\Processor(_Total)\\% Processor Time", 0, &counter) == ERROR_SUCCESS) {
                PdhCollectQueryData(query);
                initialized = true;
                Logger::Debug("[TelemetryManager::GetCPUUsage] PDH CPU counter initialized successfully");
            } else {
                Logger::Error("[TelemetryManager::GetCPUUsage] Failed to add PDH CPU counter");
            }
        } else {
            Logger::Error("[TelemetryManager::GetCPUUsage] Failed to open PDH query");
        }
    }

    if (initialized) {
        PdhCollectQueryData(query);
        PDH_FMT_COUNTERVALUE value;
        if (PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, nullptr, &value) == ERROR_SUCCESS) {
            Logger::Trace("[TelemetryManager::GetCPUUsage] Exit - returning %.2f%% (Windows PDH)", value.doubleValue);
            return value.doubleValue;
        } else {
            Logger::Warn("[TelemetryManager::GetCPUUsage] PdhGetFormattedCounterValue failed");
        }
    }

    Logger::Trace("[TelemetryManager::GetCPUUsage] Exit - returning 0.0 (Windows fallback)");
    return 0.0;

#else
    // macOS or other platforms - simplified implementation
    Logger::Debug("[TelemetryManager::GetCPUUsage] Platform not supported for CPU metrics, returning 0.0");
    Logger::Trace("[TelemetryManager::GetCPUUsage] Exit - returning 0.0 (unsupported platform)");
    return 0.0;
#endif
}

std::pair<uint64_t, uint64_t> TelemetryManager::GetMemoryUsage() {
    Logger::Trace("[TelemetryManager::GetMemoryUsage] Entry");
#ifdef __linux__
    Logger::Trace("[TelemetryManager::GetMemoryUsage] Platform: Linux, reading /proc/meminfo");
    std::ifstream meminfo("/proc/meminfo");
    if (!meminfo.is_open()) {
        Logger::Warn("[TelemetryManager::GetMemoryUsage] Failed to open /proc/meminfo");
        Logger::Trace("[TelemetryManager::GetMemoryUsage] Exit - returning {0, 0} (cannot open /proc/meminfo)");
        return {0, 0};
    }

    uint64_t memTotal = 0, memFree = 0, buffers = 0, cached = 0;
    std::string line;

    while (std::getline(meminfo, line)) {
        std::istringstream iss(line);
        std::string key;
        uint64_t value;
        std::string unit;

        if (iss >> key >> value >> unit) {
            if (key == "MemTotal:") memTotal = value * 1024;
            else if (key == "MemFree:") memFree = value * 1024;
            else if (key == "Buffers:") buffers = value * 1024;
            else if (key == "Cached:") cached = value * 1024;
        }
    }
    meminfo.close();

    uint64_t memUsed = memTotal - memFree - buffers - cached;
    Logger::Trace("[TelemetryManager::GetMemoryUsage] Parsed /proc/meminfo: total=%llu, free=%llu, buffers=%llu, cached=%llu, used=%llu",
                 (unsigned long long)memTotal, (unsigned long long)memFree, (unsigned long long)buffers,
                 (unsigned long long)cached, (unsigned long long)memUsed);
    Logger::Trace("[TelemetryManager::GetMemoryUsage] Exit - returning {%llu, %llu}", (unsigned long long)memUsed, (unsigned long long)memTotal);
    return {memUsed, memTotal};

#elif defined(_WIN32)
    Logger::Trace("[TelemetryManager::GetMemoryUsage] Platform: Windows, using GlobalMemoryStatusEx");
    MEMORYSTATUSEX memStatus;
    memStatus.dwLength = sizeof(memStatus);
    if (GlobalMemoryStatusEx(&memStatus)) {
        uint64_t memUsed = memStatus.ullTotalPhys - memStatus.ullAvailPhys;
        Logger::Trace("[TelemetryManager::GetMemoryUsage] Exit - returning {%llu, %llu} (Windows)", (unsigned long long)memUsed, (unsigned long long)memStatus.ullTotalPhys);
        return {memUsed, memStatus.ullTotalPhys};
    }
    Logger::Warn("[TelemetryManager::GetMemoryUsage] GlobalMemoryStatusEx failed");
    Logger::Trace("[TelemetryManager::GetMemoryUsage] Exit - returning {0, 0} (Windows API failed)");
    return {0, 0};

#else
    Logger::Debug("[TelemetryManager::GetMemoryUsage] Platform not supported for memory metrics, returning {0, 0}");
    Logger::Trace("[TelemetryManager::GetMemoryUsage] Exit - returning {0, 0} (unsupported platform)");
    return {0, 0};
#endif
}

std::pair<uint64_t, uint64_t> TelemetryManager::GetNetworkStats() {
    Logger::Trace("[TelemetryManager::GetNetworkStats] Entry");
#ifdef __linux__
    Logger::Trace("[TelemetryManager::GetNetworkStats] Platform: Linux, reading /proc/net/dev");
    std::ifstream netdev("/proc/net/dev");
    if (!netdev.is_open()) {
        Logger::Warn("[TelemetryManager::GetNetworkStats] Failed to open /proc/net/dev");
        Logger::Trace("[TelemetryManager::GetNetworkStats] Exit - returning {0, 0} (cannot open /proc/net/dev)");
        return {0, 0};
    }

    uint64_t totalSent = 0, totalReceived = 0;
    std::string line;

    // Skip header lines
    std::getline(netdev, line);
    std::getline(netdev, line);
    Logger::Trace("[TelemetryManager::GetNetworkStats] Skipped 2 header lines from /proc/net/dev");

    int interfaceCount = 0;
    while (std::getline(netdev, line)) {
        size_t colonPos = line.find(':');
        if (colonPos == std::string::npos) {
            Logger::Trace("[TelemetryManager::GetNetworkStats] Skipping line without colon separator");
            continue;
        }

        std::string interface = line.substr(0, colonPos);
        // Trim whitespace
        interface.erase(0, interface.find_first_not_of(" \t"));
        interface.erase(interface.find_last_not_of(" \t") + 1);

        // Skip loopback interface
        if (interface == "lo") {
            Logger::Trace("[TelemetryManager::GetNetworkStats] Skipping loopback interface 'lo'");
            continue;
        }

        std::istringstream iss(line.substr(colonPos + 1));
        uint64_t recvBytes, recvPackets, recvErrs, recvDrop, recvFifo, recvFrame, recvCompressed, recvMulticast;
        uint64_t transBytes, transPackets, transErrs, transDrop, transFifo, transColls, transCarrier, transCompressed;

        if (iss >> recvBytes >> recvPackets >> recvErrs >> recvDrop >> recvFifo >> recvFrame >> recvCompressed >> recvMulticast
               >> transBytes >> transPackets >> transErrs >> transDrop >> transFifo >> transColls >> transCarrier >> transCompressed) {
            totalReceived += recvBytes;
            totalSent += transBytes;
            interfaceCount++;
            Logger::Trace("[TelemetryManager::GetNetworkStats] Interface '%s': recv=%llu bytes, sent=%llu bytes", interface.c_str(), (unsigned long long)recvBytes, (unsigned long long)transBytes);
        } else {
            Logger::Warn("[TelemetryManager::GetNetworkStats] Failed to parse stats for interface '%s'", interface.c_str());
        }
    }
    netdev.close();

    Logger::Debug("[TelemetryManager::GetNetworkStats] Parsed %d network interfaces: totalSent=%llu, totalReceived=%llu", interfaceCount, (unsigned long long)totalSent, (unsigned long long)totalReceived);
    Logger::Trace("[TelemetryManager::GetNetworkStats] Exit - returning {%llu, %llu}", (unsigned long long)totalSent, (unsigned long long)totalReceived);
    return {totalSent, totalReceived};

#elif defined(_WIN32)
    // Windows implementation would use GetIfTable2 or similar
    Logger::Debug("[TelemetryManager::GetNetworkStats] Windows network stats not implemented, returning {0, 0}");
    Logger::Trace("[TelemetryManager::GetNetworkStats] Exit - returning {0, 0} (Windows not implemented)");
    return {0, 0};

#else
    Logger::Debug("[TelemetryManager::GetNetworkStats] Platform not supported for network metrics, returning {0, 0}");
    Logger::Trace("[TelemetryManager::GetNetworkStats] Exit - returning {0, 0} (unsupported platform)");
    return {0, 0};
#endif
}

std::pair<uint64_t, uint64_t> TelemetryManager::GetDiskStats() {
    Logger::Trace("[TelemetryManager::GetDiskStats] Entry");
#ifdef __linux__
    Logger::Trace("[TelemetryManager::GetDiskStats] Platform: Linux, reading /proc/diskstats");
    std::ifstream diskstats("/proc/diskstats");
    if (!diskstats.is_open()) {
        Logger::Warn("[TelemetryManager::GetDiskStats] Failed to open /proc/diskstats");
        Logger::Trace("[TelemetryManager::GetDiskStats] Exit - returning {0, 0} (cannot open /proc/diskstats)");
        return {0, 0};
    }

    uint64_t totalRead = 0, totalWrite = 0;
    std::string line;
    int deviceCount = 0;

    while (std::getline(diskstats, line)) {
        std::istringstream iss(line);
        int major, minor;
        std::string device;
        uint64_t readIOs, readMerges, readSectors, readTicks;
        uint64_t writeIOs, writeMerges, writeSectors, writeTicks;
        uint64_t inFlight, ioTicks, timeInQueue;

        if (iss >> major >> minor >> device >> readIOs >> readMerges >> readSectors >> readTicks
               >> writeIOs >> writeMerges >> writeSectors >> writeTicks >> inFlight >> ioTicks >> timeInQueue) {
            // Only count main disk devices (not partitions)
            if (device.find_first_of("0123456789") == std::string::npos) {
                totalRead += readSectors * 512;  // sectors are 512 bytes
                totalWrite += writeSectors * 512;
                deviceCount++;
                Logger::Trace("[TelemetryManager::GetDiskStats] Device '%s': readSectors=%llu (%llu bytes), writeSectors=%llu (%llu bytes)",
                             device.c_str(), (unsigned long long)readSectors, (unsigned long long)(readSectors * 512),
                             (unsigned long long)writeSectors, (unsigned long long)(writeSectors * 512));
            } else {
                Logger::Trace("[TelemetryManager::GetDiskStats] Skipping partition device '%s'", device.c_str());
            }
        }
    }
    diskstats.close();

    Logger::Debug("[TelemetryManager::GetDiskStats] Parsed %d disk devices: totalRead=%llu bytes, totalWrite=%llu bytes", deviceCount, (unsigned long long)totalRead, (unsigned long long)totalWrite);
    Logger::Trace("[TelemetryManager::GetDiskStats] Exit - returning {%llu, %llu}", (unsigned long long)totalRead, (unsigned long long)totalWrite);
    return {totalRead, totalWrite};

#else
    Logger::Debug("[TelemetryManager::GetDiskStats] Platform not supported for disk metrics, returning {0, 0}");
    Logger::Trace("[TelemetryManager::GetDiskStats] Exit - returning {0, 0} (unsupported platform)");
    return {0, 0};
#endif
}

MetricsSnapshot TelemetryManager::GetLatestSnapshot() const {
    Logger::Trace("[TelemetryManager::GetLatestSnapshot] Entry");
    std::lock_guard<std::mutex> lock(m_snapshotMutex);
    if (m_snapshots.empty()) {
        Logger::Debug("[TelemetryManager::GetLatestSnapshot] No snapshots available, returning default empty snapshot");
        Logger::Trace("[TelemetryManager::GetLatestSnapshot] Exit - returning default MetricsSnapshot (empty)");
        return MetricsSnapshot{};
    }

    if (m_snapshots.size() < m_config.maxSamplesInMemory) {
        Logger::Debug("[TelemetryManager::GetLatestSnapshot] Linear storage mode, returning last snapshot (index %zu of %zu)", m_snapshots.size() - 1, m_snapshots.size());
        Logger::Trace("[TelemetryManager::GetLatestSnapshot] Exit - returning snapshot from back of linear storage");
        return m_snapshots.back();
    } else {
        size_t latest = (m_snapshotIndex + m_config.maxSamplesInMemory - 1) % m_config.maxSamplesInMemory;
        Logger::Debug("[TelemetryManager::GetLatestSnapshot] Circular buffer mode, returning snapshot at index %zu (snapshotIndex=%zu, maxSamples=%zu)", latest, m_snapshotIndex, m_config.maxSamplesInMemory);
        Logger::Trace("[TelemetryManager::GetLatestSnapshot] Exit - returning snapshot from circular buffer index %zu", latest);
        return m_snapshots[latest];
    }
}

std::vector<MetricsSnapshot> TelemetryManager::GetRecentSnapshots(size_t count) const {
    Logger::Trace("[TelemetryManager::GetRecentSnapshots] Entry - count=%zu", count);
    std::lock_guard<std::mutex> lock(m_snapshotMutex);
    std::vector<MetricsSnapshot> result;

    if (m_snapshots.empty()) {
        Logger::Debug("[TelemetryManager::GetRecentSnapshots] No snapshots available, returning empty vector");
        Logger::Trace("[TelemetryManager::GetRecentSnapshots] Exit - returning empty vector");
        return result;
    }

    size_t available = std::min(count, m_snapshots.size());
    result.reserve(available);
    Logger::Debug("[TelemetryManager::GetRecentSnapshots] Requested %zu snapshots, %zu available, returning %zu", count, m_snapshots.size(), available);

    if (m_snapshots.size() < m_config.maxSamplesInMemory) {
        // Linear storage
        size_t start = m_snapshots.size() >= available ? m_snapshots.size() - available : 0;
        Logger::Debug("[TelemetryManager::GetRecentSnapshots] Using linear storage mode, reading from index %zu to %zu", start, m_snapshots.size() - 1);
        for (size_t i = start; i < m_snapshots.size(); ++i) {
            result.push_back(m_snapshots[i]);
        }
    } else {
        // Circular buffer
        Logger::Debug("[TelemetryManager::GetRecentSnapshots] Using circular buffer mode, snapshotIndex=%zu, maxSamples=%zu", m_snapshotIndex, m_config.maxSamplesInMemory);
        for (size_t i = 0; i < available; ++i) {
            size_t index = (m_snapshotIndex + m_config.maxSamplesInMemory - available + i) % m_config.maxSamplesInMemory;
            Logger::Trace("[TelemetryManager::GetRecentSnapshots] Reading circular buffer at index %zu", index);
            result.push_back(m_snapshots[index]);
        }
    }

    Logger::Trace("[TelemetryManager::GetRecentSnapshots] Exit - returning %zu snapshots", result.size());
    return result;
}

void TelemetryManager::UpdateConfig(const TelemetryConfig& config) {
    Logger::Trace("[TelemetryManager::UpdateConfig] Entry - config.enabled=%d, config.samplingInterval=%lldms, config.enableSystemMetrics=%d, config.enableApplicationMetrics=%d",
                 config.enabled, config.samplingInterval.count(), config.enableSystemMetrics, config.enableApplicationMetrics);
    bool needsRestart = (m_config.samplingInterval != config.samplingInterval) && m_running.load();
    Logger::Debug("[TelemetryManager::UpdateConfig] Sampling interval change: %lldms -> %lldms, running=%d, needsRestart=%d",
                 m_config.samplingInterval.count(), config.samplingInterval.count(), m_running.load(), needsRestart);

    m_config = config;
    Logger::Debug("[TelemetryManager::UpdateConfig] Configuration stored");

    if (needsRestart) {
        Logger::Info("[TelemetryManager::UpdateConfig] Sampling interval changed while running, restarting sampling");
        StopSampling();
        StartSampling();
        Logger::Debug("[TelemetryManager::UpdateConfig] Sampling restarted with new interval");
    } else {
        Logger::Debug("[TelemetryManager::UpdateConfig] No sampling restart needed");
    }

    Logger::Info("Telemetry configuration updated");
    Logger::Trace("[TelemetryManager::UpdateConfig] Exit");
}

std::vector<std::string> TelemetryManager::GetLastErrors() const {
    Logger::Trace("[TelemetryManager::GetLastErrors] Entry");
    std::lock_guard<std::mutex> lock(m_errorMutex);
    Logger::Debug("[TelemetryManager::GetLastErrors] Returning %zu stored errors", m_errors.size());
    Logger::Trace("[TelemetryManager::GetLastErrors] Exit - returning %zu errors", m_errors.size());
    return m_errors;
}

void TelemetryManager::ClearErrors() {
    Logger::Trace("[TelemetryManager::ClearErrors] Entry");
    std::lock_guard<std::mutex> lock(m_errorMutex);
    Logger::Debug("[TelemetryManager::ClearErrors] Clearing %zu stored errors", m_errors.size());
    m_errors.clear();
    Logger::Info("[TelemetryManager::ClearErrors] Error history cleared");
    Logger::Trace("[TelemetryManager::ClearErrors] Exit");
}

void TelemetryManager::ReportError(const std::string& error) {
    Logger::Trace("[TelemetryManager::ReportError] Entry - error='%s'", error.c_str());
    std::lock_guard<std::mutex> lock(m_errorMutex);

    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);

    std::ostringstream oss;
    oss << "[" << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S") << "] " << error;

    m_errors.push_back(oss.str());
    Logger::Debug("[TelemetryManager::ReportError] Error recorded, total errors in history: %zu", m_errors.size());

    // Limit error history
    if (m_errors.size() > MAX_ERRORS) {
        Logger::Debug("[TelemetryManager::ReportError] Error history exceeded MAX_ERRORS (%zu), removing oldest entry", (size_t)MAX_ERRORS);
        m_errors.erase(m_errors.begin());
    }

    Logger::Error("Telemetry: %s", error.c_str());
    Logger::Trace("[TelemetryManager::ReportError] Exit");
}

} // namespace Telemetry