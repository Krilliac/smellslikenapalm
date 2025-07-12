// Server/telemetry/TelemetryManager.cpp
// Implementation of comprehensive telemetry system for the RS2V server

#include "TelemetryManager.h"
#include "MetricsReporter.h"
#include "../Utils/Logger.h"

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
    static TelemetryManager instance;
    return instance;
}

TelemetryManager::~TelemetryManager() {
    Shutdown();
}

bool TelemetryManager::Initialize(const TelemetryConfig& config) {
    if (m_initialized.exchange(true)) {
        Logger::Warn("TelemetryManager already initialized");
        return true;
    }
    
    Logger::Info("Initializing TelemetryManager...");
    
    m_config = config;
    m_startTime = std::chrono::steady_clock::now();
    
    // Create metrics directory if enabled
    if (m_config.enableFileReporter) {
        try {
            std::filesystem::create_directories(m_config.metricsDirectory);
            Logger::Info("Created telemetry directory: %s", m_config.metricsDirectory.c_str());
        } catch (const std::exception& ex) {
            ReportError("Failed to create metrics directory: " + std::string(ex.what()));
            return false;
        }
    }
    
    // Initialize snapshot storage
    {
        std::lock_guard<std::mutex> lock(m_snapshotMutex);
        m_snapshots.clear();
        m_snapshots.reserve(m_config.maxSamplesInMemory);
        m_snapshotIndex = 0;
    }
    
    // Initialize reporters
    {
        std::lock_guard<std::mutex> lock(m_reporterMutex);
        for (auto& reporter : m_reporters) {
            try {
                reporter->Initialize(m_config.metricsDirectory);
                Logger::Info("Initialized telemetry reporter");
            } catch (const std::exception& ex) {
                ReportError("Failed to initialize reporter: " + std::string(ex.what()));
            }
        }
    }
    
    Logger::Info("TelemetryManager initialized successfully");
    return true;
}

void TelemetryManager::Shutdown() {
    if (!m_initialized.exchange(false)) {
        return;
    }
    
    Logger::Info("Shutting down TelemetryManager...");
    
    // Stop sampling
    StopSampling();
    
    // Take final snapshot
    if (m_config.enabled) {
        try {
            ForceSample();
        } catch (const std::exception& ex) {
            Logger::Error("Failed to take final telemetry sample: %s", ex.what());
        }
    }
    
    // Shutdown reporters
    {
        std::lock_guard<std::mutex> lock(m_reporterMutex);
        for (auto& reporter : m_reporters) {
            try {
                reporter->Shutdown();
            } catch (const std::exception& ex) {
                Logger::Error("Error shutting down telemetry reporter: %s", ex.what());
            }
        }
        m_reporters.clear();
    }
    
    // Clear snapshots
    {
        std::lock_guard<std::mutex> lock(m_snapshotMutex);
        m_snapshots.clear();
    }
    
    Logger::Info("TelemetryManager shutdown complete");
}

void TelemetryManager::AddReporter(std::unique_ptr<MetricsReporter> reporter) {
    std::lock_guard<std::mutex> lock(m_reporterMutex);
    if (m_initialized.load()) {
        try {
            reporter->Initialize(m_config.metricsDirectory);
        } catch (const std::exception& ex) {
            ReportError("Failed to initialize new reporter: " + std::string(ex.what()));
            return;
        }
    }
    m_reporters.push_back(std::move(reporter));
    Logger::Info("Added telemetry reporter");
}

void TelemetryManager::RemoveAllReporters() {
    std::lock_guard<std::mutex> lock(m_reporterMutex);
    for (auto& reporter : m_reporters) {
        try {
            reporter->Shutdown();
        } catch (const std::exception& ex) {
            Logger::Error("Error shutting down telemetry reporter: %s", ex.what());
        }
    }
    m_reporters.clear();
    Logger::Info("Removed all telemetry reporters");
}

void TelemetryManager::StartSampling() {
    if (!m_config.enabled) {
        Logger::Info("Telemetry disabled, not starting sampling");
        return;
    }
    
    if (m_running.exchange(true)) {
        Logger::Warn("Telemetry sampling already running");
        return;
    }
    
    Logger::Info("Starting telemetry sampling (interval: %lldms)", 
                m_config.samplingInterval.count());
    
    m_samplingThread = std::thread(&TelemetryManager::SamplingLoop, this);
}

void TelemetryManager::StopSampling() {
    if (!m_running.exchange(false)) {
        return;
    }
    
    Logger::Info("Stopping telemetry sampling...");
    
    if (m_samplingThread.joinable()) {
        m_samplingThread.join();
    }
    
    Logger::Info("Telemetry sampling stopped");
}

