// src/Security/EnhancedEACAntiCheat.cpp
#include "Security/EnhancedEACAntiCheat.h"
#include "Utils/Logger.h"

EnhancedEACAntiCheat::EnhancedEACAntiCheat(std::shared_ptr<SecurityConfig> config)
    : m_config(std::move(config))
    , m_detector()
    , m_memoryScanner()
    , m_emulator()
{}

EnhancedEACAntiCheat::~EnhancedEACAntiCheat() {
    Shutdown();
}

bool EnhancedEACAntiCheat::Initialize() {
    if (!m_detector.Initialize()) {
        Logger::Error("EnhancedEACAntiCheat: Detector init failed");
        return false;
    }
    m_detector.SetDetectionCallback([this](uint32_t id, EACDetectionResult r, const std::string& d){
        OnDetectorResult(id, r, d);
    });

    // Load memory signatures from config
    auto sigs = m_config->GetEACSignatures();
    if (!m_memoryScanner.Initialize(sigs, std::chrono::milliseconds(m_config->GetInt("EAC.ScanTimeoutMs", 5000)))) {
        Logger::Error("EnhancedEACAntiCheat: MemoryScanner init failed");
        return false;
    }
    m_memoryScanner.SetScanCallback([this](uint32_t id, EACScanResult r, const std::string& d){
        OnMemoryScanResult(id, r, d);
    });

    if (!m_emulator.Initialize(uint16_t(m_config->GetInt("EAC.EmulatorPort", 7957)))) {
        Logger::Error("EnhancedEACAntiCheat: Emulator init failed");
        return false;
    }
    m_emulator.SetDetectionCallback([this](uint32_t id, EACDetectionResult r, const std::string& d){
        OnEmulatorResult(id, r, d);
    });

    return true;
}

void EnhancedEACAntiCheat::Shutdown() {
    m_emulator.Shutdown();
    m_memoryScanner.Shutdown();
    m_detector.Shutdown();
}

void EnhancedEACAntiCheat::ValidateClient(std::shared_ptr<ClientConnection> conn,
                                          uintptr_t processHandle,
                                          const std::string& exePath)
{
    uint32_t id = conn->GetClientId();
    // Step 1: challenge handshake
    m_emulator.HandlePacket(conn->GetIP(), conn->GetPort(), {/* simulate EAC handshake */});
    // Step 2: detector (exe check)
    m_detector.DetectClient(id, exePath);
    // Step 3: memory scan
    m_memoryScanner.ScanClient(id, processHandle);
}

void EnhancedEACAntiCheat::Update() {
    m_detector.Poll();
    m_memoryScanner.Poll();
    m_emulator.ProcessRequests();

    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& rpt : m_pendingReports) {
        if (m_callback) m_callback(rpt);
    }
    m_pendingReports.clear();
}

void EnhancedEACAntiCheat::SetReportCallback(ReportCallback cb) {
    m_callback = std::move(cb);
}

void EnhancedEACAntiCheat::OnDetectorResult(uint32_t clientId, EACDetectionResult res, const std::string& details) {
    EnhancedEACResult er = (res == EACDetectionResult::Clean ? EnhancedEACResult::Clean : EnhancedEACResult::TamperingDetected);
    EnqueueReport({clientId, er, "Executable: " + details, std::chrono::steady_clock::now()});
}

void EnhancedEACAntiCheat::OnMemoryScanResult(uint32_t clientId, EACScanResult res, const std::string& details) {
    EnhancedEACResult er = (res == EACScanResult::Clean ? EnhancedEACResult::Clean : EnhancedEACResult::MemorySignatureDetected);
    EnqueueReport({clientId, er, "Memory: " + details, std::chrono::steady_clock::now()});
}

void EnhancedEACAntiCheat::OnEmulatorResult(uint32_t clientId, EACDetectionResult res, const std::string& details) {
    EnhancedEACResult er = (res == EACDetectionResult::Clean ? EnhancedEACResult::Clean : EnhancedEACResult::HandshakeFailed);
    EnqueueReport({clientId, er, "Handshake: " + details, std::chrono::steady_clock::now()});
}

void EnhancedEACAntiCheat::EnqueueReport(const EnhancedEACReport& report) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pendingReports.push_back(report);
}