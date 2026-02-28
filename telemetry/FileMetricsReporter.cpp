// Server/telemetry/FileMetricsReporter.cpp
// Implementation of file-based JSON metrics reporter with rotation and compression
// for the RS2V server telemetry system

#include "MetricsReporter.h"
#include "TelemetryManager.h"
#include "Utils/Logger.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <regex>
#include <chrono>
#include <thread>

// Manual JSON serialization (nlohmann/json not available)

// Compression support
#ifdef ENABLE_COMPRESSION
    #include <zlib.h>
    #include <fstream>
#endif

namespace Telemetry {

FileMetricsReporter::FileMetricsReporter(const FileReporterConfig& config)
    : m_config(config), m_healthy(true), m_reportsGenerated(0), m_reportsFailed(0),
      m_currentFileSize(0) {
    Logger::Trace("[FileMetricsReporter::FileMetricsReporter] Entry: prefix='%s', maxFileSize=%zu, maxFiles=%zu",
                  config.filePrefix.c_str(), config.maxFileSize, config.maxFiles);
    m_lastRotation = std::chrono::steady_clock::now();

    // Validate configuration
    if (m_config.maxFileSize == 0) {
        m_config.maxFileSize = 10 * 1024 * 1024; // 10MB default
        Logger::Debug("[FileMetricsReporter::FileMetricsReporter] maxFileSize was 0, defaulting to 10MB");
    }
    if (m_config.maxFiles == 0) {
        m_config.maxFiles = 10; // Default to 10 files
        Logger::Debug("[FileMetricsReporter::FileMetricsReporter] maxFiles was 0, defaulting to 10");
    }
    if (m_config.filePrefix.empty()) {
        m_config.filePrefix = "metrics";
        Logger::Debug("[FileMetricsReporter::FileMetricsReporter] filePrefix was empty, defaulting to 'metrics'");
    }
    if (m_config.fileExtension.empty()) {
        m_config.fileExtension = ".json";
        Logger::Debug("[FileMetricsReporter::FileMetricsReporter] fileExtension was empty, defaulting to '.json'");
    }

    Logger::Info("FileMetricsReporter created with config: prefix=%s, maxSize=%zu, maxFiles=%zu",
                m_config.filePrefix.c_str(), m_config.maxFileSize, m_config.maxFiles);
    Logger::Trace("[FileMetricsReporter::FileMetricsReporter] Exit");
}

FileMetricsReporter::~FileMetricsReporter() {
    Logger::Trace("[FileMetricsReporter::~FileMetricsReporter] Entry");
    Shutdown();
    Logger::Trace("[FileMetricsReporter::~FileMetricsReporter] Exit");
}

bool FileMetricsReporter::Initialize(const std::string& outputDirectory) {
    Logger::Trace("[FileMetricsReporter::Initialize] Entry: outputDirectory='%s'", outputDirectory.c_str());
    std::lock_guard<std::mutex> lock(m_fileMutex);

    try {
        m_outputDirectory = outputDirectory;

        // Create output directory if it doesn't exist
        if (!std::filesystem::exists(m_outputDirectory)) {
            Logger::Debug("[FileMetricsReporter::Initialize] Output directory does not exist, creating it");
            std::filesystem::create_directories(m_outputDirectory);
            Logger::Info("Created metrics output directory: %s", m_outputDirectory.c_str());
        } else {
            Logger::Debug("[FileMetricsReporter::Initialize] Output directory already exists: '%s'", m_outputDirectory.c_str());
        }

        // Validate directory is writable
        std::string testFile = m_outputDirectory + "/.write_test";
        std::ofstream test(testFile);
        if (!test.is_open()) {
            Logger::Error("[FileMetricsReporter::Initialize] Output directory is not writable: '%s'", m_outputDirectory.c_str());
            ReportError("Output directory is not writable: " + m_outputDirectory);
            m_healthy = false;
            Logger::Trace("[FileMetricsReporter::Initialize] Exit: returning false (not writable)");
            return false;
        }
        test.close();
        std::filesystem::remove(testFile);
        Logger::Debug("[FileMetricsReporter::Initialize] Directory write test passed");

        // Scan for existing files to populate file list
        Logger::Debug("[FileMetricsReporter::Initialize] Scanning for existing metric files");
        ScanExistingFiles();

        // Create initial file
        if (!CreateNewFile()) {
            Logger::Error("[FileMetricsReporter::Initialize] Failed to create initial metrics file");
            ReportError("Failed to create initial metrics file");
            m_healthy = false;
            Logger::Trace("[FileMetricsReporter::Initialize] Exit: returning false (file creation failed)");
            return false;
        }

        m_healthy = true;
        Logger::Info("FileMetricsReporter initialized successfully in directory: %s",
                    m_outputDirectory.c_str());
        Logger::Trace("[FileMetricsReporter::Initialize] Exit: returning true");
        return true;

    } catch (const std::exception& ex) {
        Logger::Error("[FileMetricsReporter::Initialize] Exception during initialization: %s", ex.what());
        ReportError("Failed to initialize FileMetricsReporter: " + std::string(ex.what()));
        m_healthy = false;
        Logger::Trace("[FileMetricsReporter::Initialize] Exit: returning false (exception)");
        return false;
    }
}

void FileMetricsReporter::Shutdown() {
    Logger::Trace("[FileMetricsReporter::Shutdown] Entry");
    std::lock_guard<std::mutex> lock(m_fileMutex);

    if (m_currentFile && m_currentFile->is_open()) {
        Logger::Debug("[FileMetricsReporter::Shutdown] Current file is open, writing footer and closing");
        try {
            // Write final metadata
            WriteFileFooter();
            m_currentFile->flush();
            m_currentFile->close();
            Logger::Debug("[FileMetricsReporter::Shutdown] File flushed and closed");

            // Compress final file if enabled
            if (m_config.enableCompression && !m_currentFilePath.empty()) {
                Logger::Debug("[FileMetricsReporter::Shutdown] Compressing final file: '%s'", m_currentFilePath.c_str());
                CompressFile(m_currentFilePath);
            }

        } catch (const std::exception& ex) {
            Logger::Error("Error during FileMetricsReporter shutdown: %s", ex.what());
        }
    } else {
        Logger::Debug("[FileMetricsReporter::Shutdown] No current file open");
    }

    m_currentFile.reset();
    m_currentFilePath.clear();

    Logger::Info("FileMetricsReporter shutdown complete. Generated %llu reports total.",
                m_reportsGenerated.load());
    Logger::Trace("[FileMetricsReporter::Shutdown] Exit");
}

void FileMetricsReporter::Report(const MetricsSnapshot& snapshot) {
    Logger::Trace("[FileMetricsReporter::Report] Entry");
    std::lock_guard<std::mutex> lock(m_fileMutex);

    if (!m_healthy.load()) {
        Logger::Debug("[FileMetricsReporter::Report] Reporter is unhealthy, skipping report");
        m_reportsFailed.fetch_add(1, std::memory_order_relaxed);
        Logger::Trace("[FileMetricsReporter::Report] Exit: skipped (unhealthy)");
        return;
    }

    try {
        // Check if we need to rotate the file
        bool shouldRotate = false;

        if (m_config.enableRotation) {
            auto now = std::chrono::steady_clock::now();
            auto timeSinceRotation = std::chrono::duration_cast<std::chrono::minutes>(now - m_lastRotation);

            shouldRotate = (m_currentFileSize.load() >= m_config.maxFileSize) ||
                          (timeSinceRotation >= m_config.rotationInterval);

            if (shouldRotate) {
                Logger::Debug("[FileMetricsReporter::Report] Rotation needed: fileSize=%zu, maxSize=%zu, timeSinceRotation=%lld min",
                              m_currentFileSize.load(), m_config.maxFileSize, (long long)timeSinceRotation.count());
            }
        } else {
            Logger::Debug("[FileMetricsReporter::Report] Rotation is disabled");
        }

        if (shouldRotate) {
            Logger::Info("[FileMetricsReporter::Report] Triggering file rotation");
            RotateFile();
        }

        // Ensure we have a valid file
        if (!m_currentFile || !m_currentFile->is_open()) {
            Logger::Debug("[FileMetricsReporter::Report] No valid file, creating new one");
            if (!CreateNewFile()) {
                Logger::Error("[FileMetricsReporter::Report] Failed to create new metrics file");
                ReportError("Failed to create new metrics file");
                m_reportsFailed.fetch_add(1, std::memory_order_relaxed);
                Logger::Trace("[FileMetricsReporter::Report] Exit: failed (no file)");
                return;
            }
        }

        // Format and write the snapshot
        std::string jsonData = FormatSnapshot(snapshot);
        size_t dataSize = jsonData.length();
        Logger::Debug("[FileMetricsReporter::Report] Formatted snapshot: %zu bytes", dataSize);

        *m_currentFile << jsonData;
        if (!jsonData.empty() && jsonData.back() != '\n') {
            *m_currentFile << "\n";
            dataSize++;
        }

        if (m_config.flushImmediately) {
            m_currentFile->flush();
            Logger::Debug("[FileMetricsReporter::Report] Flushed file immediately");
        }

        // Update statistics
        m_currentFileSize.fetch_add(dataSize, std::memory_order_relaxed);
        m_reportsGenerated.fetch_add(1, std::memory_order_relaxed);
        Logger::Debug("[FileMetricsReporter::Report] Report #%llu written, currentFileSize=%zu",
                      m_reportsGenerated.load(), m_currentFileSize.load());

        // Check for write errors
        if (m_currentFile->fail()) {
            Logger::Error("[FileMetricsReporter::Report] File write error occurred");
            ReportError("File write error occurred");
            m_reportsFailed.fetch_add(1, std::memory_order_relaxed);
            m_healthy = false;
        }

    } catch (const std::exception& ex) {
        Logger::Error("[FileMetricsReporter::Report] Exception writing metrics: %s", ex.what());
        ReportError("Error writing metrics report: " + std::string(ex.what()));
        m_reportsFailed.fetch_add(1, std::memory_order_relaxed);
    }
    Logger::Trace("[FileMetricsReporter::Report] Exit");
}

std::vector<std::string> FileMetricsReporter::GetLastErrors() const {
    Logger::Trace("[FileMetricsReporter::GetLastErrors] Entry");
    std::lock_guard<std::mutex> lock(m_errorMutex);
    Logger::Debug("[FileMetricsReporter::GetLastErrors] Returning %zu errors", m_errors.size());
    Logger::Trace("[FileMetricsReporter::GetLastErrors] Exit: returning %zu errors", m_errors.size());
    return m_errors;
}

void FileMetricsReporter::ClearErrors() {
    Logger::Trace("[FileMetricsReporter::ClearErrors] Entry");
    std::lock_guard<std::mutex> lock(m_errorMutex);
    size_t count = m_errors.size();
    m_errors.clear();
    m_healthy = true;
    Logger::Info("[FileMetricsReporter::ClearErrors] Cleared %zu errors, reset healthy flag", count);
    Logger::Trace("[FileMetricsReporter::ClearErrors] Exit");
}

void FileMetricsReporter::ForceRotation() {
    Logger::Trace("[FileMetricsReporter::ForceRotation] Entry");
    std::lock_guard<std::mutex> lock(m_fileMutex);
    Logger::Info("[FileMetricsReporter::ForceRotation] Forcing file rotation");
    RotateFile();
    Logger::Trace("[FileMetricsReporter::ForceRotation] Exit");
}

std::vector<std::string> FileMetricsReporter::GetGeneratedFiles() const {
    Logger::Trace("[FileMetricsReporter::GetGeneratedFiles] Entry");
    std::lock_guard<std::mutex> lock(m_fileListMutex);
    Logger::Debug("[FileMetricsReporter::GetGeneratedFiles] Returning %zu files", m_generatedFiles.size());
    Logger::Trace("[FileMetricsReporter::GetGeneratedFiles] Exit: returning %zu files", m_generatedFiles.size());
    return m_generatedFiles;
}

size_t FileMetricsReporter::GetCurrentFileSize() const {
    Logger::Trace("[FileMetricsReporter::GetCurrentFileSize] Entry");
    size_t size = m_currentFileSize.load();
    Logger::Trace("[FileMetricsReporter::GetCurrentFileSize] Exit: returning %zu", size);
    return size;
}

void FileMetricsReporter::RotateFile() {
    Logger::Trace("[FileMetricsReporter::RotateFile] Entry");
    try {
        // Close current file
        if (m_currentFile && m_currentFile->is_open()) {
            Logger::Debug("[FileMetricsReporter::RotateFile] Closing current file: '%s'", m_currentFilePath.c_str());
            WriteFileFooter();
            m_currentFile->flush();
            m_currentFile->close();

            // Compress the completed file if enabled
            if (m_config.enableCompression && !m_currentFilePath.empty()) {
                Logger::Debug("[FileMetricsReporter::RotateFile] Compressing completed file: '%s'", m_currentFilePath.c_str());
                CompressFile(m_currentFilePath);
            }
        } else {
            Logger::Debug("[FileMetricsReporter::RotateFile] No current file to close");
        }

        // Create new file
        if (!CreateNewFile()) {
            Logger::Error("[FileMetricsReporter::RotateFile] Failed to create new file during rotation");
            ReportError("Failed to create new file during rotation");
            Logger::Trace("[FileMetricsReporter::RotateFile] Exit: failed");
            return;
        }

        // Cleanup old files
        Logger::Debug("[FileMetricsReporter::RotateFile] Cleaning up old files");
        CleanupOldFiles();

        m_lastRotation = std::chrono::steady_clock::now();

        Logger::Info("FileMetricsReporter rotated to new file: %s",
                    m_currentFilePath.c_str());

    } catch (const std::exception& ex) {
        Logger::Error("[FileMetricsReporter::RotateFile] Exception during rotation: %s", ex.what());
        ReportError("Error during file rotation: " + std::string(ex.what()));
    }
    Logger::Trace("[FileMetricsReporter::RotateFile] Exit");
}

std::string FileMetricsReporter::GenerateFilename() const {
    Logger::Trace("[FileMetricsReporter::GenerateFilename] Entry");
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::ostringstream filename;
    filename << m_config.filePrefix << "_"
             << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S")
             << "_" << std::setfill('0') << std::setw(3) << ms.count()
             << m_config.fileExtension;

    std::string result = m_outputDirectory + "/" + filename.str();
    Logger::Debug("[FileMetricsReporter::GenerateFilename] Generated filename: '%s'", result.c_str());
    Logger::Trace("[FileMetricsReporter::GenerateFilename] Exit: returning '%s'", result.c_str());
    return result;
}

bool FileMetricsReporter::CreateNewFile() {
    Logger::Trace("[FileMetricsReporter::CreateNewFile] Entry");
    try {
        m_currentFilePath = GenerateFilename();
        Logger::Debug("[FileMetricsReporter::CreateNewFile] Creating file: '%s'", m_currentFilePath.c_str());
        m_currentFile = std::make_unique<std::ofstream>(m_currentFilePath,
                                                       std::ios::out | std::ios::trunc);

        if (!m_currentFile->is_open()) {
            Logger::Error("[FileMetricsReporter::CreateNewFile] Failed to open new metrics file: '%s'", m_currentFilePath.c_str());
            ReportError("Failed to open new metrics file: " + m_currentFilePath);
            Logger::Trace("[FileMetricsReporter::CreateNewFile] Exit: returning false (open failed)");
            return false;
        }

        // Write file header
        Logger::Debug("[FileMetricsReporter::CreateNewFile] Writing file header");
        WriteFileHeader();

        // Add to generated files list
        {
            std::lock_guard<std::mutex> lock(m_fileListMutex);
            m_generatedFiles.push_back(m_currentFilePath);
            Logger::Debug("[FileMetricsReporter::CreateNewFile] Added to generated files list, total=%zu", m_generatedFiles.size());
        }

        m_currentFileSize = 0;
        Logger::Info("[FileMetricsReporter::CreateNewFile] Successfully created new metrics file: '%s'", m_currentFilePath.c_str());
        Logger::Trace("[FileMetricsReporter::CreateNewFile] Exit: returning true");
        return true;

    } catch (const std::exception& ex) {
        Logger::Error("[FileMetricsReporter::CreateNewFile] Exception creating file: %s", ex.what());
        ReportError("Exception creating new file: " + std::string(ex.what()));
        Logger::Trace("[FileMetricsReporter::CreateNewFile] Exit: returning false (exception)");
        return false;
    }
}

void FileMetricsReporter::WriteFileHeader() {
    Logger::Trace("[FileMetricsReporter::WriteFileHeader] Entry");
    if (!m_currentFile || !m_currentFile->is_open()) {
        Logger::Warn("[FileMetricsReporter::WriteFileHeader] No file open, skipping header write");
        Logger::Trace("[FileMetricsReporter::WriteFileHeader] Exit: skipped (no file)");
        return;
    }

    try {
        auto createdAt = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::ostringstream header;
        header << "{\"file_format\":\"rs2v_telemetry_v1\""
               << ",\"created_at\":" << createdAt
               << ",\"generator\":\"FileMetricsReporter\""
               << ",\"server_version\":\"1.0.0\""
               << ",\"format_description\":\"Each line after this header is a JSON metrics snapshot\""
               << "}";

        std::string headerStr = header.str();
        *m_currentFile << "# " << headerStr << "\n";
        m_currentFileSize.fetch_add(headerStr.length() + 3, std::memory_order_relaxed);
        Logger::Debug("[FileMetricsReporter::WriteFileHeader] Wrote header (%zu bytes)", headerStr.length() + 3);

    } catch (const std::exception& ex) {
        Logger::Error("[FileMetricsReporter::WriteFileHeader] Error writing file header: %s", ex.what());
        ReportError("Error writing file header: " + std::string(ex.what()));
    }
    Logger::Trace("[FileMetricsReporter::WriteFileHeader] Exit");
}

void FileMetricsReporter::WriteFileFooter() {
    Logger::Trace("[FileMetricsReporter::WriteFileFooter] Entry");
    if (!m_currentFile || !m_currentFile->is_open()) {
        Logger::Warn("[FileMetricsReporter::WriteFileFooter] No file open, skipping footer write");
        Logger::Trace("[FileMetricsReporter::WriteFileFooter] Exit: skipped (no file)");
        return;
    }

    try {
        auto closedAt = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::ostringstream footer;
        footer << "{\"file_closed_at\":" << closedAt
               << ",\"total_snapshots\":" << m_reportsGenerated.load()
               << ",\"file_size_bytes\":" << m_currentFileSize.load()
               << "}";

        *m_currentFile << "# " << footer.str() << "\n";
        Logger::Debug("[FileMetricsReporter::WriteFileFooter] Wrote footer (closedAt=%lld, snapshots=%llu, size=%zu)",
                      (long long)closedAt, m_reportsGenerated.load(), m_currentFileSize.load());

    } catch (const std::exception& ex) {
        Logger::Error("Error writing file footer: %s", ex.what());
    }
    Logger::Trace("[FileMetricsReporter::WriteFileFooter] Exit");
}

std::string FileMetricsReporter::FormatSnapshot(const MetricsSnapshot& snapshot) const {
    Logger::Trace("[FileMetricsReporter::FormatSnapshot] Entry");
    try {
        auto ts = std::chrono::duration_cast<std::chrono::seconds>(
            snapshot.timestamp.time_since_epoch()).count();
        auto ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            snapshot.timestamp.time_since_epoch()).count();

        std::ostringstream j;
        j << std::fixed << std::setprecision(4);
        j << "{\"timestamp\":" << ts
          << ",\"timestamp_ms\":" << ts_ms;

        // System metrics
        j << ",\"system\":{";
        if (snapshot.cpuUsagePercent >= 0) {
            j << "\"cpu_usage_percent\":" << snapshot.cpuUsagePercent << ",";
            Logger::Debug("[FileMetricsReporter::FormatSnapshot] CPU usage: %.2f%%", snapshot.cpuUsagePercent);
        }
        if (snapshot.memoryUsedBytes > 0) {
            double memPct = snapshot.memoryTotalBytes > 0 ?
                (static_cast<double>(snapshot.memoryUsedBytes) / snapshot.memoryTotalBytes) * 100.0 : 0.0;
            j << "\"memory_used_bytes\":" << snapshot.memoryUsedBytes
              << ",\"memory_total_bytes\":" << snapshot.memoryTotalBytes
              << ",\"memory_usage_percent\":" << memPct << ",";
        }
        j << "\"network_bytes_sent\":" << snapshot.networkBytesSent
          << ",\"network_bytes_received\":" << snapshot.networkBytesReceived
          << ",\"disk_read_bytes\":" << snapshot.diskReadBytes
          << ",\"disk_write_bytes\":" << snapshot.diskWriteBytes
          << "}";

        // Network metrics
        j << ",\"network\":{\"active_connections\":" << snapshot.activeConnections
          << ",\"authenticated_players\":" << snapshot.authenticatedPlayers
          << ",\"total_packets_processed\":" << snapshot.totalPacketsProcessed
          << ",\"total_packets_dropped\":" << snapshot.totalPacketsDropped
          << ",\"average_latency_ms\":" << snapshot.averageLatencyMs
          << ",\"packet_loss_rate\":" << snapshot.packetLossRate;
        if (snapshot.totalPacketsProcessed > 0) {
            j << ",\"packet_loss_percent\":" <<
                (static_cast<double>(snapshot.totalPacketsDropped) / snapshot.totalPacketsProcessed) * 100.0;
        }
        j << "}";

        // Gameplay metrics
        j << ",\"gameplay\":{\"current_tick\":" << snapshot.currentTick
          << ",\"active_matches\":" << snapshot.activeMatches
          << ",\"total_kills\":" << snapshot.totalKills
          << ",\"total_deaths\":" << snapshot.totalDeaths
          << ",\"objectives_captured\":" << snapshot.objectivesCaptured
          << ",\"chat_messages_sent\":" << snapshot.chatMessagesSent;
        if (snapshot.totalKills + snapshot.totalDeaths > 0) {
            double kd = snapshot.totalDeaths > 0 ?
                static_cast<double>(snapshot.totalKills) / snapshot.totalDeaths :
                static_cast<double>(snapshot.totalKills);
            j << ",\"kill_death_ratio\":" << kd;
        }
        j << "}";

        // Performance metrics
        j << ",\"performance\":{\"frame_time_ms\":" << snapshot.frameTimeMs
          << ",\"physics_time_ms\":" << snapshot.physicsTimeMs
          << ",\"network_time_ms\":" << snapshot.networkTimeMs
          << ",\"game_logic_time_ms\":" << snapshot.gameLogicTimeMs
          << "}";

        // Security metrics
        j << ",\"security\":{\"security_violations\":" << snapshot.securityViolations
          << ",\"malformed_packets\":" << snapshot.malformedPackets
          << ",\"speed_hack_detections\":" << snapshot.speedHackDetections
          << ",\"kicked_players\":" << snapshot.kickedPlayers
          << ",\"banned_players\":" << snapshot.bannedPlayers
          << "}}";

        std::string result = j.str();
        Logger::Debug("[FileMetricsReporter::FormatSnapshot] Formatted snapshot: %zu bytes, timestamp=%lld",
                      result.size(), (long long)ts);
        Logger::Trace("[FileMetricsReporter::FormatSnapshot] Exit: returning JSON (%zu bytes)", result.size());
        return result;

    } catch (const std::exception& ex) {
        Logger::Error("[FileMetricsReporter::FormatSnapshot] Exception formatting snapshot: %s", ex.what());
        ReportError("Error formatting snapshot to JSON: " + std::string(ex.what()));
        Logger::Trace("[FileMetricsReporter::FormatSnapshot] Exit: returning '{}' (error)");
        return "{}";
    }
}

