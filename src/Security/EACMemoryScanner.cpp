// src/Security/EACMemoryScanner.cpp
#include "Security/EACMemoryScanner.h"
#ifdef _WIN32
#include <windows.h>
#endif
#include "Utils/Logger.h"
#include <thread>
#include <future>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace {
    std::string FormatHexAddr(uintptr_t addr) {
        std::ostringstream oss;
        oss << std::hex << std::setfill('0') << std::setw(sizeof(uintptr_t)*2) << addr;
        return oss.str();
    }
}

EACMemoryScanner::EACMemoryScanner() {
    Logger::Trace("[EACMemoryScanner::EACMemoryScanner] Default constructor called");
}

EACMemoryScanner::~EACMemoryScanner() {
    Logger::Trace("[EACMemoryScanner::~EACMemoryScanner] Destructor called, invoking Shutdown");
    Shutdown();
    Logger::Trace("[EACMemoryScanner::~EACMemoryScanner] Destructor completed");
}

bool EACMemoryScanner::Initialize(const std::vector<ScanSignature>& signatures,
                                  std::chrono::milliseconds timeout)
{
    Logger::Trace("[EACMemoryScanner::Initialize] Entry, signatures count=%zu, timeout=%llu ms",
                  signatures.size(), static_cast<unsigned long long>(timeout.count()));
    m_signatures = signatures;
    m_timeout = timeout;
    for (size_t i = 0; i < signatures.size(); ++i) {
        Logger::Debug("[EACMemoryScanner::Initialize] Signature[%zu]: name='%s', pattern size=%zu, mask size=%zu",
                      i, signatures[i].name.c_str(), signatures[i].pattern.size(), signatures[i].mask.size());
    }
    Logger::Info("EACMemoryScanner: initialized with %zu signatures, timeout=%lums",
                 signatures.size(), timeout.count());
    Logger::Trace("[EACMemoryScanner::Initialize] Exit, returning true");
    return true;
}

void EACMemoryScanner::Shutdown() {
    Logger::Trace("[EACMemoryScanner::Shutdown] Entry, active sessions=%zu", m_sessions.size());
    m_sessions.clear();
    Logger::Info("EACMemoryScanner: shutdown");
    Logger::Debug("[EACMemoryScanner::Shutdown] All sessions cleared");
    Logger::Trace("[EACMemoryScanner::Shutdown] Exit");
}

void EACMemoryScanner::SetScanCallback(ScanCallback cb) {
    Logger::Trace("[EACMemoryScanner::SetScanCallback] Entry, callback is %s", cb ? "non-null" : "null");
    m_callback = std::move(cb);
    Logger::Debug("[EACMemoryScanner::SetScanCallback] Scan callback has been set");
    Logger::Trace("[EACMemoryScanner::SetScanCallback] Exit");
}

void EACMemoryScanner::ScanClient(uint32_t clientId, uintptr_t processHandle) {
    Logger::Trace("[EACMemoryScanner::ScanClient] Entry, clientId=%u, processHandle=0x%llx",
                  clientId, static_cast<unsigned long long>(processHandle));
    Session session{ clientId, processHandle,
                     std::chrono::steady_clock::now(), true };
    m_sessions.push_back(session);
    Logger::Info("[EACMemoryScanner::ScanClient] Started memory scan session for client %u with processHandle=0x%llx",
                 clientId, static_cast<unsigned long long>(processHandle));
    Logger::Debug("[EACMemoryScanner::ScanClient] Total active scan sessions: %zu", m_sessions.size());
    Logger::Trace("[EACMemoryScanner::ScanClient] Exit");
}

