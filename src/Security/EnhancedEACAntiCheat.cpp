// src/Security/EnhancedEACAntiCheat.cpp
#include "Security/EnhancedEACAntiCheat.h"
#include "Utils/Logger.h"

EnhancedEACAntiCheat::EnhancedEACAntiCheat(std::shared_ptr<SecurityConfig> config)
    : m_config(std::move(config))
    , m_detector()
    , m_memoryScanner()
    , m_emulator()
{
    Logger::Trace("[EnhancedEACAntiCheat::EnhancedEACAntiCheat] Constructor called, config is %s",
                  m_config ? "non-null" : "null");
}

EnhancedEACAntiCheat::~EnhancedEACAntiCheat() {
    Logger::Trace("[EnhancedEACAntiCheat::~EnhancedEACAntiCheat] Destructor called, invoking Shutdown");
    Shutdown();
    Logger::Trace("[EnhancedEACAntiCheat::~EnhancedEACAntiCheat] Destructor completed");
}

bool EnhancedEACAntiCheat::Initialize() {
    Logger::Trace("[EnhancedEACAntiCheat::Initialize] Entry");
    Logger::Info("[EnhancedEACAntiCheat::Initialize] Beginning EnhancedEACAntiCheat initialization");

    Logger::Debug("[EnhancedEACAntiCheat::Initialize] Initializing ClientEACDetector...");
    if (!m_detector.Initialize()) {
        Logger::Error("EnhancedEACAntiCheat: Detector init failed");
        Logger::Error("[EnhancedEACAntiCheat::Initialize] ClientEACDetector initialization failed - aborting");
        Logger::Trace("[EnhancedEACAntiCheat::Initialize] Exit, returning false (detector init failed)");
        return false;
    }
    Logger::Debug("[EnhancedEACAntiCheat::Initialize] ClientEACDetector initialized successfully");

    m_detector.SetDetectionCallback([this](uint32_t id, EACDetectionResult r, const std::string& d){
        Logger::Debug("[EnhancedEACAntiCheat::DetectionCallback] Detector callback fired: clientId=%u, result=%d, details='%s'",
                      id, int(r), d.c_str());
        OnDetectorResult(id, r, d);
    });
    Logger::Debug("[EnhancedEACAntiCheat::Initialize] Detector detection callback registered");

    // Load memory signatures (empty default; would be loaded from config files)
    std::vector<ScanSignature> sigs;
    Logger::Debug("[EnhancedEACAntiCheat::Initialize] Initializing EACMemoryScanner with %zu signatures, timeout=5000ms",
                  sigs.size());
    if (!m_memoryScanner.Initialize(sigs, std::chrono::milliseconds(5000))) {
        Logger::Error("EnhancedEACAntiCheat: MemoryScanner init failed");
        Logger::Error("[EnhancedEACAntiCheat::Initialize] EACMemoryScanner initialization failed - aborting");
        Logger::Trace("[EnhancedEACAntiCheat::Initialize] Exit, returning false (memory scanner init failed)");
        return false;
    }
    Logger::Debug("[EnhancedEACAntiCheat::Initialize] EACMemoryScanner initialized successfully");

    m_memoryScanner.SetScanCallback([this](uint32_t id, EACScanResult r, const std::string& d){
        Logger::Debug("[EnhancedEACAntiCheat::ScanCallback] Memory scanner callback fired: clientId=%u, result=%d, details='%s'",
                      id, int(r), d.c_str());
        OnMemoryScanResult(id, r, d);
    });
    Logger::Debug("[EnhancedEACAntiCheat::Initialize] Memory scanner scan callback registered");

    Logger::Debug("[EnhancedEACAntiCheat::Initialize] Initializing EACServerEmulator on port 7957...");
    if (!m_emulator.Initialize(7957)) {
        Logger::Error("EnhancedEACAntiCheat: Emulator init failed");
        Logger::Error("[EnhancedEACAntiCheat::Initialize] EACServerEmulator initialization failed on port 7957 - aborting");
        Logger::Trace("[EnhancedEACAntiCheat::Initialize] Exit, returning false (emulator init failed)");
        return false;
    }
    Logger::Debug("[EnhancedEACAntiCheat::Initialize] EACServerEmulator initialized successfully on port 7957");
    // Note: EACServerEmulator does not expose a detection callback;
    // emulator results are handled via ProcessRequests/polling.

    Logger::Info("[EnhancedEACAntiCheat::Initialize] All anti-cheat subsystems initialized successfully");
    Logger::Trace("[EnhancedEACAntiCheat::Initialize] Exit, returning true");
    return true;
}

