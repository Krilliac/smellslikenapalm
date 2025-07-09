#include "Security/FlexibleEACServer.h"
#include "Utils/Logger.h"
#include <chrono>

FlexibleEACServer::FlexibleEACServer(std::shared_ptr<SecurityConfig> config)
    : m_config(std::move(config))
    , m_emulator()
    , m_detector()
    , m_memoryScanner()
{}

FlexibleEACServer::~FlexibleEACServer() {
    Shutdown();
}

bool FlexibleEACServer::Initialize(uint16_t listenPort) {
    if (!m_config) return false;

    // Bind UDP for emulator
    if (!m_socket.Bind(listenPort)) {
        Logger::Error("FlexibleEACServer: UDP bind failed on port %u", listenPort);
        return false;
    }

    // Initialize emulator
    if (!m_emulator.Initialize(listenPort)) return false;
    m_emulator.SetDetectionCallback([this](uint32_t id, EACDetectionResult r, const std::string& d){
        OnChallenge(id, d);
    });

    // Initialize detector
    if (!m_detector.Initialize()) return false;
    m_detector.SetDetectionCallback([this](uint32_t id, EACDetectionResult r, const std::string& d){
        OnDetector(id, r, d);
    });

    // Load memory signatures from config
    auto sigs = m_config->GetEACSignatures();
    if (!m_memoryScanner.Initialize(sigs,
                        std::chrono::milliseconds(m_config->GetInt("EAC.ScanTimeoutMs", 5000))))
    {
        return false;
    }
    m_memoryScanner.SetScanCallback([this](uint32_t id, EACScanResult r, const std::string& d){
        OnMemoryScan(id, r, d);
    });

    Logger::Info("FlexibleEACServer: Initialized on port %u", listenPort);
    return true;
}

void FlexibleEACServer::ValidateClient(uint32_t clientId,
                                       const std::string& ip,
                                       uint16_t port,
                                       uintptr_t processHandle,
                                       const std::string& exePath)
{
    Session sess;
    sess.clientId       = clientId;
    sess.ip             = ip;
    sess.port           = port;
    sess.processHandle  = processHandle;
    sess.exePath        = exePath;
    sess.startTime      = std::chrono::steady_clock::now();
    m_sessions[clientId] = sess;

    // Kick off handshake
    m_emulator.DetectClient(clientId, exePath /*reuse handshake API*/);

    // Kick off exe check
    m_detector.DetectClient(clientId, exePath);

    // Kick off memory scan
    m_memoryScanner.ScanClient(clientId, processHandle);
}

void FlexibleEACServer::Update() {
    // Process network/emulator
    ProcessEmulator();

    // Poll detectors and scanners
    m_detector.Poll();
    m_memoryScanner.Poll();

    // Dispatch all accumulated reports
    for (auto& rpt : m_reports) {
        DispatchReport(rpt);
    }
    m_reports.clear();
}

void FlexibleEACServer::ProcessEmulator() {
    std::string ip; uint16_t port;
    std::vector<uint8_t> buf(1500);
    int len;
    while ((len = m_socket.ReceiveFrom(ip, port, buf.data(), (int)buf.size())) > 0) {
        buf.resize(len);
        m_emulator.HandlePacket(ip, port, buf);
    }
}

void FlexibleEACServer::Shutdown() {
    m_emulator.Shutdown();
    m_memoryScanner.Shutdown();
    m_detector.Shutdown();
    m_socket.Close();
    Logger::Info("FlexibleEACServer: Shutdown complete");
}

void FlexibleEACServer::SetReportCallback(ReportCallback cb) {
    m_reportCb = std::move(cb);
}

bool FlexibleEACServer::ReloadConfig() {
    // Re-read signatures, timeouts, emulator settings, etc.
    // Implementation depends on SecurityConfig support
    return true;
}

void FlexibleEACServer::OnChallenge(uint32_t clientId, const std::string& details) {
    FlexibleEACReport rpt{clientId, FlexibleEACResult::Clean, "Handshake: " + details, NowMs()};
    m_reports.push_back(rpt);
}

void FlexibleEACServer::OnDetector(uint32_t clientId, EACDetectionResult res, const std::string& details) {
    FlexibleEACResult er = (res == EACDetectionResult::Clean
                           ? FlexibleEACResult::Clean
                           : FlexibleEACResult::ExecutableTampered);
    FlexibleEACReport rpt{clientId, er, "Exe: " + details, NowMs()};
    m_reports.push_back(rpt);
}

void FlexibleEACServer::OnMemoryScan(uint32_t clientId, EACScanResult res, const std::string& details) {
    FlexibleEACResult er = (res == EACScanResult::Clean
                           ? FlexibleEACResult::Clean
                           : FlexibleEACResult::MemoryTampered);
    FlexibleEACReport rpt{clientId, er, "Mem: " + details, NowMs()};
    m_reports.push_back(rpt);
}

void FlexibleEACServer::DispatchReport(const FlexibleEACReport& rpt) {
    if (m_reportCb) {
        m_reportCb(rpt);
    }
}

uint64_t FlexibleEACServer::NowMs() const {
    auto tp = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp).count();
}