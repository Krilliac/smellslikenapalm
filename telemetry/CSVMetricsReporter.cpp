// Server/telemetry/CSVMetricsReporter.cpp
// Implementation of CSV metrics reporter for exporting telemetry data
// to CSV format for analysis tools and spreadsheet applications

#include "MetricsReporter.h"
#include "TelemetryManager.h"
#include "Utils/Logger.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <chrono>

namespace Telemetry {

// Default column order when no selectedFields are specified
static const std::vector<std::string> ALL_FIELDS = {
    "timestamp",
    "timestamp_ms",
    "cpu_usage_percent",
    "memory_used_bytes",
    "memory_total_bytes",
    "network_bytes_sent",
    "network_bytes_received",
    "disk_read_bytes",
    "disk_write_bytes",
    "active_connections",
    "authenticated_players",
    "total_packets_processed",
    "total_packets_dropped",
    "current_tick",
    "average_latency_ms",
    "packet_loss_rate",
    "active_matches",
    "total_kills",
    "total_deaths",
    "objectives_captured",
    "chat_messages_sent",
    "frame_time_ms",
    "physics_time_ms",
    "network_time_ms",
    "game_logic_time_ms",
    "security_violations",
    "malformed_packets",
    "speed_hack_detections",
    "kicked_players",
    "banned_players"
};

CSVMetricsReporter::CSVMetricsReporter(const CSVReporterConfig& config)
    : m_config(config), m_healthy(true), m_reportsGenerated(0), m_reportsFailed(0) {
    Logger::Trace("[CSVMetricsReporter::CSVMetricsReporter] Entry: filename='%s', delimiter='%c', appendMode=%d",
                  config.filename.c_str(), config.delimiter, config.appendMode);

    // Validate configuration
    if (m_config.filename.empty()) {
        m_config.filename = "metrics.csv";
        Logger::Debug("[CSVMetricsReporter::CSVMetricsReporter] filename was empty, defaulting to 'metrics.csv'");
    }
    if (m_config.delimiter == '\0') {
        m_config.delimiter = ',';
        Logger::Debug("[CSVMetricsReporter::CSVMetricsReporter] delimiter was null, defaulting to ','");
    }

    Logger::Info("CSVMetricsReporter created with config: filename=%s, includeHeaders=%d, delimiter='%c', appendMode=%d",
                m_config.filename.c_str(), m_config.includeHeaders, m_config.delimiter, m_config.appendMode);
    Logger::Trace("[CSVMetricsReporter::CSVMetricsReporter] Exit");
}

CSVMetricsReporter::~CSVMetricsReporter() {
    Logger::Trace("[CSVMetricsReporter::~CSVMetricsReporter] Entry");
    Shutdown();
    Logger::Trace("[CSVMetricsReporter::~CSVMetricsReporter] Exit");
}

bool CSVMetricsReporter::Initialize(const std::string& outputDirectory) {
    Logger::Trace("[CSVMetricsReporter::Initialize] Entry: outputDirectory='%s'", outputDirectory.c_str());
    std::lock_guard<std::mutex> lock(m_fileMutex);

    try {
        m_outputDirectory = outputDirectory;

        // Create output directory if it doesn't exist
        if (!std::filesystem::exists(m_outputDirectory)) {
            Logger::Debug("[CSVMetricsReporter::Initialize] Output directory does not exist, creating it");
            std::filesystem::create_directories(m_outputDirectory);
            Logger::Info("Created metrics output directory: %s", m_outputDirectory.c_str());
        } else {
            Logger::Debug("[CSVMetricsReporter::Initialize] Output directory already exists: '%s'", m_outputDirectory.c_str());
        }

        // Validate directory is writable
        std::string testFile = m_outputDirectory + "/.write_test";
        std::ofstream test(testFile);
        if (!test.is_open()) {
            Logger::Error("[CSVMetricsReporter::Initialize] Output directory is not writable: '%s'", m_outputDirectory.c_str());
            ReportError("Output directory is not writable: " + m_outputDirectory);
            m_healthy = false;
            Logger::Trace("[CSVMetricsReporter::Initialize] Exit: returning false (not writable)");
            return false;
        }
        test.close();
        std::filesystem::remove(testFile);
        Logger::Debug("[CSVMetricsReporter::Initialize] Directory write test passed");

        // Build the full file path
        m_filePath = m_outputDirectory + "/" + m_config.filename;
        Logger::Debug("[CSVMetricsReporter::Initialize] CSV file path: '%s'", m_filePath.c_str());

        // Determine if we need to write headers
        bool fileExists = std::filesystem::exists(m_filePath);
        bool needHeaders = m_config.includeHeaders && (!fileExists || !m_config.appendMode);
        Logger::Debug("[CSVMetricsReporter::Initialize] fileExists=%d, appendMode=%d, needHeaders=%d",
                      fileExists, m_config.appendMode, needHeaders);

        // Open the file with appropriate mode
        auto openMode = std::ios::out;
        if (m_config.appendMode && fileExists) {
            openMode |= std::ios::app;
            Logger::Debug("[CSVMetricsReporter::Initialize] Opening file in append mode");
        } else {
            openMode |= std::ios::trunc;
            Logger::Debug("[CSVMetricsReporter::Initialize] Opening file in truncate mode");
        }

        m_file = std::make_unique<std::ofstream>(m_filePath, openMode);

        if (!m_file->is_open()) {
            Logger::Error("[CSVMetricsReporter::Initialize] Failed to open CSV file: '%s'", m_filePath.c_str());
            ReportError("Failed to open CSV file: " + m_filePath);
            m_healthy = false;
            Logger::Trace("[CSVMetricsReporter::Initialize] Exit: returning false (file open failed)");
            return false;
        }

        // Write headers if needed
        if (needHeaders) {
            Logger::Debug("[CSVMetricsReporter::Initialize] Writing CSV headers");
            WriteHeaders();
        } else {
            m_headersWritten = true;
            Logger::Debug("[CSVMetricsReporter::Initialize] Skipping headers (already present or disabled)");
        }

        m_healthy = true;
        Logger::Info("CSVMetricsReporter initialized successfully: %s", m_filePath.c_str());
        Logger::Trace("[CSVMetricsReporter::Initialize] Exit: returning true");
        return true;

    } catch (const std::exception& ex) {
        Logger::Error("[CSVMetricsReporter::Initialize] Exception during initialization: %s", ex.what());
        ReportError("Failed to initialize CSVMetricsReporter: " + std::string(ex.what()));
        m_healthy = false;
        Logger::Trace("[CSVMetricsReporter::Initialize] Exit: returning false (exception)");
        return false;
    }
}

void CSVMetricsReporter::Shutdown() {
    Logger::Trace("[CSVMetricsReporter::Shutdown] Entry");
    std::lock_guard<std::mutex> lock(m_fileMutex);

    if (m_file && m_file->is_open()) {
        Logger::Debug("[CSVMetricsReporter::Shutdown] CSV file is open, flushing and closing");
        try {
            m_file->flush();
            m_file->close();
            Logger::Debug("[CSVMetricsReporter::Shutdown] File flushed and closed");
        } catch (const std::exception& ex) {
            Logger::Error("Error during CSVMetricsReporter shutdown: %s", ex.what());
        }
    } else {
        Logger::Debug("[CSVMetricsReporter::Shutdown] No current file open");
    }

    m_file.reset();
    m_filePath.clear();
    m_headersWritten = false;

    Logger::Info("CSVMetricsReporter shutdown complete. Generated %llu reports total.",
                m_reportsGenerated.load());
    Logger::Trace("[CSVMetricsReporter::Shutdown] Exit");
}

void CSVMetricsReporter::Report(const MetricsSnapshot& snapshot) {
    Logger::Trace("[CSVMetricsReporter::Report] Entry");
    std::lock_guard<std::mutex> lock(m_fileMutex);

    if (!m_healthy.load()) {
        Logger::Debug("[CSVMetricsReporter::Report] Reporter is unhealthy, skipping report");
        m_reportsFailed.fetch_add(1, std::memory_order_relaxed);
        Logger::Trace("[CSVMetricsReporter::Report] Exit: skipped (unhealthy)");
        return;
    }

    try {
        // Ensure we have a valid file
        if (!m_file || !m_file->is_open()) {
            Logger::Error("[CSVMetricsReporter::Report] No valid CSV file open");
            ReportError("No valid CSV file open for writing");
            m_reportsFailed.fetch_add(1, std::memory_order_relaxed);
            Logger::Trace("[CSVMetricsReporter::Report] Exit: failed (no file)");
            return;
        }

        // Write headers if they haven't been written yet
        if (!m_headersWritten && m_config.includeHeaders) {
            Logger::Debug("[CSVMetricsReporter::Report] Headers not yet written, writing now");
            WriteHeaders();
        }

        // Format and write the CSV row
        std::string csvRow = FormatCSVRow(snapshot);
        Logger::Debug("[CSVMetricsReporter::Report] Formatted CSV row: %zu bytes", csvRow.size());

        *m_file << csvRow << "\n";
        m_file->flush();

        // Update statistics
        m_reportsGenerated.fetch_add(1, std::memory_order_relaxed);
        Logger::Debug("[CSVMetricsReporter::Report] Report #%llu written",
                      m_reportsGenerated.load());

        // Check for write errors
        if (m_file->fail()) {
            Logger::Error("[CSVMetricsReporter::Report] File write error occurred");
            ReportError("CSV file write error occurred");
            m_reportsFailed.fetch_add(1, std::memory_order_relaxed);
            m_healthy = false;
        }

    } catch (const std::exception& ex) {
        Logger::Error("[CSVMetricsReporter::Report] Exception writing CSV metrics: %s", ex.what());
        ReportError("Error writing CSV metrics report: " + std::string(ex.what()));
        m_reportsFailed.fetch_add(1, std::memory_order_relaxed);
    }
    Logger::Trace("[CSVMetricsReporter::Report] Exit");
}

std::vector<std::string> CSVMetricsReporter::GetLastErrors() const {
    Logger::Trace("[CSVMetricsReporter::GetLastErrors] Entry");
    std::lock_guard<std::mutex> lock(m_errorMutex);
    Logger::Debug("[CSVMetricsReporter::GetLastErrors] Returning %zu errors", m_errors.size());
    Logger::Trace("[CSVMetricsReporter::GetLastErrors] Exit: returning %zu errors", m_errors.size());
    return m_errors;
}

void CSVMetricsReporter::ClearErrors() {
    Logger::Trace("[CSVMetricsReporter::ClearErrors] Entry");
    std::lock_guard<std::mutex> lock(m_errorMutex);
    size_t count = m_errors.size();
    m_errors.clear();
    m_healthy = true;
    Logger::Info("[CSVMetricsReporter::ClearErrors] Cleared %zu errors, reset healthy flag", count);
    Logger::Trace("[CSVMetricsReporter::ClearErrors] Exit");
}

void CSVMetricsReporter::WriteHeaders() {
    Logger::Trace("[CSVMetricsReporter::WriteHeaders] Entry");
    if (!m_file || !m_file->is_open()) {
        Logger::Warn("[CSVMetricsReporter::WriteHeaders] No file open, skipping header write");
        Logger::Trace("[CSVMetricsReporter::WriteHeaders] Exit: skipped (no file)");
        return;
    }

    try {
        const std::vector<std::string>& fields =
            m_config.selectedFields.empty() ? ALL_FIELDS : m_config.selectedFields;

        std::ostringstream header;
        for (size_t i = 0; i < fields.size(); ++i) {
            if (i > 0) {
                header << m_config.delimiter;
            }
            header << EscapeCSVField(fields[i]);
        }

        std::string headerStr = header.str();
        *m_file << headerStr << "\n";
        m_file->flush();
        m_headersWritten = true;

        Logger::Debug("[CSVMetricsReporter::WriteHeaders] Wrote %zu column headers (%zu bytes)",
                      fields.size(), headerStr.size());

    } catch (const std::exception& ex) {
        Logger::Error("[CSVMetricsReporter::WriteHeaders] Error writing CSV headers: %s", ex.what());
        ReportError("Error writing CSV headers: " + std::string(ex.what()));
    }
    Logger::Trace("[CSVMetricsReporter::WriteHeaders] Exit");
}

std::string CSVMetricsReporter::FormatCSVRow(const MetricsSnapshot& snapshot) const {
    Logger::Trace("[CSVMetricsReporter::FormatCSVRow] Entry");
    try {
        auto ts = std::chrono::duration_cast<std::chrono::seconds>(
            snapshot.timestamp.time_since_epoch()).count();
        auto ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            snapshot.timestamp.time_since_epoch()).count();

        // Build a map of field name to string value for flexible field selection
        std::unordered_map<std::string, std::string> fieldValues;

        std::ostringstream converter;
        converter << std::fixed << std::setprecision(4);

        fieldValues["timestamp"] = std::to_string(ts);
        fieldValues["timestamp_ms"] = std::to_string(ts_ms);

        converter.str(""); converter.clear();
        converter << std::fixed << std::setprecision(4) << snapshot.cpuUsagePercent;
        fieldValues["cpu_usage_percent"] = converter.str();

        fieldValues["memory_used_bytes"] = std::to_string(snapshot.memoryUsedBytes);
        fieldValues["memory_total_bytes"] = std::to_string(snapshot.memoryTotalBytes);
        fieldValues["network_bytes_sent"] = std::to_string(snapshot.networkBytesSent);
        fieldValues["network_bytes_received"] = std::to_string(snapshot.networkBytesReceived);
        fieldValues["disk_read_bytes"] = std::to_string(snapshot.diskReadBytes);
        fieldValues["disk_write_bytes"] = std::to_string(snapshot.diskWriteBytes);
        fieldValues["active_connections"] = std::to_string(snapshot.activeConnections);
        fieldValues["authenticated_players"] = std::to_string(snapshot.authenticatedPlayers);
        fieldValues["total_packets_processed"] = std::to_string(snapshot.totalPacketsProcessed);
        fieldValues["total_packets_dropped"] = std::to_string(snapshot.totalPacketsDropped);
        fieldValues["current_tick"] = std::to_string(snapshot.currentTick);

        converter.str(""); converter.clear();
        converter << std::fixed << std::setprecision(4) << snapshot.averageLatencyMs;
        fieldValues["average_latency_ms"] = converter.str();

        converter.str(""); converter.clear();
        converter << std::fixed << std::setprecision(4) << snapshot.packetLossRate;
        fieldValues["packet_loss_rate"] = converter.str();

        fieldValues["active_matches"] = std::to_string(snapshot.activeMatches);
        fieldValues["total_kills"] = std::to_string(snapshot.totalKills);
        fieldValues["total_deaths"] = std::to_string(snapshot.totalDeaths);
        fieldValues["objectives_captured"] = std::to_string(snapshot.objectivesCaptured);
        fieldValues["chat_messages_sent"] = std::to_string(snapshot.chatMessagesSent);

        converter.str(""); converter.clear();
        converter << std::fixed << std::setprecision(4) << snapshot.frameTimeMs;
        fieldValues["frame_time_ms"] = converter.str();

        converter.str(""); converter.clear();
        converter << std::fixed << std::setprecision(4) << snapshot.physicsTimeMs;
        fieldValues["physics_time_ms"] = converter.str();

        converter.str(""); converter.clear();
        converter << std::fixed << std::setprecision(4) << snapshot.networkTimeMs;
        fieldValues["network_time_ms"] = converter.str();

        converter.str(""); converter.clear();
        converter << std::fixed << std::setprecision(4) << snapshot.gameLogicTimeMs;
        fieldValues["game_logic_time_ms"] = converter.str();

        fieldValues["security_violations"] = std::to_string(snapshot.securityViolations);
        fieldValues["malformed_packets"] = std::to_string(snapshot.malformedPackets);
        fieldValues["speed_hack_detections"] = std::to_string(snapshot.speedHackDetections);
        fieldValues["kicked_players"] = std::to_string(snapshot.kickedPlayers);
        fieldValues["banned_players"] = std::to_string(snapshot.bannedPlayers);

        // Select which fields to output
        const std::vector<std::string>& fields =
            m_config.selectedFields.empty() ? ALL_FIELDS : m_config.selectedFields;

        std::ostringstream row;
        for (size_t i = 0; i < fields.size(); ++i) {
            if (i > 0) {
                row << m_config.delimiter;
            }
            auto it = fieldValues.find(fields[i]);
            if (it != fieldValues.end()) {
                row << EscapeCSVField(it->second);
            } else {
                Logger::Debug("[CSVMetricsReporter::FormatCSVRow] Unknown field '%s', writing empty value",
                              fields[i].c_str());
            }
        }

        std::string result = row.str();
        Logger::Debug("[CSVMetricsReporter::FormatCSVRow] Formatted row: %zu bytes, timestamp=%lld",
                      result.size(), (long long)ts);
        Logger::Trace("[CSVMetricsReporter::FormatCSVRow] Exit: returning CSV row (%zu bytes)", result.size());
        return result;

    } catch (const std::exception& ex) {
        Logger::Error("[CSVMetricsReporter::FormatCSVRow] Exception formatting CSV row: %s", ex.what());
        Logger::Trace("[CSVMetricsReporter::FormatCSVRow] Exit: returning empty string (error)");
        return "";
    }
}