void FileMetricsReporter::CompressFile(const std::string& filepath) {
    Logger::Trace("[FileMetricsReporter::CompressFile] Entry: filepath='%s'", filepath.c_str());
#ifdef ENABLE_COMPRESSION
    try {
        std::string compressedPath = filepath + ".gz";
        Logger::Debug("[FileMetricsReporter::CompressFile] Compressing to: '%s'", compressedPath.c_str());

        std::ifstream input(filepath, std::ios::binary);
        if (!input.is_open()) {
            Logger::Error("[FileMetricsReporter::CompressFile] Failed to open file for compression: '%s'", filepath.c_str());
            ReportError("Failed to open file for compression: " + filepath);
            Logger::Trace("[FileMetricsReporter::CompressFile] Exit: failed (input open failed)");
            return;
        }

        gzFile output = gzopen(compressedPath.c_str(), "wb");
        if (output == nullptr) {
            Logger::Error("[FileMetricsReporter::CompressFile] Failed to create compressed file: '%s'", compressedPath.c_str());
            ReportError("Failed to create compressed file: " + compressedPath);
            Logger::Trace("[FileMetricsReporter::CompressFile] Exit: failed (gzopen failed)");
            return;
        }

        char buffer[8192];
        size_t totalBytesWritten = 0;
        while (input.read(buffer, sizeof(buffer)) || input.gcount() > 0) {
            gzwrite(output, buffer, static_cast<unsigned int>(input.gcount()));
            totalBytesWritten += input.gcount();
        }

        input.close();
        gzclose(output);
        Logger::Debug("[FileMetricsReporter::CompressFile] Compressed %zu bytes", totalBytesWritten);

        // Remove original file
        std::filesystem::remove(filepath);
        Logger::Debug("[FileMetricsReporter::CompressFile] Removed original file: '%s'", filepath.c_str());

        // Update file list
        {
            std::lock_guard<std::mutex> lock(m_fileListMutex);
            auto it = std::find(m_generatedFiles.begin(), m_generatedFiles.end(), filepath);
            if (it != m_generatedFiles.end()) {
                *it = compressedPath;
                Logger::Debug("[FileMetricsReporter::CompressFile] Updated file list entry to compressed path");
            } else {
                Logger::Debug("[FileMetricsReporter::CompressFile] Original file not found in generated files list");
            }
        }

        Logger::Info("Compressed metrics file: %s -> %s", filepath.c_str(), compressedPath.c_str());

    } catch (const std::exception& ex) {
        Logger::Error("[FileMetricsReporter::CompressFile] Exception compressing file: %s", ex.what());
        ReportError("Error compressing file: " + std::string(ex.what()));
    }
#else
    Logger::Debug("Compression not enabled for file: %s", filepath.c_str());
#endif
    Logger::Trace("[FileMetricsReporter::CompressFile] Exit");
}

