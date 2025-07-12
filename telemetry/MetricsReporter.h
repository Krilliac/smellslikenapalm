// Server/telemetry/MetricsReporter.h
// Abstract base class and concrete implementations for telemetry reporting backends
//
// Provides:
// - Abstract reporter interface for pluggable metrics output
// - File-based JSON reporter with rotation
// - Prometheus HTTP endpoint reporter
// - In-memory circular buffer reporter
// - CSV export reporter for analysis tools

#pragma once

#include <string>
#include <memory>
#include <vector>
#include <fstream>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <queue>
#include <unordered_map>

// Forward declaration
namespace Telemetry {
struct MetricsSnapshot;
}

namespace Telemetry {

// Abstract base class for all metrics reporters
class MetricsReporter {
public:
    virtual ~MetricsReporter() = default;
    
    // Lifecycle management
    virtual bool Initialize(const std::string& outputDirectory) = 0;
    virtual void Shutdown() = 0;
    
    // Core reporting interface
    virtual void Report(const MetricsSnapshot& snapshot) = 0;
    
    // Optional interfaces
    virtual std::string GetReporterType() const = 0;
    virtual bool IsHealthy() const { return true; }
    virtual std::vector<std::string> GetLastErrors() const { return {}; }
    virtual void ClearErrors() {}
    
    // Statistics
    virtual uint64_t GetReportsGenerated() const { return 0; }
    virtual uint64_t GetReportsFailed() const { return 0; }
};

// Configuration for file-based reporters
struct FileReporterConfig {
    std::string filePrefix = "metrics";
    std::string fileExtension = ".json";
    size_t maxFileSize = 10 * 1024 * 1024; // 10MB
    size_t maxFiles = 10;
    bool enableCompression = false;
    bool enableRotation = true;
    std::chrono::minutes rotationInterval = std::chrono::minutes(60);
    bool flushImmediately = false;
};

// JSON file reporter with rotation support
class FileMetricsReporter : public MetricsReporter {
public:
    explicit FileMetricsReporter(const FileReporterConfig& config = FileReporterConfig{});
    ~FileMetricsReporter() override;
    
    // MetricsReporter interface
    bool Initialize(const std::string& outputDirectory) override;
    void Shutdown() override;
    void Report(const MetricsSnapshot& snapshot) override;
    std::string GetReporterType() const override { return "FileReporter"; }
    bool IsHealthy() const override { return m_healthy.load(); }
    std::vector<std::string> GetLastErrors() const override;
    void ClearErrors() override;
    uint64_t GetReportsGenerated() const override { return m_reportsGenerated.load(); }
    uint64_t GetReportsFailed() const override { return m_reportsFailed.load(); }
    
    // File-specific methods
    void ForceRotation();
    std::vector<std::string> GetGeneratedFiles() const;
    size_t GetCurrentFileSize() const;

private:
    void RotateFile();
    std::string GenerateFilename() const;
    std::string FormatSnapshot(const MetricsSnapshot& snapshot) const;
    void CompressFile(const std::string& filepath);
    void CleanupOldFiles();
    void ReportError(const std::string& error);

    FileReporterConfig m_config;
    std::string m_outputDirectory;
    std::string m_currentFilePath;
    std::unique_ptr<std::ofstream> m_currentFile;
    mutable std::mutex m_fileMutex;
    
    // Statistics and health
    std::atomic<bool> m_healthy{true};
    std::atomic<uint64_t> m_reportsGenerated{0};
    std::atomic<uint64_t> m_reportsFailed{0};
    std::atomic<size_t> m_currentFileSize{0};
    std::chrono::steady_clock::time_point m_lastRotation;
    
    // Error tracking
    mutable std::mutex m_errorMutex;
    std::vector<std::string> m_errors;
    static constexpr size_t MAX_ERRORS = 50;
    
    // File management
    std::vector<std::string> m_generatedFiles;
    mutable std::mutex m_fileListMutex;
};

// Configuration for CSV reporter
struct CSVReporterConfig {
    std::string filename = "metrics.csv";
    bool includeHeaders = true;
    char delimiter = ',';
    bool appendMode = true;
    std::vector<std::string> selectedFields; // Empty = all fields
};

// CSV export reporter for analysis tools
class CSVMetricsReporter : public MetricsReporter {
public:
    explicit CSVMetricsReporter(const CSVReporterConfig& config = CSVReporterConfig{});
    ~CSVMetricsReporter() override;
    