void EnhancedEACAntiCheat::Shutdown() {
    Logger::Trace("[EnhancedEACAntiCheat::Shutdown] Entry");
    Logger::Info("[EnhancedEACAntiCheat::Shutdown] Beginning EnhancedEACAntiCheat shutdown");

    Logger::Debug("[EnhancedEACAntiCheat::Shutdown] Shutting down EACServerEmulator...");
    m_emulator.Shutdown();
    Logger::Debug("[EnhancedEACAntiCheat::Shutdown] EACServerEmulator shutdown complete");

    Logger::Debug("[EnhancedEACAntiCheat::Shutdown] Shutting down EACMemoryScanner...");
    m_memoryScanner.Shutdown();
    Logger::Debug("[EnhancedEACAntiCheat::Shutdown] EACMemoryScanner shutdown complete");

    Logger::Debug("[EnhancedEACAntiCheat::Shutdown] Shutting down ClientEACDetector...");
    m_detector.Shutdown();
    Logger::Debug("[EnhancedEACAntiCheat::Shutdown] ClientEACDetector shutdown complete");

    Logger::Info("[EnhancedEACAntiCheat::Shutdown] All anti-cheat subsystems shut down");
    Logger::Trace("[EnhancedEACAntiCheat::Shutdown] Exit");
}

void EnhancedEACAntiCheat::ValidateClient(std::shared_ptr<ClientConnection> conn,
                                          uintptr_t processHandle,
                                          const std::string& exePath)
{
    Logger::Trace("[EnhancedEACAntiCheat::ValidateClient] Entry, conn=%p, processHandle=0x%llx, exePath='%s'",
                  static_cast<void*>(conn.get()),
                  static_cast<unsigned long long>(processHandle),
                  exePath.c_str());
    uint32_t id = conn->GetClientId();
    Logger::Debug("[EnhancedEACAntiCheat::ValidateClient] Client ID resolved to %u", id);
    Logger::Info("[EnhancedEACAntiCheat::ValidateClient] Starting anti-cheat validation for client %u (exePath='%s', processHandle=0x%llx)",
                 id, exePath.c_str(), static_cast<unsigned long long>(processHandle));

    // Step 1: challenge handshake (handled via ProcessRequests)
    Logger::Debug("[EnhancedEACAntiCheat::ValidateClient] Step 1: Processing EAC emulator requests (challenge handshake) for client %u", id);
    m_emulator.ProcessRequests();
    Logger::Debug("[EnhancedEACAntiCheat::ValidateClient] Step 1 complete: Emulator requests processed for client %u", id);

    // Step 2: detector (exe check)
    Logger::Debug("[EnhancedEACAntiCheat::ValidateClient] Step 2: Starting executable detection for client %u, exePath='%s'", id, exePath.c_str());
    m_detector.DetectClient(id, exePath);
    Logger::Debug("[EnhancedEACAntiCheat::ValidateClient] Step 2 complete: Executable detection initiated for client %u", id);

    // Step 3: memory scan
    Logger::Debug("[EnhancedEACAntiCheat::ValidateClient] Step 3: Starting memory scan for client %u, processHandle=0x%llx",
                  id, static_cast<unsigned long long>(processHandle));
    m_memoryScanner.ScanClient(id, processHandle);
    Logger::Debug("[EnhancedEACAntiCheat::ValidateClient] Step 3 complete: Memory scan initiated for client %u", id);

    Logger::Info("[EnhancedEACAntiCheat::ValidateClient] All anti-cheat validation steps initiated for client %u", id);
    Logger::Trace("[EnhancedEACAntiCheat::ValidateClient] Exit");
}

void EnhancedEACAntiCheat::Update() {
    Logger::Trace("[EnhancedEACAntiCheat::Update] Entry");

    Logger::Trace("[EnhancedEACAntiCheat::Update] Polling ClientEACDetector");
    m_detector.Poll();
    Logger::Trace("[EnhancedEACAntiCheat::Update] Polling EACMemoryScanner");
    m_memoryScanner.Poll();
    Logger::Trace("[EnhancedEACAntiCheat::Update] Processing EACServerEmulator requests");
    m_emulator.ProcessRequests();

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_pendingReports.empty()) {
        Logger::Debug("[EnhancedEACAntiCheat::Update] Dispatching %zu pending reports", m_pendingReports.size());
    }
    for (auto& rpt : m_pendingReports) {
        Logger::Debug("[EnhancedEACAntiCheat::Update] Dispatching report: clientId=%u, result=%d, details='%s'",
                      rpt.clientId, int(rpt.result), rpt.details.c_str());
        if (m_callback) {
            m_callback(rpt);
            Logger::Trace("[EnhancedEACAntiCheat::Update] Report dispatched to callback for client %u", rpt.clientId);
        } else {
            Logger::Debug("[EnhancedEACAntiCheat::Update] No report callback set, skipping dispatch for client %u", rpt.clientId);
        }
    }
    m_pendingReports.clear();

    Logger::Trace("[EnhancedEACAntiCheat::Update] Exit");
}

