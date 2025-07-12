// Server/telemetry/FileMetricsReporter.cpp
// Implementation of file-based JSON metrics reporter with rotation and compression
// for the RS2V server telemetry system

#include "MetricsReporter.h"
#include "TelemetryManager.h"
#include "../Utils/Logger.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <regex>
#include <chrono>
#include <thread>

// JSON library - using nlohmann/json for structured output
#include <nlohmann/json.hpp>

// Compression support
#ifdef ENABLE_COMPRESSION
    #include <zlib.h>
    #include <fstream>
#endif

namespace Telemetry {

FileMetricsReporter::FileMetricsReporter(const FileReporterConfig& config)
    : m_config(config), m_healthy(true), m_reportsGenerated(0), m_reportsFailed(0), 
      m_currentFileSize(0) {
    m_lastRotation = std::chrono::steady_clock::now();
    
    // Validate configuration
    if (m_config.maxFileSize == 0) {
        m_config.maxFileSize = 10 * 1024 * 1024; // 10MB default
    }
    if (m_config.maxFiles == 0) {
        m_config.maxFiles = 10; // Default to 10 files
    }
    if (m_config.filePrefix.empty()) {
        m_config.filePrefix = "metrics";
    }
    if (m_config.fileExtension.empty()) {
        m_config.fileExtension = ".json";
    }
    
    Logger::Info("FileMetricsReporter created with config: prefix=%s, maxSize=%zu, maxFiles=%zu",
                m_config.filePrefix.c_str(), m_config.maxFileSize, m_config.maxFiles);
}

FileMetricsReporter::~FileMetricsReporter() {
    Shutdown();
}

bool FileMetricsReporter::Initialize(const std::string& outputDirectory) {
    std::lock_guard<std::mutex> lock(m_fileMutex);
    
    try {
        m_outputDirectory = outputDirectory;
        
        // Create output directory if it doesn't exist
        if (!std::filesystem::exists(m_outputDirectory)) {
            std::filesystem::create_directories(m_outputDirectory);
            Logger::Info("Created metrics output directory: %s", m_outputDirectory.c_str());
        }
        
        // Validate directory is writable
        std::string testFile = m_outputDirectory + "/.write_test";
        std::ofstream test(testFile);
        if (!test.is_open()) {
            ReportError("Output directory is not writable: " + m_outputDirectory);
            m_healthy = false;
            return false;
        }
        test.close();
        std::filesystem::remove(testFile);
        
        // Scan for existing files to populate file list
        ScanExistingFiles();
        
        // Create initial file
        if (!CreateNewFile()) {
            ReportError("Failed to create initial metrics file");
            m_healthy = false;
            return false;
        }
        
        m_healthy = true;
        Logger::Info("FileMetricsReporter initialized successfully in directory: %s", 
                    m_outputDirectory.c_str());
        return true;
        
    } catch (const std::exception& ex) {
        ReportError("Failed to initialize FileMetricsReporter: " + std::string(ex.what()));
        m_healthy = false;
        return false;
    }
}

void FileMetricsReporter::Shutdown() {
    std::lock_guard<std::mutex> lock(m_fileMutex);
    
    if (m_currentFile && m_currentFile->is_open()) {
        try {
            // Write final metadata
            WriteFileFooter();
            m_currentFile->flush();
            m_currentFile->close();
            
            // Compress final file if enabled
            if (m_config.enableCompression && !m_currentFilePath.empty()) {
                CompressFile(m_currentFilePath);
            }
            
        } catch (const std::exception& ex) {
            Logger::Error("Error during FileMetricsReporter shutdown: %s", ex.what());
        }
    }
    
    m_currentFile.reset();
    m_currentFilePath.clear();
    
    Logger::Info("FileMetricsReporter shutdown complete. Generated %llu reports total.",
                m_reportsGenerated.load());
}

void FileMetricsReporter::Report(const MetricsSnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(m_fileMutex);
    
    if (!m_healthy.load()) {
        m_reportsFailed.fetch_add(1, std::memory_order_relaxed);
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
        }
        
        if (shouldRotate) {
            RotateFile();
        }
        
        // Ensure we have a valid file
        if (!m_currentFile || !m_currentFile->is_open()) {
            if (!CreateNewFile()) {
                ReportError("Failed to create new metrics file");
                m_reportsFailed.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        
        // Format and write the snapshot
        std::string jsonData = FormatSnapshot(snapshot);
        size_t dataSize = jsonData.length();
        
        *m_currentFile << jsonData;
        if (!jsonData.empty() && jsonData.back() != '\n') {
            *m_currentFile << "\n";
            dataSize++;
        }
        
        if (m_config.flushImmediately) {
            m_currentFile->flush();
        }
        
        // Update statistics
        m_currentFileSize.fetch_add(dataSize, std::memory_order_relaxed);
        m_reportsGenerated.fetch_add(1, std::memory_order_relaxed);
        
        // Check for write errors
        if (m_currentFile->fail()) {
            ReportError("File write error occurred");
            m_reportsFailed.fetch_add(1, std::memory_order_relaxed);
            m_healthy = false;
        }
        
    } catch (const std::exception& ex) {
        ReportError("Error writing metrics report: " + std::string(ex.what()));
        m_reportsFailed.fetch_add(1, std::memory_order_relaxed);
    }
}

std::vector<std::string> FileMetricsReporter::GetLastErrors() const {
    std::lock_guard<std::mutex> lock(m_errorMutex);
    return m_errors;
}

void FileMetricsReporter::ClearErrors() {
    std::lock_guard<std::mutex> lock(m_errorMutex);
    m_errors.clear();
    m_healthy = true;
}

void FileMetricsReporter::ForceRotation() {
    std::lock_guard<std::mutex> lock(m_fileMutex);
    RotateFile();
}

std::vector<std::string> FileMetricsReporter::GetGeneratedFiles() const {
    std::lock_guard<std::mutex> lock(m_fileListMutex);
    return m_generatedFiles;
}

size_t FileMetricsReporter::GetCurrentFileSize() const {
    return m_currentFileSize.load();
}

void FileMetricsReporter::RotateFile() {
    try {
        // Close current file
        if (m_currentFile && m_currentFile->is_open()) {
            WriteFileFooter();
            m_currentFile->flush();
            m_currentFile->close();
            
            // Compress the completed file if enabled
            if (m_config.enableCompression && !m_currentFilePath.empty()) {
                CompressFile(m_currentFilePath);
            }
        }
        
        // Create new file
        if (!CreateNewFile()) {
            ReportError("Failed to create new file during rotation");
            return;
        }
        
        // Cleanup old files
        CleanupOldFiles();
        
        m_lastRotation = std::chrono::steady_clock::now();
        
        Logger::Info("FileMetricsReporter rotated to new file: %s", 
                    m_currentFilePath.c_str());
        
    } catch (const std::exception& ex) {
        ReportError("Error during file rotation: " + std::string(ex.what()));
    }
}

std::string FileMetricsReporter::GenerateFilename() const {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    std::ostringstream filename;
    filename << m_config.filePrefix << "_"
             << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S")
             << "_" << std::setfill('0') << std::setw(3) << ms.count()
             << m_config.fileExtension;
    
    return m_outputDirectory + "/" + filename.str();
}

bool FileMetricsReporter::CreateNewFile() {
    try {
        m_currentFilePath = GenerateFilename();
        m_currentFile = std::make_unique<std::ofstream>(m_currentFilePath, 
                                                       std::ios::out | std::ios::trunc);
        
        if (!m_currentFile->is_open()) {
            ReportError("Failed to open new metrics file: " + m_currentFilePath);
            return false;
        }
        
        // Write file header
        WriteFileHeader();
        
        // Add to generated files list
        {
            std::lock_guard<std::mutex> lock(m_fileListMutex);
            m_generatedFiles.push_back(m_currentFilePath);
        }
        
        m_currentFileSize = 0;
        return true;
        
    } catch (const std::exception& ex) {
        ReportError("Exception creating new file: " + std::string(ex.what()));
        return false;
    }
}

void FileMetricsReporter::WriteFileHeader() {
    if (!m_currentFile || !m_currentFile->is_open()) {
        return;
    }
    
    try {
        nlohmann::json header;
        header["file_format"] = "rs2v_telemetry_v1";
        header["created_at"] = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        header["generator"] = "FileMetricsReporter";
        header["server_version"] = "1.0.0"; // Could be read from config
        header["format_description"] = "Each line after this header is a JSON metrics snapshot";
        
        *m_currentFile << "# " << header.dump() << "\n";
        m_currentFileSize.fetch_add(header.dump().length() + 3, std::memory_order_relaxed);
        
    } catch (const std::exception& ex) {
        ReportError("Error writing file header: " + std::string(ex.what()));
    }
}

void FileMetricsReporter::WriteFileFooter() {
    if (!m_currentFile || !m_currentFile->is_open()) {
        return;
    }
    
    try {
        nlohmann::json footer;
        footer["file_closed_at"] = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        footer["total_snapshots"] = m_reportsGenerated.load();
        footer["file_size_bytes"] = m_currentFileSize.load();
        
        *m_currentFile << "# " << footer.dump() << "\n";
        
    } catch (const std::exception& ex) {
        Logger::Error("Error writing file footer: %s", ex.what());
    }
}

std::string FileMetricsReporter::FormatSnapshot(const MetricsSnapshot& snapshot) const {
    try {
        nlohmann::json j;
        
        // Timestamp
        j["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
            snapshot.timestamp.time_since_epoch()).count();
        j["timestamp_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            snapshot.timestamp.time_since_epoch()).count();
        
        // System metrics
        if (snapshot.cpuUsagePercent >= 0) {
            j["system"]["cpu_usage_percent"] = snapshot.cpuUsagePercent;
        }
        if (snapshot.memoryUsedBytes > 0) {
            j["system"]["memory_used_bytes"] = snapshot.memoryUsedBytes;
            j["system"]["memory_total_bytes"] = snapshot.memoryTotalBytes;
            j["system"]["memory_usage_percent"] = snapshot.memoryTotalBytes > 0 ? 
                (static_cast<double>(snapshot.memoryUsedBytes) / snapshot.memoryTotalBytes) * 100.0 : 0.0;
        }
        
        j["system"]["network_bytes_sent"] = snapshot.networkBytesSent;
        j["system"]["network_bytes_received"] = snapshot.networkBytesReceived;
        j["system"]["disk_read_bytes"] = snapshot.diskReadBytes;
        j["system"]["disk_write_bytes"] = snapshot.diskWriteBytes;
        
        // Network metrics
        j["network"]["active_connections"] = snapshot.activeConnections;
        j["network"]["authenticated_players"] = snapshot.authenticatedPlayers;
        j["network"]["total_packets_processed"] = snapshot.totalPacketsProcessed;
        j["network"]["total_packets_dropped"] = snapshot.totalPacketsDropped;
        j["network"]["average_latency_ms"] = snapshot.averageLatencyMs;
        j["network"]["packet_loss_rate"] = snapshot.packetLossRate;
        
        // Gameplay metrics
        j["gameplay"]["current_tick"] = snapshot.currentTick;
        j["gameplay"]["active_matches"] = snapshot.activeMatches;
        j["gameplay"]["total_kills"] = snapshot.totalKills;
        j["gameplay"]["total_deaths"] = snapshot.totalDeaths;
        j["gameplay"]["objectives_captured"] = snapshot.objectivesCaptured;
        j["gameplay"]["chat_messages_sent"] = snapshot.chatMessagesSent;
        
        // Performance metrics
        j["performance"]["frame_time_ms"] = snapshot.frameTimeMs;
        j["performance"]["physics_time_ms"] = snapshot.physicsTimeMs;
        j["performance"]["network_time_ms"] = snapshot.networkTimeMs;
        j["performance"]["game_logic_time_ms"] = snapshot.gameLogicTimeMs;
        
        // Security metrics
        j["security"]["security_violations"] = snapshot.securityViolations;
        j["security"]["malformed_packets"] = snapshot.malformedPackets;
        j["security"]["speed_hack_detections"] = snapshot.speedHackDetections;
        j["security"]["kicked_players"] = snapshot.kickedPlayers;
        j["security"]["banned_players"] = snapshot.bannedPlayers;
        
        // Additional computed metrics
        if (snapshot.totalKills + snapshot.totalDeaths > 0) {
            j["gameplay"]["kill_death_ratio"] = snapshot.totalDeaths > 0 ? 
                static_cast<double>(snapshot.totalKills) / snapshot.totalDeaths : 
                static_cast<double>(snapshot.totalKills);
        }
        
        if (snapshot.totalPacketsProcessed > 0) {
            j["network"]["packet_loss_percent"] = 
                (static_cast<double>(snapshot.totalPacketsDropped) / snapshot.totalPacketsProcessed) * 100.0;
        }
        
        return j.dump();
        
    } catch (const std::exception& ex) {
        ReportError("Error formatting snapshot to JSON: " + std::string(ex.what()));
        return "{}"; // Return empty JSON on error
    }
}

void FileMetricsReporter::CompressFile(const std::string& filepath) {
#ifdef ENABLE_COMPRESSION
    try {
        std::string compressedPath = filepath + ".gz";
        
        std::ifstream input(filepath, std::ios::binary);
        if (!input.is_open()) {
            ReportError("Failed to open file for compression: " + filepath);
            return;
        }
        
        gzFile output = gzopen(compressedPath.c_str(), "wb");
        if (output == nullptr) {
            ReportError("Failed to create compressed file: " + compressedPath);
            return;
        }
        
        char buffer[8192];
        while (input.read(buffer, sizeof(buffer)) || input.gcount() > 0) {
            gzwrite(output, buffer, static_cast<unsigned int>(input.gcount()));
        }
        
        input.close();
        gzclose(output);
        
        // Remove original file
        std::filesystem::remove(filepath);
        
        // Update file list
        {
            std::lock_guard<std::mutex> lock(m_fileListMutex);
            auto it = std::find(m_generatedFiles.begin(), m_generatedFiles.end(), filepath);
            if (it != m_generatedFiles.end()) {
                *it = compressedPath;
            }
        }
        
        Logger::Info("Compressed metrics file: %s -> %s", filepath.c_str(), compressedPath.c_str());
        
    } catch (const std::exception& ex) {
        ReportError("Error compressing file: " + std::string(ex.what()));
    }
#else
    Logger::Debug("Compression not enabled for file: %s", filepath.c_str());
#endif
}

void FileMetricsReporter::CleanupOldFiles() {
    try {
        std::lock_guard<std::mutex> lock(m_fileListMutex);
        
        if (m_generatedFiles.size() <= m_config.maxFiles) {
            return; // No cleanup needed
        }
        
        // Sort files by creation time (oldest first)
        std::sort(m_generatedFiles.begin(), m_generatedFiles.end());
        
        size_t filesToRemove = m_generatedFiles.size() - m_config.maxFiles;
        
        for (size_t i = 0; i < filesToRemove; ++i) {
            const std::string& fileToRemove = m_generatedFiles[i];
            
            try {
                if (std::filesystem::exists(fileToRemove)) {
                    std::filesystem::remove(fileToRemove);
                    Logger::Info("Removed old metrics file: %s", fileToRemove.c_str());
                }
            } catch (const std::exception& ex) {
                Logger::Error("Failed to remove old metrics file %s: %s", 
                             fileToRemove.c_str(), ex.what());
            }
        }
        
        // Remove from tracking list
        m_generatedFiles.erase(m_generatedFiles.begin(), m_generatedFiles.begin() + filesToRemove);
        
    } catch (const std::exception& ex) {
        ReportError("Error during old file cleanup: " + std::string(ex.what()));
    }
}

void FileMetricsReporter::ScanExistingFiles() {
    try {
        std::lock_guard<std::mutex> lock(m_fileListMutex);
        m_generatedFiles.clear();
        
        if (!std::filesystem::exists(m_outputDirectory)) {
            return;
        }
        
        // Create regex pattern to match our metric files
        std::string pattern = m_config.filePrefix + R"(_\d{8}_\d{6}_\d{3})" + 
                             std::regex_replace(m_config.fileExtension, std::regex(R"(\.)"), R"(\.)") + 
                             R"((?:\.gz)?)";
        std::regex fileRegex(pattern);
        
        for (const auto& entry : std::filesystem::directory_iterator(m_outputDirectory)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                if (std::regex_match(filename, fileRegex)) {
                    m_generatedFiles.push_back(entry.path().string());
                }
            }
        }
        
        // Sort by filename (which includes timestamp)
        std::sort(m_generatedFiles.begin(), m_generatedFiles.end());
        
        Logger::Info("Found %zu existing metrics files in directory", m_generatedFiles.size());
        
    } catch (const std::exception& ex) {
        Logger::Error("Error scanning existing files: %s", ex.what());
    }
}

void FileMetricsReporter::ReportError(const std::string& error) {
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
    
    Logger::Error("FileMetricsReporter: %s", error.c_str());
    m_healthy = false;
}

// Factory function implementation
namespace ReporterFactory {

std::unique_ptr<MetricsReporter> CreateFileReporter(
    const std::string& prefix,
    size_t maxFileSize,
    size_t maxFiles) {
    
    FileReporterConfig config;
    config.filePrefix = prefix;
    config.maxFileSize = maxFileSize;
    config.maxFiles = maxFiles;
    config.enableRotation = true;
    config.flushImmediately = false;
    
    return std::make_unique<FileMetricsReporter>(config);
}

} // namespace ReporterFactory

} // namespace Telemetry