    // MetricsReporter interface
    bool Initialize(const std::string& outputDirectory) override;
    void Shutdown() override;
    void Report(const MetricsSnapshot& snapshot) override;
    std::string GetReporterType() const override { return "CSVReporter"; }
    bool IsHealthy() const override { return m_healthy.load(); }
    std::vector<std::string> GetLastErrors() const override;
    void ClearErrors() override;
    uint64_t GetReportsGenerated() const override { return m_reportsGenerated.load(); }
    uint64_t GetReportsFailed() const override { return m_reportsFailed.load(); }

private:
    void WriteHeaders();
    std::string FormatCSVRow(const MetricsSnapshot& snapshot) const;
    std::string EscapeCSVField(const std::string& field) const;
    void ReportError(const std::string& error);

    CSVReporterConfig m_config;
    std::string m_outputDirectory;
    std::string m_filePath;
    std::unique_ptr<std::ofstream> m_file;
    mutable std::mutex m_fileMutex;
    bool m_headersWritten = false;
    
    // Statistics and health
    std::atomic<bool> m_healthy{true};
    std::atomic<uint64_t> m_reportsGenerated{0};
    std::atomic<uint64_t> m_reportsFailed{0};
    
    // Error tracking
    mutable std::mutex m_errorMutex;
    std::vector<std::string> m_errors;
    static constexpr size_t MAX_ERRORS = 50;
};

// Configuration for in-memory reporter
struct MemoryReporterConfig {
    size_t maxSnapshots = 3600; // 1 hour at 1 second intervals
    bool enableStatistics = true;
    std::chrono::minutes retentionPeriod = std::chrono::minutes(60);
};

// In-memory circular buffer reporter for real-time queries
class MemoryMetricsReporter : public MetricsReporter {
public:
    explicit MemoryMetricsReporter(const MemoryReporterConfig& config = MemoryReporterConfig{});
    ~MemoryMetricsReporter() override = default;
    
    // MetricsReporter interface
    bool Initialize(const std::string& outputDirectory) override;
    void Shutdown() override;
    void Report(const MetricsSnapshot& snapshot) override;
    std::string GetReporterType() const override { return "MemoryReporter"; }
    bool IsHealthy() const override { return true; }
    uint64_t GetReportsGenerated() const override { return m_reportsGenerated.load(); }
    
    // Memory-specific methods
    std::vector<MetricsSnapshot> GetSnapshots(size_t count = 0) const;
    std::vector<MetricsSnapshot> GetSnapshotsInRange(
        std::chrono::system_clock::time_point start,
        std::chrono::system_clock::time_point end) const;
    MetricsSnapshot GetLatestSnapshot() const;
    void ClearSnapshots();
    size_t GetSnapshotCount() const;
    
    // Statistics
    struct Statistics {
        double avgCpuUsage = 0.0;
        uint64_t maxMemoryUsed = 0;
        uint64_t totalNetworkBytes = 0;
        uint64_t totalPacketsProcessed = 0;
        double avgLatency = 0.0;
        uint64_t totalSecurityViolations = 0;
    };
    Statistics GetStatistics() const;

private:
    void UpdateStatistics(const MetricsSnapshot& snapshot);
    void CleanupExpiredSnapshots();

    MemoryReporterConfig m_config;
    mutable std::mutex m_snapshotMutex;
    std::vector<MetricsSnapshot> m_snapshots;
    size_t m_writeIndex = 0;
    bool m_bufferFull = false;
    
    // Statistics
    std::atomic<uint64_t> m_reportsGenerated{0};
    mutable std::mutex m_statsMutex;
    Statistics m_statistics;
};

// Configuration for Prometheus HTTP endpoint
struct PrometheusReporterConfig {
    int port = 9090;
    std::string endpoint = "/metrics";
    std::string metricsPrefix = "rs2v_server";
    bool enableTimestamps = false;
    std::vector<std::string> excludeMetrics; // Metrics to exclude from export
    std::unordered_map<std::string, std::string> staticLabels; // Labels to add to all metrics
};

// Prometheus metrics exporter (HTTP endpoint)
class PrometheusMetricsReporter : public MetricsReporter {
public:
    explicit PrometheusMetricsReporter(const PrometheusReporterConfig& config = PrometheusReporterConfig{});
    ~PrometheusMetricsReporter() override;
    
    // MetricsReporter interface
    bool Initialize(const std::string& outputDirectory) override;
    void Shutdown() override;
    void Report(const MetricsSnapshot& snapshot) override;
    std::string GetReporterType() const override { return "PrometheusReporter"; }
    bool IsHealthy() const override { return m_healthy.load(); }
    std::vector<std::string> GetLastErrors() const override;
    void ClearErrors() override;
    uint64_t GetReportsGenerated() const override { return m_reportsGenerated.load(); }
    uint64_t GetReportsFailed() const override { return m_reportsFailed.load(); }
    