std::string CSVMetricsReporter::EscapeCSVField(const std::string& field) const {
    Logger::Trace("[CSVMetricsReporter::EscapeCSVField] Entry: field='%s'", field.c_str());

    // Check if the field needs quoting
    bool needsQuoting = false;
    for (char c : field) {
        if (c == m_config.delimiter || c == '"' || c == '\n' || c == '\r') {
            needsQuoting = true;
            break;
        }
    }

    if (!needsQuoting) {
        Logger::Trace("[CSVMetricsReporter::EscapeCSVField] Exit: no quoting needed");
        return field;
    }

    // Escape double quotes by doubling them and wrap in quotes
    std::ostringstream escaped;
    escaped << '"';
    for (char c : field) {
        if (c == '"') {
            escaped << "\"\"";
        } else {
            escaped << c;
        }
    }
    escaped << '"';

    std::string result = escaped.str();
    Logger::Debug("[CSVMetricsReporter::EscapeCSVField] Escaped field: '%s' -> '%s'", field.c_str(), result.c_str());
    Logger::Trace("[CSVMetricsReporter::EscapeCSVField] Exit: returning escaped field");
    return result;
}

void CSVMetricsReporter::ReportError(const std::string& error) {
    Logger::Trace("[CSVMetricsReporter::ReportError] Entry: error='%s'", error.c_str());
    std::lock_guard<std::mutex> lock(m_errorMutex);

    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);

    std::ostringstream oss;
    oss << "[" << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S") << "] " << error;

    m_errors.push_back(oss.str());

    // Limit error history
    if (m_errors.size() > MAX_ERRORS) {
        Logger::Debug("[CSVMetricsReporter::ReportError] Error history exceeds MAX_ERRORS=%zu, trimming oldest", MAX_ERRORS);
        m_errors.erase(m_errors.begin());
    }

    Logger::Error("CSVMetricsReporter: %s", error.c_str());
    m_healthy = false;
    Logger::Trace("[CSVMetricsReporter::ReportError] Exit");
}

// Factory function implementation
namespace ReporterFactory {

std::unique_ptr<MetricsReporter> CreateCSVReporter(
    const std::string& filename) {

    Logger::Trace("[ReporterFactory::CreateCSVReporter] Entry: filename='%s'", filename.c_str());
    CSVReporterConfig config;
    config.filename = filename;
    config.includeHeaders = true;
    config.delimiter = ',';
    config.appendMode = true;

    Logger::Debug("[ReporterFactory::CreateCSVReporter] Config: includeHeaders=true, delimiter=',', appendMode=true");
    auto reporter = std::make_unique<CSVMetricsReporter>(config);
    Logger::Info("[ReporterFactory::CreateCSVReporter] Created CSVMetricsReporter with filename='%s'", filename.c_str());
    Logger::Trace("[ReporterFactory::CreateCSVReporter] Exit: returning CSVMetricsReporter");
    return reporter;
}

} // namespace ReporterFactory

} // namespace Telemetry
