// Server/telemetry/MemoryMetricsReporter.cpp
// Implementation of in-memory circular buffer metrics reporter for real-time queries
// in the RS2V server telemetry system

#include "MetricsReporter.h"
#include "TelemetryManager.h"
#include "Utils/Logger.h"

#include <algorithm>
#include <chrono>

namespace Telemetry {

MemoryMetricsReporter::MemoryMetricsReporter(const MemoryReporterConfig& config)
    : m_config(config), m_writeIndex(0), m_bufferFull(false), m_reportsGenerated(0) {
    Logger::Trace("[MemoryMetricsReporter::MemoryMetricsReporter] Entry: maxSnapshots=%zu, enableStatistics=%s, retentionPeriod=%lld min",
                  config.maxSnapshots, config.enableStatistics ? "true" : "false",
                  (long long)config.retentionPeriod.count());

    // Validate configuration
    if (m_config.maxSnapshots == 0) {
        m_config.maxSnapshots = 3600; // Default to 1 hour at 1 second intervals
        Logger::Debug("[MemoryMetricsReporter::MemoryMetricsReporter] maxSnapshots was 0, defaulting to 3600");
    }

    Logger::Info("MemoryMetricsReporter created with config: maxSnapshots=%zu, enableStatistics=%s, retentionPeriod=%lld min",
                m_config.maxSnapshots, m_config.enableStatistics ? "true" : "false",
                (long long)m_config.retentionPeriod.count());
    Logger::Trace("[MemoryMetricsReporter::MemoryMetricsReporter] Exit");
}

bool MemoryMetricsReporter::Initialize(const std::string& outputDirectory) {
    Logger::Trace("[MemoryMetricsReporter::Initialize] Entry: outputDirectory='%s'", outputDirectory.c_str());
    std::lock_guard<std::mutex> lock(m_snapshotMutex);

    try {
        // Pre-allocate the circular buffer
        m_snapshots.clear();
        m_snapshots.reserve(m_config.maxSnapshots);
        m_writeIndex = 0;
        m_bufferFull = false;
        m_reportsGenerated = 0;

        // Reset statistics
        {
            std::lock_guard<std::mutex> statsLock(m_statsMutex);
            m_statistics = Statistics{};
        }

        Logger::Debug("[MemoryMetricsReporter::Initialize] Pre-allocated buffer for %zu snapshots", m_config.maxSnapshots);
        Logger::Info("MemoryMetricsReporter initialized successfully with capacity for %zu snapshots",
                    m_config.maxSnapshots);
        Logger::Trace("[MemoryMetricsReporter::Initialize] Exit: returning true");
        return true;

    } catch (const std::exception& ex) {
        Logger::Error("[MemoryMetricsReporter::Initialize] Exception during initialization: %s", ex.what());
        Logger::Trace("[MemoryMetricsReporter::Initialize] Exit: returning false (exception)");
        return false;
    }
}

void MemoryMetricsReporter::Shutdown() {
    Logger::Trace("[MemoryMetricsReporter::Shutdown] Entry");

    {
        std::lock_guard<std::mutex> lock(m_snapshotMutex);
        m_snapshots.clear();
        m_writeIndex = 0;
        m_bufferFull = false;
        Logger::Debug("[MemoryMetricsReporter::Shutdown] Cleared snapshot buffer");
    }

    {
        std::lock_guard<std::mutex> statsLock(m_statsMutex);
        m_statistics = Statistics{};
        Logger::Debug("[MemoryMetricsReporter::Shutdown] Reset statistics");
    }

    Logger::Info("MemoryMetricsReporter shutdown complete. Generated %llu reports total.",
                m_reportsGenerated.load());
    Logger::Trace("[MemoryMetricsReporter::Shutdown] Exit");
}

void MemoryMetricsReporter::Report(const MetricsSnapshot& snapshot) {
    Logger::Trace("[MemoryMetricsReporter::Report] Entry");
    std::lock_guard<std::mutex> lock(m_snapshotMutex);

    try {
        // Store snapshot in circular buffer
        if (m_bufferFull) {
            // Overwrite the oldest entry
            m_snapshots[m_writeIndex] = snapshot;
            Logger::Debug("[MemoryMetricsReporter::Report] Overwrote snapshot at index %zu (buffer full)", m_writeIndex);
        } else {
            // Buffer still has room, append
            m_snapshots.push_back(snapshot);
            Logger::Debug("[MemoryMetricsReporter::Report] Appended snapshot at index %zu (buffer size=%zu)",
                          m_writeIndex, m_snapshots.size());
        }

        m_writeIndex = (m_writeIndex + 1) % m_config.maxSnapshots;
        if (m_writeIndex == 0 && !m_bufferFull) {
            m_bufferFull = true;
            Logger::Debug("[MemoryMetricsReporter::Report] Buffer is now full, will start overwriting oldest entries");
        }

        m_reportsGenerated.fetch_add(1, std::memory_order_relaxed);
        Logger::Debug("[MemoryMetricsReporter::Report] Report #%llu stored, bufferSize=%zu",
                      m_reportsGenerated.load(), m_snapshots.size());

        // Update running statistics if enabled
        if (m_config.enableStatistics) {
            UpdateStatistics(snapshot);
        }

        // Cleanup expired snapshots based on retention period
        CleanupExpiredSnapshots();

    } catch (const std::exception& ex) {
        Logger::Error("[MemoryMetricsReporter::Report] Exception storing snapshot: %s", ex.what());
    }
    Logger::Trace("[MemoryMetricsReporter::Report] Exit");
}

std::vector<MetricsSnapshot> MemoryMetricsReporter::GetSnapshots(size_t count) const {
    Logger::Trace("[MemoryMetricsReporter::GetSnapshots] Entry: count=%zu", count);
    std::lock_guard<std::mutex> lock(m_snapshotMutex);

    std::vector<MetricsSnapshot> result;

    if (m_snapshots.empty()) {
        Logger::Debug("[MemoryMetricsReporter::GetSnapshots] No snapshots available");
        Logger::Trace("[MemoryMetricsReporter::GetSnapshots] Exit: returning 0 snapshots");
        return result;
    }

    size_t totalSnapshots = m_snapshots.size();
    size_t numToReturn = (count == 0 || count > totalSnapshots) ? totalSnapshots : count;
    result.reserve(numToReturn);

    Logger::Debug("[MemoryMetricsReporter::GetSnapshots] Retrieving %zu of %zu snapshots", numToReturn, totalSnapshots);

    if (m_bufferFull) {
        // Buffer is full, read from oldest to newest
        // The oldest entry is at m_writeIndex, the newest is at (m_writeIndex - 1 + maxSnapshots) % maxSnapshots
        // We want the most recent 'numToReturn' snapshots
        size_t startIndex = (m_writeIndex + totalSnapshots - numToReturn) % totalSnapshots;
        for (size_t i = 0; i < numToReturn; ++i) {
            size_t idx = (startIndex + i) % totalSnapshots;
            result.push_back(m_snapshots[idx]);
        }
    } else {
        // Buffer is not full, snapshots are stored sequentially from 0 to m_snapshots.size()-1
        // Return the most recent numToReturn entries
        size_t startIndex = totalSnapshots - numToReturn;
        for (size_t i = startIndex; i < totalSnapshots; ++i) {
            result.push_back(m_snapshots[i]);
        }
    }

    Logger::Debug("[MemoryMetricsReporter::GetSnapshots] Returning %zu snapshots", result.size());
    Logger::Trace("[MemoryMetricsReporter::GetSnapshots] Exit: returning %zu snapshots", result.size());
    return result;
}

std::vector<MetricsSnapshot> MemoryMetricsReporter::GetSnapshotsInRange(
    std::chrono::system_clock::time_point start,
    std::chrono::system_clock::time_point end) const {
    Logger::Trace("[MemoryMetricsReporter::GetSnapshotsInRange] Entry");
    std::lock_guard<std::mutex> lock(m_snapshotMutex);

    std::vector<MetricsSnapshot> result;

    if (m_snapshots.empty()) {
        Logger::Debug("[MemoryMetricsReporter::GetSnapshotsInRange] No snapshots available");
        Logger::Trace("[MemoryMetricsReporter::GetSnapshotsInRange] Exit: returning 0 snapshots");
        return result;
    }

    size_t totalSnapshots = m_snapshots.size();
    Logger::Debug("[MemoryMetricsReporter::GetSnapshotsInRange] Scanning %zu snapshots for time range", totalSnapshots);

    if (m_bufferFull) {
        // Iterate in chronological order starting from the oldest
        for (size_t i = 0; i < totalSnapshots; ++i) {
            size_t idx = (m_writeIndex + i) % totalSnapshots;
            const auto& snap = m_snapshots[idx];
            if (snap.timestamp >= start && snap.timestamp <= end) {
                result.push_back(snap);
            }
        }
    } else {
        // Sequential buffer, iterate directly
        for (const auto& snap : m_snapshots) {
            if (snap.timestamp >= start && snap.timestamp <= end) {
                result.push_back(snap);
            }
        }
    }

    Logger::Debug("[MemoryMetricsReporter::GetSnapshotsInRange] Found %zu snapshots in range", result.size());
    Logger::Trace("[MemoryMetricsReporter::GetSnapshotsInRange] Exit: returning %zu snapshots", result.size());
    return result;
}

MetricsSnapshot MemoryMetricsReporter::GetLatestSnapshot() const {
    Logger::Trace("[MemoryMetricsReporter::GetLatestSnapshot] Entry");
    std::lock_guard<std::mutex> lock(m_snapshotMutex);

    if (m_snapshots.empty()) {
        Logger::Debug("[MemoryMetricsReporter::GetLatestSnapshot] No snapshots available, returning default");
        Logger::Trace("[MemoryMetricsReporter::GetLatestSnapshot] Exit: returning default snapshot");
        return MetricsSnapshot{};
    }

    // The most recent snapshot is at the index just before m_writeIndex
    size_t latestIndex;
    if (m_bufferFull) {
        latestIndex = (m_writeIndex == 0) ? m_snapshots.size() - 1 : m_writeIndex - 1;
    } else {
        latestIndex = m_snapshots.size() - 1;
    }

    Logger::Debug("[MemoryMetricsReporter::GetLatestSnapshot] Returning snapshot at index %zu", latestIndex);
    Logger::Trace("[MemoryMetricsReporter::GetLatestSnapshot] Exit");
    return m_snapshots[latestIndex];
}

void MemoryMetricsReporter::ClearSnapshots() {
    Logger::Trace("[MemoryMetricsReporter::ClearSnapshots] Entry");
    std::lock_guard<std::mutex> lock(m_snapshotMutex);

    size_t count = m_snapshots.size();
    m_snapshots.clear();
    m_writeIndex = 0;
    m_bufferFull = false;

    Logger::Info("[MemoryMetricsReporter::ClearSnapshots] Cleared %zu snapshots from buffer", count);
    Logger::Trace("[MemoryMetricsReporter::ClearSnapshots] Exit");
}

size_t MemoryMetricsReporter::GetSnapshotCount() const {
    Logger::Trace("[MemoryMetricsReporter::GetSnapshotCount] Entry");
    std::lock_guard<std::mutex> lock(m_snapshotMutex);
    size_t count = m_snapshots.size();
    Logger::Trace("[MemoryMetricsReporter::GetSnapshotCount] Exit: returning %zu", count);
    return count;
}

MemoryMetricsReporter::Statistics MemoryMetricsReporter::GetStatistics() const {
    Logger::Trace("[MemoryMetricsReporter::GetStatistics] Entry");
    std::lock_guard<std::mutex> lock(m_statsMutex);

    Logger::Debug("[MemoryMetricsReporter::GetStatistics] avgCpu=%.2f, maxMemory=%llu, totalNetworkBytes=%llu, "
                  "totalPackets=%llu, avgLatency=%.2f, totalSecurityViolations=%llu",
                  m_statistics.avgCpuUsage, (unsigned long long)m_statistics.maxMemoryUsed,
                  (unsigned long long)m_statistics.totalNetworkBytes,
                  (unsigned long long)m_statistics.totalPacketsProcessed,
                  m_statistics.avgLatency,
                  (unsigned long long)m_statistics.totalSecurityViolations);

    Logger::Trace("[MemoryMetricsReporter::GetStatistics] Exit");
    return m_statistics;
}

void MemoryMetricsReporter::UpdateStatistics(const MetricsSnapshot& snapshot) {
    Logger::Trace("[MemoryMetricsReporter::UpdateStatistics] Entry");
    std::lock_guard<std::mutex> statsLock(m_statsMutex);

    try {
        uint64_t reportCount = m_reportsGenerated.load(std::memory_order_relaxed);

        // Running average for CPU usage
        // avg_new = avg_old + (new_value - avg_old) / n
        if (reportCount > 0) {
            m_statistics.avgCpuUsage += (snapshot.cpuUsagePercent - m_statistics.avgCpuUsage) / static_cast<double>(reportCount);
        } else {
            m_statistics.avgCpuUsage = snapshot.cpuUsagePercent;
        }

        // Max memory used
        if (snapshot.memoryUsedBytes > m_statistics.maxMemoryUsed) {
            m_statistics.maxMemoryUsed = snapshot.memoryUsedBytes;
            Logger::Debug("[MemoryMetricsReporter::UpdateStatistics] New max memory: %llu bytes",
                          (unsigned long long)m_statistics.maxMemoryUsed);
        }

        // Accumulate total network bytes
        m_statistics.totalNetworkBytes += snapshot.networkBytesSent + snapshot.networkBytesReceived;

        // Accumulate total packets processed
        m_statistics.totalPacketsProcessed += snapshot.totalPacketsProcessed;

        // Running average for latency
        if (reportCount > 0) {
            m_statistics.avgLatency += (snapshot.averageLatencyMs - m_statistics.avgLatency) / static_cast<double>(reportCount);
        } else {
            m_statistics.avgLatency = snapshot.averageLatencyMs;
        }

        // Accumulate total security violations
        m_statistics.totalSecurityViolations += snapshot.securityViolations;

        Logger::Debug("[MemoryMetricsReporter::UpdateStatistics] Updated statistics: avgCpu=%.2f, maxMem=%llu, "
                      "totalNet=%llu, totalPkts=%llu, avgLat=%.2f, totalSecViol=%llu",
                      m_statistics.avgCpuUsage, (unsigned long long)m_statistics.maxMemoryUsed,
                      (unsigned long long)m_statistics.totalNetworkBytes,
                      (unsigned long long)m_statistics.totalPacketsProcessed,
                      m_statistics.avgLatency,
                      (unsigned long long)m_statistics.totalSecurityViolations);

    } catch (const std::exception& ex) {
        Logger::Error("[MemoryMetricsReporter::UpdateStatistics] Exception updating statistics: %s", ex.what());
    }
    Logger::Trace("[MemoryMetricsReporter::UpdateStatistics] Exit");
}

void MemoryMetricsReporter::CleanupExpiredSnapshots() {
    // Note: m_snapshotMutex is already held by the caller (Report)
    Logger::Trace("[MemoryMetricsReporter::CleanupExpiredSnapshots] Entry");

    if (m_snapshots.empty()) {
        Logger::Trace("[MemoryMetricsReporter::CleanupExpiredSnapshots] Exit: no snapshots to clean");
        return;
    }

    auto now = std::chrono::system_clock::now();
    auto expirationThreshold = now - m_config.retentionPeriod;

    size_t expiredCount = 0;

    if (m_bufferFull) {
        // In a full circular buffer, we cannot easily remove entries from the middle.
        // Instead, we invalidate expired entries by checking timestamps during reads.
        // However, since the circular buffer naturally evicts the oldest entries when new
        // ones are written, retention is inherently enforced by the buffer size.
        // We only log how many would be expired for diagnostic purposes.
        for (size_t i = 0; i < m_snapshots.size(); ++i) {
            size_t idx = (m_writeIndex + i) % m_snapshots.size();
            if (m_snapshots[idx].timestamp < expirationThreshold) {
                expiredCount++;
            } else {
                // Since snapshots are in chronological order, once we hit a non-expired one, stop
                break;
            }
        }
        if (expiredCount > 0) {
            Logger::Debug("[MemoryMetricsReporter::CleanupExpiredSnapshots] %zu snapshots are past retention period "
                          "(will be naturally overwritten by circular buffer)", expiredCount);
        }
    } else {
        // Buffer is not full, we can remove expired entries from the front
        size_t removeCount = 0;
        for (size_t i = 0; i < m_snapshots.size(); ++i) {
            if (m_snapshots[i].timestamp < expirationThreshold) {
                removeCount++;
            } else {
                break;
            }
        }

        if (removeCount > 0) {
            m_snapshots.erase(m_snapshots.begin(), m_snapshots.begin() + removeCount);
            // Adjust write index since we removed from the front
            if (m_writeIndex >= removeCount) {
                m_writeIndex -= removeCount;
            } else {
                m_writeIndex = 0;
            }
            Logger::Info("[MemoryMetricsReporter::CleanupExpiredSnapshots] Removed %zu expired snapshots, "
                         "%zu remaining", removeCount, m_snapshots.size());
        }
    }

    Logger::Trace("[MemoryMetricsReporter::CleanupExpiredSnapshots] Exit");
}

// Factory function implementation
namespace ReporterFactory {

std::unique_ptr<MetricsReporter> CreateMemoryReporter(size_t maxSnapshots) {
    Logger::Trace("[ReporterFactory::CreateMemoryReporter] Entry: maxSnapshots=%zu", maxSnapshots);

    MemoryReporterConfig config;
    config.maxSnapshots = maxSnapshots;
    config.enableStatistics = true;
    config.retentionPeriod = std::chrono::minutes(60);

    Logger::Debug("[ReporterFactory::CreateMemoryReporter] Config: enableStatistics=true, retentionPeriod=60 min");
    auto reporter = std::make_unique<MemoryMetricsReporter>(config);
    Logger::Info("[ReporterFactory::CreateMemoryReporter] Created MemoryMetricsReporter with maxSnapshots=%zu", maxSnapshots);
    Logger::Trace("[ReporterFactory::CreateMemoryReporter] Exit: returning MemoryMetricsReporter");
    return reporter;
}

} // namespace ReporterFactory

} // namespace Telemetry