void TelemetryManager::ForceSample() {
    if (!m_config.enabled) {
        return;
    }
    
    try {
        MetricsSnapshot snapshot = CollectSnapshot();
        
        // Store snapshot
        {
            std::lock_guard<std::mutex> lock(m_snapshotMutex);
            if (m_snapshots.size() < m_config.maxSamplesInMemory) {
                m_snapshots.push_back(snapshot);
            } else {
                m_snapshots[m_snapshotIndex] = snapshot;
                m_snapshotIndex = (m_snapshotIndex + 1) % m_config.maxSamplesInMemory;
            }
        }
        
        // Report to all reporters
        {
            std::lock_guard<std::mutex> lock(m_reporterMutex);
            for (auto& reporter : m_reporters) {
                try {
                    reporter->Report(snapshot);
                } catch (const std::exception& ex) {
                    ReportError("Reporter failed: " + std::string(ex.what()));
                }
            }
        }
        
        m_totalSamples.fetch_add(1, std::memory_order_relaxed);
        
    } catch (const std::exception& ex) {
        ReportError("Failed to collect metrics snapshot: " + std::string(ex.what()));
    }
}

void TelemetryManager::SamplingLoop() {
    Logger::Info("Telemetry sampling loop started");
    
    while (m_running.load()) {
        auto loopStart = std::chrono::steady_clock::now();
        
        try {
            ForceSample();
        } catch (const std::exception& ex) {
            ReportError("Sampling loop error: " + std::string(ex.what()));
        }
        
        // Calculate sleep time to maintain consistent interval
        auto loopEnd = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(loopEnd - loopStart);
        auto sleepTime = m_config.samplingInterval - elapsed;
        
        if (sleepTime > std::chrono::milliseconds(0)) {
            std::this_thread::sleep_for(sleepTime);
        } else if (elapsed > m_config.samplingInterval * 2) {
            // Warn if sampling is taking too long
            Logger::Warn("Telemetry sampling took %lldms (interval: %lldms)",
                        elapsed.count(), m_config.samplingInterval.count());
        }
    }
    
    Logger::Info("Telemetry sampling loop stopped");
}

MetricsSnapshot TelemetryManager::CollectSnapshot() {
    MetricsSnapshot snapshot;
    snapshot.timestamp = std::chrono::system_clock::now();
    
    try {
        if (m_config.enableSystemMetrics) {
            CollectSystemMetrics(snapshot);
        }
        
        if (m_config.enableApplicationMetrics) {
            CollectApplicationMetrics(snapshot);
        }
        
    } catch (const std::exception& ex) {
        ReportError("Error collecting metrics: " + std::string(ex.what()));
    }
    
    return snapshot;
}

void TelemetryManager::CollectSystemMetrics(MetricsSnapshot& snapshot) {
    // CPU Usage
    snapshot.cpuUsagePercent = GetCPUUsage();
    
    // Memory Usage
    auto [memUsed, memTotal] = GetMemoryUsage();
    snapshot.memoryUsedBytes = memUsed;
    snapshot.memoryTotalBytes = memTotal;
    
    // Network Stats
    auto [netSent, netRecv] = GetNetworkStats();
    snapshot.networkBytesSent = netSent;
    snapshot.networkBytesReceived = netRecv;
    
    // Disk Stats
    auto [diskRead, diskWrite] = GetDiskStats();
    snapshot.diskReadBytes = diskRead;
    snapshot.diskWriteBytes = diskWrite;
}

