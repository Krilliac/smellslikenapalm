// src/Security/ClientEACDetector.h
#pragma once

#include <cstdint>
#include <string>
#include <chrono>
#include <functional>

enum class EACDetectionResult {
    Clean,
    Tampered,
    Unknown,
    Error
};

class ClientEACDetector {
public:
    using DetectionCallback = std::function<void(uint32_t clientId, EACDetectionResult result, const std::string& details)>;

    ClientEACDetector();
    ~ClientEACDetector();

    // Initialize detector (e.g., load signatures, configure environment)
    bool Initialize();

    // Shutdown detector, free resources
    void Shutdown();

    // Perform detection for a given client; asynchronous if callback provided
    void DetectClient(uint32_t clientId, const std::string& clientExePath);

    // Set callback invoked when detection completes
    void SetDetectionCallback(DetectionCallback cb);

    // Poll for any pending detection results (if using synchronous mode)
    void Poll();

private:
    DetectionCallback m_callback;

    // Internal state per client
    struct Session {
        uint32_t clientId;
        std::string exePath;
        std::chrono::steady_clock::time_point startTime;
        bool inProgress;
    };
    std::vector<Session> m_sessions;

    // Internal helpers
    EACDetectionResult ScanExecutable(const std::string& path, std::string& outDetails);
    void CompleteSession(Session& session, EACDetectionResult result, const std::string& details);
};