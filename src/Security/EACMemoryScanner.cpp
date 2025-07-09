// src/Security/EACMemoryScanner.cpp
#include "Security/EACMemoryScanner.h"
#include "Utils/Logger.h"
#include <thread>
#include <future>
#include <cstring>

EACMemoryScanner::EACMemoryScanner() = default;
EACMemoryScanner::~EACMemoryScanner() {
    Shutdown();
}

bool EACMemoryScanner::Initialize(const std::vector<ScanSignature>& signatures,
                                  std::chrono::milliseconds timeout)
{
    m_signatures = signatures;
    m_timeout = timeout;
    Logger::Info("EACMemoryScanner: initialized with %zu signatures, timeout=%lums",
                 signatures.size(), timeout.count());
    return true;
}

void EACMemoryScanner::Shutdown() {
    m_sessions.clear();
    Logger::Info("EACMemoryScanner: shutdown");
}

void EACMemoryScanner::SetScanCallback(ScanCallback cb) {
    m_callback = std::move(cb);
}

void EACMemoryScanner::ScanClient(uint32_t clientId, uintptr_t processHandle) {
    Session session{ clientId, processHandle,
                     std::chrono::steady_clock::now(), true };
    m_sessions.push_back(session);
}

void EACMemoryScanner::Poll() {
    auto now = std::chrono::steady_clock::now();
    for (auto& session : m_sessions) {
        if (!session.inProgress) continue;
        // Launch async scan on first poll
        if (session.startTime == now) {
            std::async(std::launch::async, [this, &session]() {
                std::string details;
                EACScanResult result = PerformScan(session, details);
                session.inProgress = false;
                Logger::Info("EACMemoryScanner: client %u scan result %d: %s",
                             session.clientId, int(result), details.c_str());
                if (m_callback) {
                    m_callback(session.clientId, result, details);
                }
            });
        }
        // Timeout check
        if (now - session.startTime > m_timeout) {
            session.inProgress = false;
            if (m_callback) {
                m_callback(session.clientId, EACScanResult::Timeout, "Scan timed out");
            }
        }
    }
    // Remove completed sessions
    m_sessions.erase(
        std::remove_if(m_sessions.begin(), m_sessions.end(),
                       [](const Session& s){ return !s.inProgress; }),
        m_sessions.end());
}

EACScanResult EACMemoryScanner::PerformScan(const Session& s, std::string& outDetails) {
    // Example: scan first 1MB of process memory in pages
    const uintptr_t base = 0x00400000;
    const size_t regionSize = 1024 * 1024;
    const size_t pageSize = 4096;
    std::vector<uint8_t> buffer(pageSize);

    for (uintptr_t addr = base; addr < base + regionSize; addr += pageSize) {
        if (!ReadMemory(s.processHandle, addr, buffer.data(), pageSize)) {
            return EACScanResult::MemoryReadError;
        }
        for (const auto& sig : m_signatures) {
            uintptr_t matchAddr = 0;
            if (MatchPattern(buffer.data(), pageSize, sig.pattern, sig.mask, addr, matchAddr)) {
                outDetails = "Signature '" + sig.name + "' matched at 0x"
                             + Utils::FormatHex(matchAddr);
                return EACScanResult::SignatureMatch;
            }
        }
    }
    outDetails = "No signatures found";
    return EACScanResult::Clean;
}

bool EACMemoryScanner::ReadMemory(uintptr_t procHandle, uintptr_t address,
                                  void* buffer, size_t size)
{
    // Platform‐specific: e.g., ReadProcessMemory on Windows
#ifdef _WIN32
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory((HANDLE)procHandle, (LPCVOID)address,
                           buffer, size, &bytesRead) || bytesRead != size)
    {
        return false;
    }
    return true;
#else
    // POSIX or stub
    return false;
#endif
}

bool EACMemoryScanner::MatchPattern(const uint8_t* data, size_t dataLen,
                                    const std::vector<uint8_t>& pattern,
                                    const std::string& mask,
                                    uintptr_t baseAddress,
                                    uintptr_t& matchAddress)
{
    size_t patLen = pattern.size();
    for (size_t i = 0; i + patLen <= dataLen; ++i) {
        bool found = true;
        for (size_t j = 0; j < patLen; ++j) {
            if (mask[j] == 'x' && data[i + j] != pattern[j]) {
                found = false;
                break;
            }
        }
        if (found) {
            matchAddress = baseAddress + i;
            return true;
        }
    }
    return false;
}