#include "Security/FlexibleEACServer.h"
#include "Utils/Logger.h"
#include <chrono>

FlexibleEACServer::FlexibleEACServer(std::shared_ptr<SecurityConfig> config)
    : m_config(std::move(config))
    , m_emulator()
    , m_detector()
    , m_memoryScanner()
{
    Logger::Trace("[FlexibleEACServer::FlexibleEACServer] Constructor called, config is %s",
                  m_config ? "non-null" : "null");
}

FlexibleEACServer::~FlexibleEACServer() {
    Logger::Trace("[FlexibleEACServer::~FlexibleEACServer] Destructor called, invoking Shutdown");
    Shutdown();
    Logger::Trace("[FlexibleEACServer::~FlexibleEACServer] Destructor completed");
}

bool FlexibleEACServer::Initialize(uint16_t listenPort) {
    Logger::Trace("[FlexibleEACServer::Initialize] Entry, listenPort=%u", listenPort);
    Logger::Info("[FlexibleEACServer::Initialize] Beginning FlexibleEACServer initialization on port %u", listenPort);

    if (!m_config) {
        Logger::Error("[FlexibleEACServer::Initialize] Config is null, cannot initialize");
        Logger::Trace("[FlexibleEACServer::Initialize] Exit, returning false (null config)");
        return false;
    }
    Logger::Debug("[FlexibleEACServer::Initialize] Config is valid, proceeding with initialization");

    // Bind UDP for emulator
    Logger::Debug("[FlexibleEACServer::Initialize] Binding UDP socket on port %u", listenPort);
    if (!m_socket.Bind(listenPort)) {
        Logger::Error("FlexibleEACServer: UDP bind failed on port %u", listenPort);
        Logger::Error("[FlexibleEACServer::Initialize] Failed to bind UDP socket on port %u - aborting initialization", listenPort);
        Logger::Trace("[FlexibleEACServer::Initialize] Exit, returning false (bind failed)");
        return false;
    }
    Logger::Debug("[FlexibleEACServer::Initialize] UDP socket bound successfully on port %u", listenPort);

    // Initialize emulator
    Logger::Debug("[FlexibleEACServer::Initialize] Initializing EACServerEmulator on port %u...", listenPort);
    if (!m_emulator.Initialize(listenPort)) {
        Logger::Error("[FlexibleEACServer::Initialize] EACServerEmulator initialization failed on port %u", listenPort);
        Logger::Trace("[FlexibleEACServer::Initialize] Exit, returning false (emulator init failed)");
        return false;
    }
    Logger::Debug("[FlexibleEACServer::Initialize] EACServerEmulator initialized successfully");
    // Note: EACServerEmulator detection results are handled via ProcessRequests/polling

    // Initialize detector
    Logger::Debug("[FlexibleEACServer::Initialize] Initializing ClientEACDetector...");
    if (!m_detector.Initialize()) {
        Logger::Error("[FlexibleEACServer::Initialize] ClientEACDetector initialization failed");
        Logger::Trace("[FlexibleEACServer::Initialize] Exit, returning false (detector init failed)");
        return false;
    }
    Logger::Debug("[FlexibleEACServer::Initialize] ClientEACDetector initialized successfully");

    m_detector.SetDetectionCallback([this](uint32_t id, EACDetectionResult r, const std::string& d){
        Logger::Debug("[FlexibleEACServer::DetectorCallback] Detector callback fired: clientId=%u, result=%d, details='%s'",
                      id, int(r), d.c_str());
        OnDetector(id, r, d);
    });
    Logger::Debug("[FlexibleEACServer::Initialize] Detector detection callback registered");

    // Load memory signatures (empty default set)
    std::vector<ScanSignature> sigs;
    Logger::Debug("[FlexibleEACServer::Initialize] Initializing EACMemoryScanner with %zu signatures, timeout=5000ms",
                  sigs.size());
    if (!m_memoryScanner.Initialize(sigs,
                        std::chrono::milliseconds(5000)))
    {
        Logger::Error("[FlexibleEACServer::Initialize] EACMemoryScanner initialization failed");
        Logger::Trace("[FlexibleEACServer::Initialize] Exit, returning false (memory scanner init failed)");
        return false;
    }
    Logger::Debug("[FlexibleEACServer::Initialize] EACMemoryScanner initialized successfully");

    m_memoryScanner.SetScanCallback([this](uint32_t id, EACScanResult r, const std::string& d){
        Logger::Debug("[FlexibleEACServer::ScanCallback] Memory scanner callback fired: clientId=%u, result=%d, details='%s'",
                      id, int(r), d.c_str());
        OnMemoryScan(id, r, d);
    });
    Logger::Debug("[FlexibleEACServer::Initialize] Memory scanner scan callback registered");

    Logger::Info("FlexibleEACServer: Initialized on port %u", listenPort);
    Logger::Info("[FlexibleEACServer::Initialize] All subsystems initialized successfully on port %u", listenPort);
    Logger::Trace("[FlexibleEACServer::Initialize] Exit, returning true");
    return true;
}

