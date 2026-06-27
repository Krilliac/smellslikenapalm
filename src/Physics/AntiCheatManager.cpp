#include "Physics/AntiCheatManager.h"
#include "Game/GameServer.h"
#include "Game/AdminManager.h"
#include "Network/ClientConnection.h"
#include "Utils/Logger.h"
#include "../../telemetry/TelemetryManager.h"
#include <algorithm>

AntiCheatManager::AntiCheatManager(GameServer* server, std::shared_ptr<SecurityConfig> cfg)
    : m_server(server), m_config(cfg)
{
    Logger::Trace("[AntiCheatManager::AntiCheatManager] Entry - server=%p, config=%p",
                  static_cast<void*>(server), static_cast<void*>(cfg.get()));
    m_allowedTags = {
        "CHAT_MESSAGE","PLAYER_ACTION","HEARTBEAT",
        "MOVE","SHOOT","RELOAD"
    };
    m_maxViolations = 5; // default max violations
    Logger::Debug("[AntiCheatManager::AntiCheatManager] Allowed tags initialized: count=%zu, maxViolations=%d",
                  m_allowedTags.size(), m_maxViolations);
    for (size_t i = 0; i < m_allowedTags.size(); ++i) {
        Logger::Debug("[AntiCheatManager::AntiCheatManager] Allowed tag[%zu]='%s'", i, m_allowedTags[i].c_str());
    }
    Logger::Info("[AntiCheatManager::AntiCheatManager] AntiCheatManager constructed with %zu allowed tags and maxViolations=%d",
                 m_allowedTags.size(), m_maxViolations);
    Logger::Trace("[AntiCheatManager::AntiCheatManager] Exit");
}

AntiCheatManager::~AntiCheatManager() {
    Logger::Trace("[AntiCheatManager::~AntiCheatManager] Entry - tracked violations for %zu clients", m_violations.size());
    Logger::Info("[AntiCheatManager::~AntiCheatManager] AntiCheatManager destroyed, was tracking violations for %zu clients", m_violations.size());
    Logger::Trace("[AntiCheatManager::~AntiCheatManager] Exit");
}

bool AntiCheatManager::Initialize() {
    Logger::Trace("[AntiCheatManager::Initialize] Entry");
    Logger::Info("AntiCheatManager initialized (max violations=%d)", m_maxViolations);
    Logger::Debug("[AntiCheatManager::Initialize] Initialization complete with maxViolations=%d, allowedTags count=%zu",
                  m_maxViolations, m_allowedTags.size());
    Logger::Trace("[AntiCheatManager::Initialize] Exit - returning true");
    return true;
}

void AntiCheatManager::OnReceive(uint32_t clientId, const Packet& pkt, const PacketMetadata& /*meta*/) {
    Logger::Trace("[AntiCheatManager::OnReceive] Entry - clientId=%u, packet tag='%s', incoming=true",
                  clientId, pkt.GetTag().c_str());
    Logger::Debug("[AntiCheatManager::OnReceive] Forwarding received packet from client %u with tag '%s' to InspectPacket",
                  clientId, pkt.GetTag().c_str());
    InspectPacket(clientId, pkt, true);
    Logger::Trace("[AntiCheatManager::OnReceive] Exit");
}

void AntiCheatManager::OnSend(uint32_t clientId, const Packet& pkt) {
    Logger::Trace("[AntiCheatManager::OnSend] Entry - clientId=%u, packet tag='%s', incoming=false",
                  clientId, pkt.GetTag().c_str());
    Logger::Debug("[AntiCheatManager::OnSend] Forwarding outgoing packet for client %u with tag '%s' to InspectPacket",
                  clientId, pkt.GetTag().c_str());
    InspectPacket(clientId, pkt, false);
    Logger::Trace("[AntiCheatManager::OnSend] Exit");
}

void AntiCheatManager::Update() {
    Logger::Trace("[AntiCheatManager::Update] Entry - tracked clients=%zu", m_violations.size());
    // Periodically decay violations
    int decayedCount = 0;
    std::lock_guard<std::mutex> lock(m_violationsMutex);
    for (auto& kv : m_violations) {
        int oldVal = kv.second;
        kv.second = std::max(0, kv.second - 1);
        if (oldVal != kv.second) {
            decayedCount++;
            Logger::Debug("[AntiCheatManager::Update] Client %u violations decayed from %d to %d", kv.first, oldVal, kv.second);
        }
    }
    Logger::Debug("[AntiCheatManager::Update] Violation decay complete: %d clients had violations decayed out of %zu tracked clients",
                  decayedCount, m_violations.size());
    Logger::Trace("[AntiCheatManager::Update] Exit");
}

