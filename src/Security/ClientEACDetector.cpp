// src/Security/ClientEACDetector.cpp
#include "Security/ClientEACDetector.h"
#include "Utils/Logger.h"
#include <thread>
#include <future>
#include <fstream>

// Time allowed for a scan before timeout
static constexpr auto kScanTimeout = std::chrono::seconds(5);

ClientEACDetector::ClientEACDetector() {
    Logger::Trace("[ClientEACDetector::ClientEACDetector] Default constructor called");
}

ClientEACDetector::~ClientEACDetector() {
    Logger::Trace("[ClientEACDetector::~ClientEACDetector] Destructor called, invoking Shutdown");
    Shutdown();
    Logger::Trace("[ClientEACDetector::~ClientEACDetector] Destructor completed");
}

bool ClientEACDetector::Initialize() {
    Logger::Trace("[ClientEACDetector::Initialize] Entry");
    Logger::Info("[ClientEACDetector::Initialize] Initializing ClientEACDetector");
    Logger::Debug("[ClientEACDetector::Initialize] Scan timeout configured to %lld seconds",
                  static_cast<long long>(kScanTimeout.count()));
    Logger::Info("ClientEACDetector: Initialized");
    Logger::Trace("[ClientEACDetector::Initialize] Returning true");
    return true;
}

void ClientEACDetector::Shutdown() {
    Logger::Trace("[ClientEACDetector::Shutdown] Entry, current session count=%zu", m_sessions.size());
    m_sessions.clear();
    Logger::Info("ClientEACDetector: Shutdown");
    Logger::Debug("[ClientEACDetector::Shutdown] All sessions cleared");
    Logger::Trace("[ClientEACDetector::Shutdown] Exit");
}

void ClientEACDetector::SetDetectionCallback(DetectionCallback cb) {
    Logger::Trace("[ClientEACDetector::SetDetectionCallback] Entry, callback is %s",
                  cb ? "non-null" : "null");
    m_callback = std::move(cb);
    Logger::Debug("[ClientEACDetector::SetDetectionCallback] Detection callback has been set");
    Logger::Trace("[ClientEACDetector::SetDetectionCallback] Exit");
}

void ClientEACDetector::DetectClient(uint32_t clientId, const std::string& clientExePath) {
    Logger::Trace("[ClientEACDetector::DetectClient] Entry, clientId=%u, clientExePath='%s'",
                  clientId, clientExePath.c_str());
    Session session{clientId, clientExePath, std::chrono::steady_clock::now(), true};
    m_sessions.push_back(session);
    Logger::Info("[ClientEACDetector::DetectClient] Started detection session for client %u with exe path '%s'",
                 clientId, clientExePath.c_str());
    Logger::Debug("[ClientEACDetector::DetectClient] Total active sessions now: %zu", m_sessions.size());
    Logger::Trace("[ClientEACDetector::DetectClient] Exit");
}

void ClientEACDetector::Poll() {
    Logger::Trace("[ClientEACDetector::Poll] Entry, session count=%zu", m_sessions.size());
    auto now = std::chrono::steady_clock::now();
    for (auto& session : m_sessions) {
        if (!session.inProgress) {
            Logger::Trace("[ClientEACDetector::Poll] Skipping completed session for client %u", session.clientId);
            continue;
        }
        // If async: start a future on first poll
        if (session.startTime == now) {
            Logger::Debug("[ClientEACDetector::Poll] Launching async scan for client %u, exePath='%s'",
                          session.clientId, session.exePath.c_str());
            // Launch async scan
            auto fut = std::async(std::launch::async, [this, &session]() {
                Logger::Trace("[ClientEACDetector::Poll::async] Async scan started for client %u", session.clientId);
                std::string details;
                EACDetectionResult res = ScanExecutable(session.exePath, details);
                Logger::Debug("[ClientEACDetector::Poll::async] Async scan completed for client %u, result=%d, details='%s'",
                              session.clientId, int(res), details.c_str());
                CompleteSession(session, res, details);
            });
        }
        // Check for timeout
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - session.startTime);
        if (now - session.startTime > kScanTimeout) {
            Logger::Warn("[ClientEACDetector::Poll] Scan timed out for client %u after %lld ms",
                         session.clientId, static_cast<long long>(elapsed.count()));
            CompleteSession(session, EACDetectionResult::Error, "Scan timed out");
        } else {
            Logger::Trace("[ClientEACDetector::Poll] Session for client %u still in progress, elapsed=%lld ms",
                          session.clientId, static_cast<long long>(elapsed.count()));
        }
    }
    // Remove completed sessions
    size_t beforeSize = m_sessions.size();
    m_sessions.erase(std::remove_if(m_sessions.begin(), m_sessions.end(),
        [](const Session& s){ return !s.inProgress; }), m_sessions.end());
    size_t afterSize = m_sessions.size();
    if (beforeSize != afterSize) {
        Logger::Debug("[ClientEACDetector::Poll] Removed %zu completed sessions, %zu remaining",
                      beforeSize - afterSize, afterSize);
    }
    Logger::Trace("[ClientEACDetector::Poll] Exit, remaining sessions=%zu", m_sessions.size());
}