    // Prometheus-specific methods
    std::string GetMetricsEndpoint() const;
    int GetPort() const { return m_config.port; }

private:
    void StartHTTPServer();
    void StopHTTPServer();
    void HandleMetricsRequest(const std::string& request, std::string& response);
    std::string FormatPrometheusMetrics() const;
    std::string FormatMetricLine(const std::string& name, double value, 
                                const std::unordered_map<std::string, std::string>& labels = {}) const;
    void ReportError(const std::string& error);

    PrometheusReporterConfig m_config;
    std::atomic<bool> m_healthy{true};
    std::atomic<bool> m_serverRunning{false};
    std::thread m_serverThread;
    
    // Latest snapshot for HTTP responses
    mutable std::mutex m_snapshotMutex;
    MetricsSnapshot m_latestSnapshot;
    
    // Statistics
    std::atomic<uint64_t> m_reportsGenerated{0};
    std::atomic<uint64_t> m_reportsFailed{0};
    std::atomic<uint64_t> m_httpRequests{0};
    
    // Error tracking
    mutable std::mutex m_errorMutex;
    std::vector<std::string> m_errors;
    static constexpr size_t MAX_ERRORS = 50;
    
    // HTTP server (simplified implementation)
    int m_serverSocket = -1;
    void HTTPServerLoop();
    bool CreateServerSocket();
    void HandleClientConnection(int clientSocket);
};

// Configuration for alert-based reporter
struct AlertReporterConfig {
    struct AlertRule {
        std::string name;
        std::string metricPath; // e.g., "cpuUsagePercent", "activeConnections"
        enum Operator { GREATER_THAN, LESS_THAN, EQUAL, NOT_EQUAL } op;
        double threshold;
        std::chrono::seconds cooldownPeriod{300}; // 5 minutes default
        std::function<void(const std::string&, const MetricsSnapshot&)> callback;
    };
    
    std::vector<AlertRule> rules;
    bool enableEmailAlerts = false;
    std::string smtpServer;
    std::string emailRecipients;
};

// Alert-based reporter for threshold monitoring
class AlertMetricsReporter : public MetricsReporter {
public:
    explicit AlertMetricsReporter(const AlertReporterConfig& config = AlertReporterConfig{});
    ~AlertMetricsReporter() override = default;
    
    // MetricsReporter interface
    bool Initialize(const std::string& outputDirectory) override;
    void Shutdown() override;
    void Report(const MetricsSnapshot& snapshot) override;
    std::string GetReporterType() const override { return "AlertReporter"; }
    bool IsHealthy() const override { return true; }
    uint64_t GetReportsGenerated() const override { return m_reportsGenerated.load(); }
    
    // Alert-specific methods
    void AddAlertRule(const AlertReporterConfig::AlertRule& rule);
    void RemoveAlertRule(const std::string& name);
    std::vector<std::string> GetActiveAlerts() const;
    void ClearActiveAlerts();

private:
    double ExtractMetricValue(const MetricsSnapshot& snapshot, const std::string& metricPath) const;
    void EvaluateRules(const MetricsSnapshot& snapshot);
    void TriggerAlert(const std::string& ruleName, const MetricsSnapshot& snapshot);
    bool IsInCooldown(const std::string& ruleName) const;

    AlertReporterConfig m_config;
    std::atomic<uint64_t> m_reportsGenerated{0};
    
    // Alert state tracking
    mutable std::mutex m_alertMutex;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_lastAlertTimes;
    std::vector<std::string> m_activeAlerts;
};

// Factory functions for creating standard reporter configurations
namespace ReporterFactory {
    std::unique_ptr<MetricsReporter> CreateFileReporter(
        const std::string& prefix = "metrics",
        size_t maxFileSize = 10 * 1024 * 1024,
        size_t maxFiles = 10);
    
    std::unique_ptr<MetricsReporter> CreateCSVReporter(
        const std::string& filename = "metrics.csv");
    
    std::unique_ptr<MetricsReporter> CreateMemoryReporter(
        size_t maxSnapshots = 3600);
    
    std::unique_ptr<MetricsReporter> CreatePrometheusReporter(
        int port = 9090,
        const std::string& prefix = "rs2v_server");
    
    std::unique_ptr<MetricsReporter> CreateAlertReporter(
        const std::vector<AlertReporterConfig::AlertRule>& rules = {});
    
    // Create a standard set of reporters for production use
    std::vector<std::unique_ptr<MetricsReporter>> CreateStandardReporters();
}

} // namespace Telemetry