void FlexibleEACServer::ValidateClient(uint32_t clientId,
                                       const std::string& ip,
                                       uint16_t port,
                                       uintptr_t processHandle,
                                       const std::string& exePath)
{
    Logger::Trace("[FlexibleEACServer::ValidateClient] Entry, clientId=%u, ip='%s', port=%u, processHandle=0x%llx, exePath='%s'",
                  clientId, ip.c_str(), port,
                  static_cast<unsigned long long>(processHandle), exePath.c_str());
    Logger::Info("[FlexibleEACServer::ValidateClient] Starting anti-cheat validation for client %u (%s:%u, exe='%s')",
                 clientId, ip.c_str(), port, exePath.c_str());

    Session sess;
    sess.clientId       = clientId;
    sess.ip             = ip;
    sess.port           = port;
    sess.processHandle  = processHandle;
    sess.exePath        = exePath;
    sess.startTime      = std::chrono::steady_clock::now();
    m_sessions[clientId] = sess;
    Logger::Debug("[FlexibleEACServer::ValidateClient] Session created for client %u: ip='%s', port=%u, total sessions=%zu",
                  clientId, ip.c_str(), port, m_sessions.size());

    // Kick off handshake via emulator packet handling
    Logger::Debug("[FlexibleEACServer::ValidateClient] Step 1: Initiating EAC handshake for client %u via emulator", clientId);
    m_emulator.HandlePacket(ip, port, {/* simulate EAC handshake */});
    Logger::Debug("[FlexibleEACServer::ValidateClient] Step 1 complete: EAC handshake initiated for client %u", clientId);

    // Kick off exe check
    Logger::Debug("[FlexibleEACServer::ValidateClient] Step 2: Starting executable detection for client %u, exePath='%s'",
                  clientId, exePath.c_str());
    m_detector.DetectClient(clientId, exePath);
    Logger::Debug("[FlexibleEACServer::ValidateClient] Step 2 complete: Executable detection initiated for client %u", clientId);

    // Kick off memory scan
    Logger::Debug("[FlexibleEACServer::ValidateClient] Step 3: Starting memory scan for client %u, processHandle=0x%llx",
                  clientId, static_cast<unsigned long long>(processHandle));
    m_memoryScanner.ScanClient(clientId, processHandle);
    Logger::Debug("[FlexibleEACServer::ValidateClient] Step 3 complete: Memory scan initiated for client %u", clientId);

    Logger::Info("[FlexibleEACServer::ValidateClient] All validation steps initiated for client %u", clientId);
    Logger::Trace("[FlexibleEACServer::ValidateClient] Exit");
}

void FlexibleEACServer::Update() {
    Logger::Trace("[FlexibleEACServer::Update] Entry");

    // Process network/emulator
    Logger::Trace("[FlexibleEACServer::Update] Processing emulator network events");
    ProcessEmulator();

    // Poll detectors and scanners
    Logger::Trace("[FlexibleEACServer::Update] Polling ClientEACDetector");
    m_detector.Poll();
    Logger::Trace("[FlexibleEACServer::Update] Polling EACMemoryScanner");
    m_memoryScanner.Poll();

    // Dispatch all accumulated reports
    if (!m_reports.empty()) {
        Logger::Debug("[FlexibleEACServer::Update] Dispatching %zu accumulated reports", m_reports.size());
    }
    for (auto& rpt : m_reports) {
        Logger::Debug("[FlexibleEACServer::Update] Dispatching report: clientId=%u, result=%d, details='%s'",
                      rpt.clientId, int(rpt.result), rpt.details.c_str());
        DispatchReport(rpt);
    }
    m_reports.clear();

    Logger::Trace("[FlexibleEACServer::Update] Exit");
}