void AntiCheatManager::InspectPacket(uint32_t clientId, const Packet& pkt, bool incoming) {
    Logger::Trace("[AntiCheatManager::InspectPacket] Entry - clientId=%u, tag='%s', incoming=%s",
                  clientId, pkt.GetTag().c_str(), incoming ? "true" : "false");
    const auto& tag = pkt.GetTag();
    bool allowed = std::find(m_allowedTags.begin(), m_allowedTags.end(), tag) != m_allowedTags.end();
    Logger::Debug("[AntiCheatManager::InspectPacket] Client %u packet tag '%s' (direction=%s): allowed=%s",
                  clientId, tag.c_str(), incoming ? "incoming" : "outgoing", allowed ? "true" : "false");
    if (!allowed) {
        int v;
        {
            std::lock_guard<std::mutex> lock(m_violationsMutex);
            v = ++m_violations[clientId];
        }
        TELEMETRY_INCREMENT_SECURITY_VIOLATION();
        Logger::Warn("AntiCheat: client %u sent disallowed packet '%s' (violation %d)",
                     clientId, tag.c_str(), v);
        Logger::Debug("[AntiCheatManager::InspectPacket] Client %u violation count incremented to %d (maxViolations=%d)",
                      clientId, v, m_maxViolations);
        Logger::Debug("[AntiCheatManager::InspectPacket] Checking if client %u needs to be banned", clientId);
        // Pass the count by value: the ban action below calls into GameServer and
        // must not run under m_violationsMutex (avoids holding the lock across an
        // unbounded call and avoids re-locking inside BanIfNeeded).
        BanIfNeeded(clientId, v);
    } else {
        Logger::Debug("[AntiCheatManager::InspectPacket] Client %u packet tag '%s' is in allowed list, no violation recorded",
                      clientId, tag.c_str());
    }
    // Additional checks: rate limiting, packet size anomalies, speed hacks via position deltas, etc.
    Logger::Debug("[AntiCheatManager::InspectPacket] Additional checks (rate limiting, packet size, speed hacks) placeholder reached for client %u",
                  clientId);
    Logger::Trace("[AntiCheatManager::InspectPacket] Exit");
}

void AntiCheatManager::BanIfNeeded(uint32_t clientId, int violations) {
    Logger::Trace("[AntiCheatManager::BanIfNeeded] Entry - clientId=%u, violations=%d, maxViolations=%d",
                  clientId, violations, m_maxViolations);
    if (violations >= m_maxViolations) {
        Logger::Error("AntiCheat: client %u exceeded max violations, banning", clientId);
        Logger::Debug("[AntiCheatManager::BanIfNeeded] Client %u violations=%d >= maxViolations=%d, proceeding with ban",
                      clientId, violations, m_maxViolations);
        TELEMETRY_INCREMENT_BAN();
        auto conn = m_server ? m_server->GetClientConnection(clientId) : nullptr;
        // Both the connection AND the AdminManager must exist before we can ban:
        // GetAdminManager() can return null during early bring-up / shutdown, and
        // dereferencing it unconditionally would crash on this hot packet path.
        AdminManager* admin = m_server ? m_server->GetAdminManager() : nullptr;
        if (conn && admin) {
            Logger::Debug("[AntiCheatManager::BanIfNeeded] Client %u connection found, retrieving SteamID for ban", clientId);
            admin->BanPlayer(0 /*system*/, conn->GetSteamID(), 60);
            Logger::Info("[AntiCheatManager::BanIfNeeded] Client %u banned for 60 minutes via AdminManager (system-initiated ban)", clientId);
        } else {
            Logger::Error("[AntiCheatManager::BanIfNeeded] Client %u cannot be banned (conn=%p, admin=%p)",
                          clientId, static_cast<void*>(conn.get()), static_cast<void*>(admin));
        }
    } else {
        Logger::Debug("[AntiCheatManager::BanIfNeeded] Client %u violations=%d < maxViolations=%d, no ban needed",
                      clientId, violations, m_maxViolations);
    }
    Logger::Trace("[AntiCheatManager::BanIfNeeded] Exit");
}