void FileMetricsReporter::CleanupOldFiles() {
    Logger::Trace("[FileMetricsReporter::CleanupOldFiles] Entry");
    try {
        std::lock_guard<std::mutex> lock(m_fileListMutex);

        if (m_generatedFiles.size() <= m_config.maxFiles) {
            Logger::Debug("[FileMetricsReporter::CleanupOldFiles] No cleanup needed: %zu files <= maxFiles=%zu",
                          m_generatedFiles.size(), m_config.maxFiles);
            Logger::Trace("[FileMetricsReporter::CleanupOldFiles] Exit: no cleanup needed");
            return; // No cleanup needed
        }

        // Sort files by creation time (oldest first)
        std::sort(m_generatedFiles.begin(), m_generatedFiles.end());

        size_t filesToRemove = m_generatedFiles.size() - m_config.maxFiles;
        Logger::Info("[FileMetricsReporter::CleanupOldFiles] Removing %zu old files (total=%zu, max=%zu)",
                     filesToRemove, m_generatedFiles.size(), m_config.maxFiles);

        for (size_t i = 0; i < filesToRemove; ++i) {
            const std::string& fileToRemove = m_generatedFiles[i];

            try {
                if (std::filesystem::exists(fileToRemove)) {
                    std::filesystem::remove(fileToRemove);
                    Logger::Info("Removed old metrics file: %s", fileToRemove.c_str());
                } else {
                    Logger::Debug("[FileMetricsReporter::CleanupOldFiles] File does not exist, skipping: '%s'", fileToRemove.c_str());
                }
            } catch (const std::exception& ex) {
                Logger::Error("Failed to remove old metrics file %s: %s",
                             fileToRemove.c_str(), ex.what());
            }
        }

        // Remove from tracking list
        m_generatedFiles.erase(m_generatedFiles.begin(), m_generatedFiles.begin() + filesToRemove);
        Logger::Debug("[FileMetricsReporter::CleanupOldFiles] Remaining files in tracking list: %zu", m_generatedFiles.size());

    } catch (const std::exception& ex) {
        Logger::Error("[FileMetricsReporter::CleanupOldFiles] Exception during cleanup: %s", ex.what());
        ReportError("Error during old file cleanup: " + std::string(ex.what()));
    }
    Logger::Trace("[FileMetricsReporter::CleanupOldFiles] Exit");
}