void FlexibleEACServer::ProcessEmulator() {
    Logger::Trace("[FlexibleEACServer::ProcessEmulator] Entry");
    std::string ip; uint16_t port;
    std::vector<uint8_t> buf(1500);
    int len;
    size_t packetsProcessed = 0;
    while ((len = m_socket.ReceiveFrom(ip, port, buf.data(), (int)buf.size())) > 0) {
        buf.resize(len);
        Logger::Trace("[FlexibleEACServer::ProcessEmulator] Received %d bytes from %s:%u", len, ip.c_str(), port);
        m_emulator.HandlePacket(ip, port, buf);
        ++packetsProcessed;
    }
    if (packetsProcessed > 0) {
        Logger::Debug("[FlexibleEACServer::ProcessEmulator] Processed %zu network packets", packetsProcessed);
    }
    Logger::Trace("[FlexibleEACServer::ProcessEmulator] Exit, packets processed=%zu", packetsProcessed);
}

void FlexibleEACServer::Shutdown() {
    Logger::Trace("[FlexibleEACServer::Shutdown] Entry");
    Logger::Info("[FlexibleEACServer::Shutdown] Beginning FlexibleEACServer shutdown");

    Logger::Debug("[FlexibleEACServer::Shutdown] Shutting down EACServerEmulator...");
    m_emulator.Shutdown();
    Logger::Debug("[FlexibleEACServer::Shutdown] EACServerEmulator shutdown complete");

    Logger::Debug("[FlexibleEACServer::Shutdown] Shutting down EACMemoryScanner...");
    m_memoryScanner.Shutdown();
    Logger::Debug("[FlexibleEACServer::Shutdown] EACMemoryScanner shutdown complete");

    Logger::Debug("[FlexibleEACServer::Shutdown] Shutting down ClientEACDetector...");
    m_detector.Shutdown();
    Logger::Debug("[FlexibleEACServer::Shutdown] ClientEACDetector shutdown complete");

    Logger::Debug("[FlexibleEACServer::Shutdown] Closing UDP socket...");
    m_socket.Close();
    Logger::Debug("[FlexibleEACServer::Shutdown] UDP socket closed");

    Logger::Info("FlexibleEACServer: Shutdown complete");
    Logger::Info("[FlexibleEACServer::Shutdown] All subsystems shut down successfully");
    Logger::Trace("[FlexibleEACServer::Shutdown] Exit");
}

void FlexibleEACServer::SetReportCallback(ReportCallback cb) {
    Logger::Trace("[FlexibleEACServer::SetReportCallback] Entry, callback is %s", cb ? "non-null" : "null");
    m_reportCb = std::move(cb);
    Logger::Debug("[FlexibleEACServer::SetReportCallback] Report callback has been set");
    Logger::Trace("[FlexibleEACServer::SetReportCallback] Exit");
}

bool FlexibleEACServer::ReloadConfig() {
    Logger::Trace("[FlexibleEACServer::ReloadConfig] Entry");
    Logger::Info("[FlexibleEACServer::ReloadConfig] Reloading configuration");
    // Re-read signatures, timeouts, emulator settings, etc.
    // Implementation depends on SecurityConfig support
    Logger::Debug("[FlexibleEACServer::ReloadConfig] Configuration reload requested (currently a no-op placeholder)");
    Logger::Warn("[FlexibleEACServer::ReloadConfig] Configuration reload is not fully implemented yet");
    Logger::Trace("[FlexibleEACServer::ReloadConfig] Exit, returning true");
    return true;
}

void FlexibleEACServer::OnChallenge(uint32_t clientId, const std::string& details) {
    Logger::Trace("[FlexibleEACServer::OnChallenge] Entry, clientId=%u, details='%s'", clientId, details.c_str());
    Logger::Info("[FlexibleEACServer::OnChallenge] Handshake challenge result for client %u: %s", clientId, details.c_str());
    FlexibleEACReport rpt{clientId, FlexibleEACResult::Clean, "Handshake: " + details, NowMs()};
    Logger::Debug("[FlexibleEACServer::OnChallenge] Created report for client %u: result=Clean, details='Handshake: %s'",
                  clientId, details.c_str());
    m_reports.push_back(rpt);
    Logger::Debug("[FlexibleEACServer::OnChallenge] Report enqueued, pending reports=%zu", m_reports.size());
    Logger::Trace("[FlexibleEACServer::OnChallenge] Exit");
}

