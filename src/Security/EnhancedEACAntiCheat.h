// src/Security/EnhancedEACAntiCheat.h
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <chrono>
#include <mutex>
#include "Security/ClientEACDetector.h"
#include "Security/EACMemoryScanner.h"
#include "Security/EACServerEmulator.h"
#include "Network/ClientConnection.h"
#include "Config/SecurityConfig.h"

enum class EnhancedEACResult {
    Clean,
    TamperingDetected,
    MemorySignatureDetected,
    HandshakeFailed,
    Timeout,
    Error
};

struct EnhancedEACReport {
    uint32_t                clientId;
    EnhancedEACResult       result;
    std::string             details;
    std::chrono::steady_clock::time_point timestamp;
};

class EnhancedEACAntiCheat {
public:
    using ReportCallback = std::function<void(const EnhancedEACReport&)>;

    EnhancedEACAntiCheat(std::shared_ptr<SecurityConfig> config);
    ~EnhancedEACAntiCheat();

    // Initialize all sub‐systems
    bool Initialize();

    // Shutdown and cleanup
    void Shutdown();

    // Begin full EAC validation sequence for client
    void ValidateClient(std::shared_ptr<ClientConnection> conn, uintptr_t processHandle, const std::string& exePath);

    // Poll pending operations (must be called each server tick)
    void Update();

    // Set callback to receive final reports
    void SetReportCallback(ReportCallback cb);

private:
    std::shared_ptr<SecurityConfig>    m_config;
    ClientEACDetector                  m_detector;
    EACMemoryScanner                   m_memoryScanner;
    EACServerEmulator                  m_emulator;

    std::mutex                         m_mutex;
    std::vector<EnhancedEACReport>     m_pendingReports;
    ReportCallback                     m_callback;

    // Internal steps
    void OnDetectorResult(uint32_t clientId, EACDetectionResult res, const std::string& details);
    void OnMemoryScanResult(uint32_t clientId, EACScanResult res, const std::string& details);
    void OnEmulatorResult(uint32_t clientId, EACDetectionResult res, const std::string& details);

    // Helper to enqueue and dispatch a report
    void EnqueueReport(const EnhancedEACReport& report);
};