void FileMetricsReporter::ScanExistingFiles() {
    Logger::Trace("[FileMetricsReporter::ScanExistingFiles] Entry");
    try {
        std::lock_guard<std::mutex> lock(m_fileListMutex);
        m_generatedFiles.clear();

        if (!std::filesystem::exists(m_outputDirectory)) {
            Logger::Debug("[FileMetricsReporter::ScanExistingFiles] Output directory does not exist: '%s'", m_outputDirectory.c_str());
            Logger::Trace("[FileMetricsReporter::ScanExistingFiles] Exit: directory not found");
            return;
        }

        // Create regex pattern to match our metric files
        std::string pattern = m_config.filePrefix + R"(_\d{8}_\d{6}_\d{3})" +
                             std::regex_replace(m_config.fileExtension, std::regex(R"(\.)"), R"(\.)") +
                             R"((?:\.gz)?)";
        std::regex fileRegex(pattern);
        Logger::Debug("[FileMetricsReporter::ScanExistingFiles] Scanning with pattern: '%s'", pattern.c_str());

        for (const auto& entry : std::filesystem::directory_iterator(m_outputDirectory)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                if (std::regex_match(filename, fileRegex)) {
                    m_generatedFiles.push_back(entry.path().string());
                    Logger::Debug("[FileMetricsReporter::ScanExistingFiles] Found matching file: '%s'", filename.c_str());
                }
            }
        }

        // Sort by filename (which includes timestamp)
        std::sort(m_generatedFiles.begin(), m_generatedFiles.end());

        Logger::Info("Found %zu existing metrics files in directory", m_generatedFiles.size());

    } catch (const std::exception& ex) {
        Logger::Error("Error scanning existing files: %s", ex.what());
    }
    Logger::Trace("[FileMetricsReporter::ScanExistingFiles] Exit");
}

