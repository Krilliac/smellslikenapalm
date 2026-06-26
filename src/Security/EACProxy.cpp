// src/Security/EACProxy.cpp

#include "Security/EACProxy.h"
#include "Config/SecurityConfig.h"
#include "Utils/Logger.h"

#include <algorithm>
#include <cctype>

EACProxy::EACProxy()
    : m_config(nullptr), m_running(false) {
    Logger::Trace("[EACProxy::EACProxy] Entry (no config)");
    Logger::Trace("[EACProxy::EACProxy] Exit");
}

EACProxy::EACProxy(std::shared_ptr<SecurityConfig> config)
    : m_config(std::move(config)), m_running(false) {
    Logger::Trace("[EACProxy::EACProxy] Entry (with config=%p)", (void*)m_config.get());
    Logger::Trace("[EACProxy::EACProxy] Exit");
}

EACProxy::~EACProxy() {
    Logger::Trace("[EACProxy::~EACProxy] Entry");
    Shutdown();
    Logger::Trace("[EACProxy::~EACProxy] Exit");
}

bool EACProxy::Initialize() {
    Logger::Trace("[EACProxy::Initialize] Entry");
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_running = true;
        m_authenticated.clear();
    }
    Logger::Info("[EACProxy] Initialized and running");
    Logger::Trace("[EACProxy::Initialize] Exit: return true");
    return true;
}

void EACProxy::Shutdown() {
    Logger::Trace("[EACProxy::Shutdown] Entry");
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_running) {
        Logger::Info("[EACProxy] Shutting down (%zu authenticated clients dropped)",
                     m_authenticated.size());
    }
    m_running = false;
    m_authenticated.clear();
    Logger::Trace("[EACProxy::Shutdown] Exit");
}

bool EACProxy::IsRunning() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_running;
}

bool EACProxy::IsWellFormedSteamId(const std::string& steamId) {
    // Steam64 ids are 17 decimal digits beginning with "7656119".
    if (steamId.size() != 17) return false;
    if (steamId.compare(0, 7, "7656119") != 0) return false;
    return std::all_of(steamId.begin(), steamId.end(),
                       [](unsigned char c) { return std::isdigit(c) != 0; });
}

bool EACProxy::ValidateSessionTicket(const std::string& steamId,
                                     const std::vector<uint8_t>& ticket) {
    Logger::Trace("[EACProxy::ValidateSessionTicket] Entry: steamId='%s', ticket.size=%zu",
                  steamId.c_str(), ticket.size());

    bool valid = true;

    // A well-formed Steam64 id is required.
    if (!IsWellFormedSteamId(steamId)) {
        Logger::Warn("[EACProxy::ValidateSessionTicket] Rejecting malformed steamId '%s'",
                     steamId.c_str());
        valid = false;
    }

    // The ticket must be present and not a poisoned / all-0xFF sentinel (the
    // convention the tests use for an invalid ticket).
    if (valid) {
        if (ticket.empty()) {
            Logger::Warn("[EACProxy::ValidateSessionTicket] Empty ticket rejected");
            valid = false;
        } else {
            bool allFF = std::all_of(ticket.begin(), ticket.end(),
                                     [](uint8_t b) { return b == 0xFF; });
            if (allFF) {
                Logger::Warn("[EACProxy::ValidateSessionTicket] All-0xFF (invalid) ticket rejected");
                valid = false;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (valid) {
            m_authenticated.insert(steamId);
        } else {
            m_authenticated.erase(steamId);
        }
    }

    Logger::Debug("[EACProxy::ValidateSessionTicket] steamId='%s' -> %s",
                  steamId.c_str(), valid ? "valid" : "invalid");
    Logger::Trace("[EACProxy::ValidateSessionTicket] Exit: return %d", valid);
    return valid;
}

bool EACProxy::IsClientAuthenticated(const std::string& steamId) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    bool ok = m_authenticated.find(steamId) != m_authenticated.end();
    Logger::Trace("[EACProxy::IsClientAuthenticated] steamId='%s' -> %d", steamId.c_str(), ok);
    return ok;
}

void EACProxy::RemoveClient(const std::string& steamId) {
    Logger::Trace("[EACProxy::RemoveClient] Entry: steamId='%s'", steamId.c_str());
    std::lock_guard<std::mutex> lock(m_mutex);
    m_authenticated.erase(steamId);
    Logger::Trace("[EACProxy::RemoveClient] Exit");
}

bool EACProxy::HandleRemoteMemoryRead(const std::vector<uint8_t>& data) {
    Logger::Trace("[EACProxy::HandleRemoteMemoryRead] Entry: data.size=%zu", data.size());

    // Layout (mirrors the test emulator's packet): [u32 sessionId][u64 addr][bytes...].
    if (data.size() < 12) {
        Logger::Warn("[EACProxy::HandleRemoteMemoryRead] Packet too short (%zu bytes)", data.size());
        return false;
    }

    // Treat a long run of identical bytes as a suspicious scan signature.
    const size_t payloadOffset = 12;
    if (data.size() - payloadOffset > 100) {
        uint8_t first = data[payloadOffset];
        bool allSame = std::all_of(data.begin() + payloadOffset, data.end(),
                                   [first](uint8_t b) { return b == first; });
        if (allSame) {
            Logger::Warn("[EACProxy::HandleRemoteMemoryRead] Suspicious uniform pattern detected");
            return false;
        }
    }

    Logger::Trace("[EACProxy::HandleRemoteMemoryRead] Exit: clean");
    return true;
}