void EACMemoryScanner::Poll() {
    Logger::Trace("[EACMemoryScanner::Poll] Entry, session count=%zu", m_sessions.size());
    auto now = std::chrono::steady_clock::now();
    for (auto& session : m_sessions) {
        if (!session.inProgress) {
            Logger::Trace("[EACMemoryScanner::Poll] Skipping completed session for client %u", session.clientId);
            continue;
        }
        // Launch async scan on first poll
        if (session.startTime == now) {
            Logger::Debug("[EACMemoryScanner::Poll] Launching async memory scan for client %u, processHandle=0x%llx",
                          session.clientId, static_cast<unsigned long long>(session.processHandle));
            std::async(std::launch::async, [this, &session]() {
                Logger::Trace("[EACMemoryScanner::Poll::async] Async memory scan started for client %u", session.clientId);
                std::string details;
                EACScanResult result = PerformScan(session, details);
                session.inProgress = false;
                Logger::Info("EACMemoryScanner: client %u scan result %d: %s",
                             session.clientId, int(result), details.c_str());
                Logger::Debug("[EACMemoryScanner::Poll::async] Scan completed for client %u: result=%d, details='%s'",
                              session.clientId, int(result), details.c_str());
                if (m_callback) {
                    Logger::Debug("[EACMemoryScanner::Poll::async] Invoking scan callback for client %u", session.clientId);
                    m_callback(session.clientId, result, details);
                    Logger::Trace("[EACMemoryScanner::Poll::async] Scan callback returned for client %u", session.clientId);
                } else {
                    Logger::Debug("[EACMemoryScanner::Poll::async] No scan callback set, skipping notification for client %u",
                                  session.clientId);
                }
            });
        }
        // Timeout check
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - session.startTime);
        if (now - session.startTime > m_timeout) {
            Logger::Warn("[EACMemoryScanner::Poll] Memory scan timed out for client %u after %lld ms (timeout=%llu ms)",
                         session.clientId, static_cast<long long>(elapsed.count()),
                         static_cast<unsigned long long>(m_timeout.count()));
            session.inProgress = false;
            if (m_callback) {
                Logger::Debug("[EACMemoryScanner::Poll] Invoking timeout callback for client %u", session.clientId);
                m_callback(session.clientId, EACScanResult::Timeout, "Scan timed out");
            }
        } else {
            Logger::Trace("[EACMemoryScanner::Poll] Session for client %u still in progress, elapsed=%lld ms",
                          session.clientId, static_cast<long long>(elapsed.count()));
        }
    }
    // Remove completed sessions
    size_t beforeSize = m_sessions.size();
    m_sessions.erase(
        std::remove_if(m_sessions.begin(), m_sessions.end(),
                       [](const Session& s){ return !s.inProgress; }),
        m_sessions.end());
    size_t afterSize = m_sessions.size();
    if (beforeSize != afterSize) {
        Logger::Debug("[EACMemoryScanner::Poll] Removed %zu completed sessions, %zu remaining",
                      beforeSize - afterSize, afterSize);
    }
    Logger::Trace("[EACMemoryScanner::Poll] Exit, remaining sessions=%zu", m_sessions.size());
}

EACScanResult EACMemoryScanner::PerformScan(const Session& s, std::string& outDetails) {
    Logger::Trace("[EACMemoryScanner::PerformScan] Entry, clientId=%u, processHandle=0x%llx",
                  s.clientId, static_cast<unsigned long long>(s.processHandle));
    // Example: scan first 1MB of process memory in pages
    const uintptr_t base = 0x00400000;
    const size_t regionSize = 1024 * 1024;
    const size_t pageSize = 4096;
    std::vector<uint8_t> buffer(pageSize);

    Logger::Debug("[EACMemoryScanner::PerformScan] Scanning memory region: base=0x%s, regionSize=%zu bytes, pageSize=%zu bytes",
                  FormatHexAddr(base).c_str(), regionSize, pageSize);
    Logger::Info("[EACMemoryScanner::PerformScan] Beginning memory scan for client %u: %zu signatures to check across %zu pages",
                 s.clientId, m_signatures.size(), regionSize / pageSize);

    size_t pagesScanned = 0;
    for (uintptr_t addr = base; addr < base + regionSize; addr += pageSize) {
        Logger::Trace("[EACMemoryScanner::PerformScan] Reading page at address 0x%s for client %u",
                      FormatHexAddr(addr).c_str(), s.clientId);
        if (!ReadMemory(s.processHandle, addr, buffer.data(), pageSize)) {
            Logger::Error("[EACMemoryScanner::PerformScan] Failed to read memory at 0x%s for client %u (processHandle=0x%llx)",
                          FormatHexAddr(addr).c_str(), s.clientId, static_cast<unsigned long long>(s.processHandle));
            Logger::Trace("[EACMemoryScanner::PerformScan] Exit, returning MemoryReadError");
            return EACScanResult::MemoryReadError;
        }
        ++pagesScanned;
        for (const auto& sig : m_signatures) {
            uintptr_t matchAddr = 0;
            Logger::Trace("[EACMemoryScanner::PerformScan] Checking signature '%s' at page 0x%s",
                          sig.name.c_str(), FormatHexAddr(addr).c_str());
            if (MatchPattern(buffer.data(), pageSize, sig.pattern, sig.mask, addr, matchAddr)) {
                outDetails = "Signature '" + sig.name + "' matched at 0x"
                             + FormatHexAddr(matchAddr);
                Logger::Warn("[EACMemoryScanner::PerformScan] CHEAT SIGNATURE DETECTED for client %u: '%s' at address 0x%s",
                             s.clientId, sig.name.c_str(), FormatHexAddr(matchAddr).c_str());
                Logger::Info("[EACMemoryScanner::PerformScan] Anti-cheat memory scan: signature match found - client %u flagged",
                             s.clientId);
                Logger::Trace("[EACMemoryScanner::PerformScan] Exit, returning SignatureMatch");
                return EACScanResult::SignatureMatch;
            }
        }
    }
    outDetails = "No signatures found";
    Logger::Info("[EACMemoryScanner::PerformScan] Memory scan clean for client %u: scanned %zu pages, no signatures matched",
                 s.clientId, pagesScanned);
    Logger::Debug("[EACMemoryScanner::PerformScan] All %zu signatures checked against %zu pages - all clean",
                  m_signatures.size(), pagesScanned);
    Logger::Trace("[EACMemoryScanner::PerformScan] Exit, returning Clean");
    return EACScanResult::Clean;
}