void FileMetricsReporter::ReportError(const std::string& error) const {
    Logger::Trace("[FileMetricsReporter::ReportError] Entry: error='%s'", error.c_str());
    std::lock_guard<std::mutex> lock(m_errorMutex);

    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);

    std::ostringstream oss;
    oss << "[" << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S") << "] " << error;

    m_errors.push_back(oss.str());

    // Limit error history
    if (m_errors.size() > MAX_ERRORS) {
        Logger::Debug("[FileMetricsReporter::ReportError] Error history exceeds MAX_ERRORS=%zu, trimming oldest", MAX_ERRORS);
        m_errors.erase(m_errors.begin());
    }

    Logger::Error("FileMetricsReporter: %s", error.c_str());
    m_healthy = false;
    Logger::Trace("[FileMetricsReporter::ReportError] Exit");
}

// Factory function implementation
namespace ReporterFactory {

std::unique_ptr<MetricsReporter> CreateFileReporter(
    const std::string& prefix,
    size_t maxFileSize,
    size_t maxFiles) {

    Logger::Trace("[ReporterFactory::CreateFileReporter] Entry: prefix='%s', maxFileSize=%zu, maxFiles=%zu",
                  prefix.c_str(), maxFileSize, maxFiles);
    FileReporterConfig config;
    config.filePrefix = prefix;
    config.maxFileSize = maxFileSize;
    config.maxFiles = maxFiles;
    config.enableRotation = true;
    config.flushImmediately = false;

    Logger::Debug("[ReporterFactory::CreateFileReporter] Config: enableRotation=true, flushImmediately=false");
    auto reporter = std::make_unique<FileMetricsReporter>(config);
    Logger::Info("[ReporterFactory::CreateFileReporter] Created FileMetricsReporter with prefix='%s'", prefix.c_str());
    Logger::Trace("[ReporterFactory::CreateFileReporter] Exit: returning FileMetricsReporter");
    return reporter;
}

} // namespace ReporterFactory

} // namespace Telemetry