void ClientEACDetector::CompleteSession(Session& session, EACDetectionResult result, const std::string& details) {
    Logger::Trace("[ClientEACDetector::CompleteSession] Entry, clientId=%u, result=%d, details='%s'",
                  session.clientId, int(result), details.c_str());
    session.inProgress = false;
    Logger::Info("ClientEACDetector: Client %u scan result %d: %s",
                 session.clientId, int(result), details.c_str());
    Logger::Debug("[ClientEACDetector::CompleteSession] Session marked as completed for client %u", session.clientId);
    if (m_callback) {
        Logger::Debug("[ClientEACDetector::CompleteSession] Invoking detection callback for client %u", session.clientId);
        m_callback(session.clientId, result, details);
        Logger::Trace("[ClientEACDetector::CompleteSession] Detection callback returned for client %u", session.clientId);
    } else {
        Logger::Debug("[ClientEACDetector::CompleteSession] No detection callback set, skipping notification for client %u",
                      session.clientId);
    }
    Logger::Trace("[ClientEACDetector::CompleteSession] Exit");
}

EACDetectionResult ClientEACDetector::ScanExecutable(const std::string& path, std::string& outDetails) {
    Logger::Trace("[ClientEACDetector::ScanExecutable] Entry, path='%s'", path.c_str());
    Logger::Info("[ClientEACDetector::ScanExecutable] Beginning executable scan for '%s'", path.c_str());
    // Simple stub: check file exists and matches expected signature
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        outDetails = "Executable not found";
        Logger::Error("[ClientEACDetector::ScanExecutable] Failed to open executable file '%s' - file not found or access denied",
                      path.c_str());
        Logger::Trace("[ClientEACDetector::ScanExecutable] Returning EACDetectionResult::Error");
        return EACDetectionResult::Error;
    }
    Logger::Debug("[ClientEACDetector::ScanExecutable] File opened successfully: '%s'", path.c_str());
    auto size = f.tellg();
    Logger::Debug("[ClientEACDetector::ScanExecutable] File size: %lld bytes", static_cast<long long>(size));
    f.seekg(0, std::ios::beg);
    // Read first bytes to check header
    std::vector<uint8_t> header(4);
    f.read(reinterpret_cast<char*>(header.data()), header.size());
    Logger::Trace("[ClientEACDetector::ScanExecutable] Read %zu header bytes: [0x%02X, 0x%02X, 0x%02X, 0x%02X]",
                  header.size(),
                  header.size() > 0 ? header[0] : 0,
                  header.size() > 1 ? header[1] : 0,
                  header.size() > 2 ? header[2] : 0,
                  header.size() > 3 ? header[3] : 0);
    // Example check: magic "MZ" at start
    if (header.size() >= 2 && header[0] == 'M' && header[1] == 'Z') {
        outDetails = "PE header OK";
        Logger::Info("[ClientEACDetector::ScanExecutable] PE header verified successfully for '%s' - executable is clean",
                     path.c_str());
        Logger::Debug("[ClientEACDetector::ScanExecutable] MZ magic bytes detected at offset 0, valid PE executable");
        Logger::Trace("[ClientEACDetector::ScanExecutable] Returning EACDetectionResult::Clean");
        return EACDetectionResult::Clean;
    } else {
        outDetails = "Invalid executable header";
        Logger::Warn("[ClientEACDetector::ScanExecutable] Invalid executable header for '%s' - expected 'MZ' but got [0x%02X, 0x%02X]",
                     path.c_str(),
                     header.size() > 0 ? header[0] : 0,
                     header.size() > 1 ? header[1] : 0);
        Logger::Info("[ClientEACDetector::ScanExecutable] Executable '%s' detected as tampered due to invalid header",
                     path.c_str());
        Logger::Trace("[ClientEACDetector::ScanExecutable] Returning EACDetectionResult::Tampered");
        return EACDetectionResult::Tampered;
    }
}