bool EACMemoryScanner::ReadMemory(uintptr_t procHandle, uintptr_t address,
                                  void* buffer, size_t size)
{
    Logger::Trace("[EACMemoryScanner::ReadMemory] Entry, procHandle=0x%llx, address=0x%s, size=%zu",
                  static_cast<unsigned long long>(procHandle), FormatHexAddr(address).c_str(), size);
    // Platform-specific: e.g., ReadProcessMemory on Windows
#ifdef _WIN32
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory((HANDLE)procHandle, (LPCVOID)address,
                           buffer, size, &bytesRead) || bytesRead != size)
    {
        Logger::Error("[EACMemoryScanner::ReadMemory] ReadProcessMemory failed at 0x%s, requested=%zu, bytesRead=%zu",
                      FormatHexAddr(address).c_str(), size, static_cast<size_t>(bytesRead));
        Logger::Trace("[EACMemoryScanner::ReadMemory] Exit, returning false (Win32 read failed)");
        return false;
    }
    Logger::Trace("[EACMemoryScanner::ReadMemory] Successfully read %zu bytes from 0x%s",
                  static_cast<size_t>(bytesRead), FormatHexAddr(address).c_str());
    Logger::Trace("[EACMemoryScanner::ReadMemory] Exit, returning true");
    return true;
#else
    // POSIX or stub
    Logger::Debug("[EACMemoryScanner::ReadMemory] Non-Win32 platform: memory read not implemented (stub returning false)");
    Logger::Trace("[EACMemoryScanner::ReadMemory] Exit, returning false (POSIX stub)");
    return false;
#endif
}

bool EACMemoryScanner::MatchPattern(const uint8_t* data, size_t dataLen,
                                    const std::vector<uint8_t>& pattern,
                                    const std::string& mask,
                                    uintptr_t baseAddress,
                                    uintptr_t& matchAddress)
{
    Logger::Trace("[EACMemoryScanner::MatchPattern] Entry, dataLen=%zu, patternLen=%zu, maskLen=%zu, baseAddress=0x%s",
                  dataLen, pattern.size(), mask.size(), FormatHexAddr(baseAddress).c_str());
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
            Logger::Debug("[EACMemoryScanner::MatchPattern] Pattern matched at offset %zu (absolute address 0x%s)",
                          i, FormatHexAddr(matchAddress).c_str());
            Logger::Trace("[EACMemoryScanner::MatchPattern] Exit, returning true, matchAddress=0x%s",
                          FormatHexAddr(matchAddress).c_str());
            return true;
        }
    }
    Logger::Trace("[EACMemoryScanner::MatchPattern] No match found in %zu bytes of data", dataLen);
    Logger::Trace("[EACMemoryScanner::MatchPattern] Exit, returning false");
    return false;
}