void EnhancedEACAntiCheat::SetReportCallback(ReportCallback cb) {
    Logger::Trace("[EnhancedEACAntiCheat::SetReportCallback] Entry, callback is %s", cb ? "non-null" : "null");
    m_callback = std::move(cb);
    Logger::Debug("[EnhancedEACAntiCheat::SetReportCallback] Report callback has been set");
    Logger::Trace("[EnhancedEACAntiCheat::SetReportCallback] Exit");
}

void EnhancedEACAntiCheat::OnDetectorResult(uint32_t clientId, EACDetectionResult res, const std::string& details) {
    Logger::Trace("[EnhancedEACAntiCheat::OnDetectorResult] Entry, clientId=%u, result=%d, details='%s'",
                  clientId, int(res), details.c_str());
    EnhancedEACResult er = (res == EACDetectionResult::Clean ? EnhancedEACResult::Clean : EnhancedEACResult::TamperingDetected);
    Logger::Debug("[EnhancedEACAntiCheat::OnDetectorResult] Detector result for client %u: EACDetectionResult=%d -> EnhancedEACResult=%d",
                  clientId, int(res), int(er));
    if (res == EACDetectionResult::Clean) {
        Logger::Info("[EnhancedEACAntiCheat::OnDetectorResult] Executable check CLEAN for client %u: %s", clientId, details.c_str());
    } else {
        Logger::Warn("[EnhancedEACAntiCheat::OnDetectorResult] Executable check FLAGGED for client %u: %s (tampering detected)",
                     clientId, details.c_str());
    }
    EnqueueReport({clientId, er, "Executable: " + details, std::chrono::steady_clock::now()});
    Logger::Trace("[EnhancedEACAntiCheat::OnDetectorResult] Exit");
}

void EnhancedEACAntiCheat::OnMemoryScanResult(uint32_t clientId, EACScanResult res, const std::string& details) {
    Logger::Trace("[EnhancedEACAntiCheat::OnMemoryScanResult] Entry, clientId=%u, result=%d, details='%s'",
                  clientId, int(res), details.c_str());
    EnhancedEACResult er = (res == EACScanResult::Clean ? EnhancedEACResult::Clean : EnhancedEACResult::MemorySignatureDetected);
    Logger::Debug("[EnhancedEACAntiCheat::OnMemoryScanResult] Memory scan result for client %u: EACScanResult=%d -> EnhancedEACResult=%d",
                  clientId, int(res), int(er));
    if (res == EACScanResult::Clean) {
        Logger::Info("[EnhancedEACAntiCheat::OnMemoryScanResult] Memory scan CLEAN for client %u: %s", clientId, details.c_str());
    } else {
        Logger::Warn("[EnhancedEACAntiCheat::OnMemoryScanResult] Memory scan FLAGGED for client %u: %s (signature detected)",
                     clientId, details.c_str());
    }
    EnqueueReport({clientId, er, "Memory: " + details, std::chrono::steady_clock::now()});
    Logger::Trace("[EnhancedEACAntiCheat::OnMemoryScanResult] Exit");
}

void EnhancedEACAntiCheat::OnEmulatorResult(uint32_t clientId, EACDetectionResult res, const std::string& details) {
    Logger::Trace("[EnhancedEACAntiCheat::OnEmulatorResult] Entry, clientId=%u, result=%d, details='%s'",
                  clientId, int(res), details.c_str());
    EnhancedEACResult er = (res == EACDetectionResult::Clean ? EnhancedEACResult::Clean : EnhancedEACResult::HandshakeFailed);
    Logger::Debug("[EnhancedEACAntiCheat::OnEmulatorResult] Emulator result for client %u: EACDetectionResult=%d -> EnhancedEACResult=%d",
                  clientId, int(res), int(er));
    if (res == EACDetectionResult::Clean) {
        Logger::Info("[EnhancedEACAntiCheat::OnEmulatorResult] Handshake check CLEAN for client %u: %s", clientId, details.c_str());
    } else {
        Logger::Warn("[EnhancedEACAntiCheat::OnEmulatorResult] Handshake check FAILED for client %u: %s",
                     clientId, details.c_str());
    }
    EnqueueReport({clientId, er, "Handshake: " + details, std::chrono::steady_clock::now()});
    Logger::Trace("[EnhancedEACAntiCheat::OnEmulatorResult] Exit");
}

void EnhancedEACAntiCheat::EnqueueReport(const EnhancedEACReport& report) {
    Logger::Trace("[EnhancedEACAntiCheat::EnqueueReport] Entry, clientId=%u, result=%d, details='%s'",
                  report.clientId, int(report.result), report.details.c_str());
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pendingReports.push_back(report);
    Logger::Debug("[EnhancedEACAntiCheat::EnqueueReport] Report enqueued for client %u, pending reports=%zu",
                  report.clientId, m_pendingReports.size());
    Logger::Trace("[EnhancedEACAntiCheat::EnqueueReport] Exit");
}
