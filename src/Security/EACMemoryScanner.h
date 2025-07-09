// src/Security/EACMemoryScanner.h
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <chrono>

enum class EACScanResult {
    Clean,
    SignatureMatch,
    MemoryReadError,
    Timeout,
    Error
};

struct ScanSignature {
    std::string name;
    std::vector<uint8_t> pattern;
    std::string mask;   // 'x' = match byte, '?' = wildcard
};

class EACMemoryScanner {
public:
    using ScanCallback = std::function<void(uint32_t clientId, EACScanResult result, const std::string& details)>;

    EACMemoryScanner();
    ~EACMemoryScanner();

    // Initialize scanner (load signatures from config)
    bool Initialize(const std::vector<ScanSignature>& signatures,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

    // Shutdown and release resources
    void Shutdown();

    // Begin scanning a client’s process memory asynchronously
    void ScanClient(uint32_t clientId, uintptr_t processHandle);

    // Poll for completed scans and invoke callback
    void Poll();

    // Set callback to receive scan results
    void SetScanCallback(ScanCallback cb);

private:
    struct Session {
        uint32_t clientId;
        uintptr_t processHandle;
        std::chrono::steady_clock::time_point startTime;
        bool inProgress;
    };

    std::vector<ScanSignature>       m_signatures;
    std::chrono::milliseconds        m_timeout;
    ScanCallback                     m_callback;
    std::vector<Session>             m_sessions;

    // Internal: perform the actual memory scan for one session
    EACScanResult PerformScan(const Session& s, std::string& outDetails);

    // Helper: read process memory safely
    bool ReadMemory(uintptr_t procHandle, uintptr_t address, void* buffer, size_t size);

    // Helper: search a buffer for a signature pattern
    bool MatchPattern(const uint8_t* data, size_t dataLen,
                      const std::vector<uint8_t>& pattern,
                      const std::string& mask,
                      uintptr_t baseAddress, uintptr_t& matchAddress);
};