void TelemetryManager::CollectApplicationMetrics(MetricsSnapshot& snapshot) {
    // Copy atomic values to snapshot
    snapshot.activeConnections = m_customMetrics.activeConnections.load(std::memory_order_relaxed);
    snapshot.authenticatedPlayers = m_customMetrics.authenticatedPlayers.load(std::memory_order_relaxed);
    snapshot.totalPacketsProcessed = m_customMetrics.totalPacketsProcessed.load(std::memory_order_relaxed);
    snapshot.totalPacketsDropped = m_customMetrics.totalPacketsDropped.load(std::memory_order_relaxed);
    snapshot.currentTick = m_customMetrics.currentTick.load(std::memory_order_relaxed);
    snapshot.averageLatencyMs = m_customMetrics.averageLatencyMs.load(std::memory_order_relaxed);
    snapshot.packetLossRate = m_customMetrics.packetLossRate.load(std::memory_order_relaxed);
    
    snapshot.activeMatches = m_customMetrics.activeMatches.load(std::memory_order_relaxed);
    snapshot.totalKills = m_customMetrics.totalKills.load(std::memory_order_relaxed);
    snapshot.totalDeaths = m_customMetrics.totalDeaths.load(std::memory_order_relaxed);
    snapshot.objectivesCaptured = m_customMetrics.objectivesCaptured.load(std::memory_order_relaxed);
    snapshot.chatMessagesSent = m_customMetrics.chatMessagesSent.load(std::memory_order_relaxed);
    
    snapshot.frameTimeMs = m_customMetrics.frameTimeMs.load(std::memory_order_relaxed);
    snapshot.physicsTimeMs = m_customMetrics.physicsTimeMs.load(std::memory_order_relaxed);
    snapshot.networkTimeMs = m_customMetrics.networkTimeMs.load(std::memory_order_relaxed);
    snapshot.gameLogicTimeMs = m_customMetrics.gameLogicTimeMs.load(std::memory_order_relaxed);
    
    snapshot.securityViolations = m_customMetrics.securityViolations.load(std::memory_order_relaxed);
    snapshot.malformedPackets = m_customMetrics.malformedPackets.load(std::memory_order_relaxed);
    snapshot.speedHackDetections = m_customMetrics.speedHackDetections.load(std::memory_order_relaxed);
    snapshot.kickedPlayers = m_customMetrics.kickedPlayers.load(std::memory_order_relaxed);
    snapshot.bannedPlayers = m_customMetrics.bannedPlayers.load(std::memory_order_relaxed);
}

double TelemetryManager::GetCPUUsage() {
#ifdef __linux__
    std::ifstream stat("/proc/stat");
    if (!stat.is_open()) {
        return 0.0;
    }
    
    CPUTimes current;
    std::string cpu;
    stat >> cpu >> current.user >> current.nice >> current.system >> current.idle
         >> current.iowait >> current.irq >> current.softirq >> current.steal;
    stat.close();
    
    if (!m_hasPreviousCPUTimes) {
        m_lastCPUTimes = current;
        m_hasPreviousCPUTimes = true;
        return 0.0;
    }
    
    uint64_t totalDelta = (current.user + current.nice + current.system + current.idle +
                          current.iowait + current.irq + current.softirq + current.steal) -
                         (m_lastCPUTimes.user + m_lastCPUTimes.nice + m_lastCPUTimes.system +
                          m_lastCPUTimes.idle + m_lastCPUTimes.iowait + m_lastCPUTimes.irq +
                          m_lastCPUTimes.softirq + m_lastCPUTimes.steal);
    
    uint64_t idleDelta = current.idle - m_lastCPUTimes.idle;
    
    m_lastCPUTimes = current;
    
    if (totalDelta == 0) {
        return 0.0;
    }
    
    return 100.0 * (totalDelta - idleDelta) / totalDelta;
    
#elif defined(_WIN32)
    static PDH_HQUERY query = nullptr;
    static PDH_HCOUNTER counter = nullptr;
    static bool initialized = false;
    
    if (!initialized) {
        if (PdhOpenQuery(nullptr, 0, &query) == ERROR_SUCCESS) {
            if (PdhAddEnglishCounter(query, L"\\Processor(_Total)\\% Processor Time", 0, &counter) == ERROR_SUCCESS) {
                PdhCollectQueryData(query);
                initialized = true;
            }
        }
    }
    
    if (initialized) {
        PdhCollectQueryData(query);
        PDH_FMT_COUNTERVALUE value;
        if (PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, nullptr, &value) == ERROR_SUCCESS) {
            return value.doubleValue;
        }
    }
    
    return 0.0;
    
#else
    // macOS or other platforms - simplified implementation
    return 0.0;
#endif
}