void FlexibleEACServer::OnDetector(uint32_t clientId, EACDetectionResult res, const std::string& details) {
    Logger::Trace("[FlexibleEACServer::OnDetector] Entry, clientId=%u, result=%d, details='%s'",
                  clientId, int(res), details.c_str());
    FlexibleEACResult er = (res == EACDetectionResult::Clean
                           ? FlexibleEACResult::Clean
                           : FlexibleEACResult::ExecutableTampered);
    Logger::Debug("[FlexibleEACServer::OnDetector] Detector result for client %u: EACDetectionResult=%d -> FlexibleEACResult=%d",
                  clientId, int(res), int(er));
    if (res == EACDetectionResult::Clean) {
        Logger::Info("[FlexibleEACServer::OnDetector] Executable check CLEAN for client %u: %s", clientId, details.c_str());
    } else {
        Logger::Warn("[FlexibleEACServer::OnDetector] Executable check FLAGGED for client %u: %s (tampering detected)",
                     clientId, details.c_str());
    }
    FlexibleEACReport rpt{clientId, er, "Exe: " + details, NowMs()};
    m_reports.push_back(rpt);
    Logger::Debug("[FlexibleEACServer::OnDetector] Report enqueued for client %u, pending reports=%zu",
                  clientId, m_reports.size());
    Logger::Trace("[FlexibleEACServer::OnDetector] Exit");
}

void FlexibleEACServer::OnMemoryScan(uint32_t clientId, EACScanResult res, const std::string& details) {
    Logger::Trace("[FlexibleEACServer::OnMemoryScan] Entry, clientId=%u, result=%d, details='%s'",
                  clientId, int(res), details.c_str());
    FlexibleEACResult er = (res == EACScanResult::Clean
                           ? FlexibleEACResult::Clean
                           : FlexibleEACResult::MemoryTampered);
    Logger::Debug("[FlexibleEACServer::OnMemoryScan] Memory scan result for client %u: EACScanResult=%d -> FlexibleEACResult=%d",
                  clientId, int(res), int(er));
    if (res == EACScanResult::Clean) {
        Logger::Info("[FlexibleEACServer::OnMemoryScan] Memory scan CLEAN for client %u: %s", clientId, details.c_str());
    } else {
        Logger::Warn("[FlexibleEACServer::OnMemoryScan] Memory scan FLAGGED for client %u: %s (memory tampering detected)",
                     clientId, details.c_str());
    }
    FlexibleEACReport rpt{clientId, er, "Mem: " + details, NowMs()};
    m_reports.push_back(rpt);
    Logger::Debug("[FlexibleEACServer::OnMemoryScan] Report enqueued for client %u, pending reports=%zu",
                  clientId, m_reports.size());
    Logger::Trace("[FlexibleEACServer::OnMemoryScan] Exit");
}

void FlexibleEACServer::DispatchReport(const FlexibleEACReport& rpt) {
    Logger::Trace("[FlexibleEACServer::DispatchReport] Entry, clientId=%u, result=%d, details='%s'",
                  rpt.clientId, int(rpt.result), rpt.details.c_str());
    if (m_reportCb) {
        Logger::Debug("[FlexibleEACServer::DispatchReport] Dispatching report to callback for client %u", rpt.clientId);
        m_reportCb(rpt);
        Logger::Trace("[FlexibleEACServer::DispatchReport] Callback returned for client %u", rpt.clientId);
    } else {
        Logger::Debug("[FlexibleEACServer::DispatchReport] No report callback set, skipping dispatch for client %u", rpt.clientId);
    }
    Logger::Trace("[FlexibleEACServer::DispatchReport] Exit");
}

uint64_t FlexibleEACServer::NowMs() const {
    Logger::Trace("[FlexibleEACServer::NowMs] Entry");
    auto tp = std::chrono::steady_clock::now().time_since_epoch();
    uint64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp).count();
    Logger::Trace("[FlexibleEACServer::NowMs] Exit, returning %llu ms", static_cast<unsigned long long>(ms));
    return ms;
}
