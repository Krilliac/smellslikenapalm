// src/Security/ClientEACDetector.cpp
#include "Security/ClientEACDetector.h"
#include "Utils/Logger.h"
#include <thread>
#include <future>
#include <fstream>

// Time allowed for a scan before timeout
static constexpr auto kScanTimeout = std::chrono::seconds(5);

ClientEACDetector::ClientEACDetector() = default;
ClientEACDetector::~ClientEACDetector() {
    Shutdown();
}

bool ClientEACDetector::Initialize() {
    Logger::Info("ClientEACDetector: Initialized");
    return true;
}

void ClientEACDetector::Shutdown() {
    m_sessions.clear();
    Logger::Info("ClientEACDetector: Shutdown");
}

void ClientEACDetector::SetDetectionCallback(DetectionCallback cb) {
    m_callback = std::move(cb);
}

void ClientEACDetector::DetectClient(uint32_t clientId, const std::string& clientExePath) {
    Session session{clientId, clientExePath, std::chrono::steady_clock::now(), true};
    m_sessions.push_back(session);
}

void ClientEACDetector::Poll() {
    auto now = std::chrono::steady_clock::now();
    for (auto& session : m_sessions) {
        if (!session.inProgress) continue;
        // If async: start a future on first poll
        if (session.startTime == now) {
            // Launch async scan
            auto fut = std::async(std::launch::async, [this, &session]() {
                std::string details;
                EACDetectionResult res = ScanExecutable(session.exePath, details);
                CompleteSession(session, res, details);
            });
        }
        // Check for timeout
        if (now - session.startTime > kScanTimeout) {
            CompleteSession(session, EACDetectionResult::Error, "Scan timed out");
        }
    }
    // Remove completed sessions
    m_sessions.erase(std::remove_if(m_sessions.begin(), m_sessions.end(),
        [](const Session& s){ return !s.inProgress; }), m_sessions.end());
}

void ClientEACDetector::CompleteSession(Session& session, EACDetectionResult result, const std::string& details) {
    session.inProgress = false;
    Logger::Info("ClientEACDetector: Client %u scan result %d: %s",
                 session.clientId, int(result), details.c_str());
    if (m_callback) {
        m_callback(session.clientId, result, details);
    }
}

EACDetectionResult ClientEACDetector::ScanExecutable(const std::string& path, std::string& outDetails) {
    // Simple stub: check file exists and matches expected signature
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        outDetails = "Executable not found";
        return EACDetectionResult::Error;
    }
    auto size = f.tellg();
    f.seekg(0, std::ios::beg);
    // Read first bytes to check header
    std::vector<uint8_t> header(4);
    f.read(reinterpret_cast<char*>(header.data()), header.size());
    // Example check: magic "MZ" at start
    if (header.size() >= 2 && header[0] == 'M' && header[1] == 'Z') {
        outDetails = "PE header OK";
        return EACDetectionResult::Clean;
    } else {
        outDetails = "Invalid executable header";
        return EACDetectionResult::Tampered;
    }
}