std::pair<uint64_t, uint64_t> TelemetryManager::GetMemoryUsage() {
#ifdef __linux__
    std::ifstream meminfo("/proc/meminfo");
    if (!meminfo.is_open()) {
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
    return {memUsed, memTotal};
    
#elif defined(_WIN32)
    MEMORYSTATUSEX memStatus;
    memStatus.dwLength = sizeof(memStatus);
    if (GlobalMemoryStatusEx(&memStatus)) {
        uint64_t memUsed = memStatus.ullTotalPhys - memStatus.ullAvailPhys;
        return {memUsed, memStatus.ullTotalPhys};
    }
    return {0, 0};
    
#else
    return {0, 0};
#endif
}

std::pair<uint64_t, uint64_t> TelemetryManager::GetNetworkStats() {
#ifdef __linux__
    std::ifstream netdev("/proc/net/dev");
    if (!netdev.is_open()) {
        return {0, 0};
    }
    
    uint64_t totalSent = 0, totalReceived = 0;
    std::string line;
    
    // Skip header lines
    std::getline(netdev, line);
    std::getline(netdev, line);
    
    while (std::getline(netdev, line)) {
        size_t colonPos = line.find(':');
        if (colonPos == std::string::npos) continue;
        
        std::string interface = line.substr(0, colonPos);
        // Trim whitespace
        interface.erase(0, interface.find_first_not_of(" \t"));
        interface.erase(interface.find_last_not_of(" \t") + 1);
        
        // Skip loopback interface
        if (interface == "lo") continue;
        
        std::istringstream iss(line.substr(colonPos + 1));
        uint64_t recvBytes, recvPackets, recvErrs, recvDrop, recvFifo, recvFrame, recvCompressed, recvMulticast;
        uint64_t transBytes, transPackets, transErrs, transDrop, transFifo, transColls, transCarrier, transCompressed;
        
        if (iss >> recvBytes >> recvPackets >> recvErrs >> recvDrop >> recvFifo >> recvFrame >> recvCompressed >> recvMulticast
               >> transBytes >> transPackets >> transErrs >> transDrop >> transFifo >> transColls >> transCarrier >> transCompressed) {
            totalReceived += recvBytes;
            totalSent += transBytes;
        }
    }
    netdev.close();
    
    return {totalSent, totalReceived};
    
#elif defined(_WIN32)
    // Windows implementation would use GetIfTable2 or similar
    return {0, 0};
    
#else
    return {0, 0};
#endif
}

std::pair<uint64_t, uint64_t> TelemetryManager::GetDiskStats() {
#ifdef __linux__
    std::ifstream diskstats("/proc/diskstats");
    if (!diskstats.is_open()) {
        return {0, 0};
    }
    
    uint64_t totalRead = 0, totalWrite = 0;
    std::string line;
    
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
            }
        }
    }
    diskstats.close();
    
    return {totalRead, totalWrite};
    
#else
    return {0, 0};
#endif
}

MetricsSnapshot TelemetryManager::GetLatestSnapshot() const {
    std::lock_guard<std::mutex> lock(m_snapshotMutex);
    if (m_snapshots.empty()) {
        return MetricsSnapshot{};
    }
    
    if (m_snapshots.size() < m_config.maxSamplesInMemory) {
        return m_snapshots.back();
    } else {
        size_t latest = (m_snapshotIndex + m_config.maxSamplesInMemory - 1) % m_config.maxSamplesInMemory;
        return m_snapshots[latest];
    }
}

std::vector<MetricsSnapshot> TelemetryManager::GetRecentSnapshots(size_t count) const {
    std::lock_guard<std::mutex> lock(m_snapshotMutex);
    std::vector<MetricsSnapshot> result;
    
    if (m_snapshots.empty()) {
        return result;
    }
    
    size_t available = std::min(count, m_snapshots.size());
    result.reserve(available);
    
    if (m_snapshots.size() < m_config.maxSamplesInMemory) {
        // Linear storage
        size_t start = m_snapshots.size() >= available ? m_snapshots.size() - available : 0;
        for (size_t i = start; i < m_snapshots.size(); ++i) {
            result.push_back(m_snapshots[i]);
        }
    } else {
        // Circular buffer
        for (size_t i = 0; i < available; ++i) {
            size_t index = (m_snapshotIndex + m_config.maxSamplesInMemory - available + i) % m_config.maxSamplesInMemory;
            result.push_back(m_snapshots[index]);
        }
    }
    
    return result;
}

void TelemetryManager::UpdateConfig(const TelemetryConfig& config) {
    bool needsRestart = (m_config.samplingInterval != config.samplingInterval) && m_running.load();
    
    m_config = config;
    
    if (needsRestart) {
        StopSampling();
        StartSampling();
    }
    
    Logger::Info("Telemetry configuration updated");
}

std::vector<std::string> TelemetryManager::GetLastErrors() const {
    std::lock_guard<std::mutex> lock(m_errorMutex);
    return m_errors;
}

void TelemetryManager::ClearErrors() {
    std::lock_guard<std::mutex> lock(m_errorMutex);
    m_errors.clear();
}

void TelemetryManager::ReportError(const std::string& error) {
    std::lock_guard<std::mutex> lock(m_errorMutex);
    
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    
    std::ostringstream oss;
    oss << "[" << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S") << "] " << error;
    
    m_errors.push_back(oss.str());
    
    // Limit error history
    if (m_errors.size() > MAX_ERRORS) {
        m_errors.erase(m_errors.begin());
    }
    
    Logger::Error("Telemetry: %s", error.c_str());
}

} // namespace Telemetry