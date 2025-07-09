#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <unordered_map>
#include "Network/UDPSocket.h"
#include "Security/EACServerEmulator.h"
#include "Security/ClientEACDetector.h"
#include "Security/EACMemoryScanner.h"
#include "Config/SecurityConfig.h"

enum class FlexibleEACResult {
    Clean,
    ChallengeFailed,
    ExecutableTampered,
    MemoryTampered,
    Timeout,
    Error
};

struct FlexibleEACReport {
    uint32_t            clientId;
    FlexibleEACResult   result;
    std::string         details;
    uint64_t            timestampMs;
};

class FlexibleEACServer {
public:
    using ReportCallback = std::function<void(const FlexibleEACReport&)>;

    explicit FlexibleEACServer(std::shared_ptr<SecurityConfig> config);
    ~FlexibleEACServer();

    // Initialize all sub-components
    bool Initialize(uint16_t listenPort);

    // Begin validation sequence for a client
    void ValidateClient(uint32_t clientId,
                        const std::string& ip,
                        uint16_t port,
                        uintptr_t processHandle,
                        const std::string& exePath);

    // Called each server tick to process network and scans
    void Update();

    // Shutdown and clean up
    void Shutdown();

    // Set a callback to receive final reports
    void SetReportCallback(ReportCallback cb);

    // Runtime reconfiguration
    bool ReloadConfig();

private:
    std::shared_ptr<SecurityConfig>    m_config;
    UDPSocket                          m_socket;
    EACServerEmulator                  m_emulator;
    ClientEACDetector                  m_detector;
    EACMemoryScanner                   m_memoryScanner;

    struct Session {
        uint32_t clientId;
        std::string ip;
        uint16_t port;
        uintptr_t processHandle;
        std::string exePath;
        bool handshakeDone = false;
        std::chrono::steady_clock::time_point startTime;
    };
    std::unordered_map<uint32_t, Session>      m_sessions;
    std::vector<FlexibleEACReport>             m_reports;
    ReportCallback                             m_reportCb;

    // Internal handlers
    void OnChallenge(uint32_t clientId, const std::string& details);
    void OnDetector(uint32_t clientId, EACDetectionResult res, const std::string& details);
    void OnMemoryScan(uint32_t clientId, EACScanResult res, const std::string& details);

    // Helpers
    void ProcessEmulator();
    void DispatchReport(const FlexibleEACReport& rpt);
    uint64_t NowMs() const;
};