#include "AntiCheat/AntiCheatManager.h"
#include "Game/GameServer.h"
#include "Utils/Logger.h"

AntiCheatManager::AntiCheatManager(GameServer* server, std::shared_ptr<SecurityConfig> cfg)
    : m_server(server), m_config(cfg) 
{
    m_allowedTags = {
        "CHAT_MESSAGE","PLAYER_ACTION","HEARTBEAT",
        "MOVE","SHOOT","RELOAD"
    };
    m_maxViolations = m_config->GetInt("AntiCheat.MaxViolations", 5);
}

AntiCheatManager::~AntiCheatManager() = default;

bool AntiCheatManager::Initialize() {
    Logger::Info("AntiCheatManager initialized (max violations=%d)", m_maxViolations);
    return true;
}

void AntiCheatManager::OnReceive(uint32_t clientId, const Packet& pkt, const PacketMetadata& meta) {
    InspectPacket(clientId, pkt, true);
}

void AntiCheatManager::OnSend(uint32_t clientId, const Packet& pkt) {
    InspectPacket(clientId, pkt, false);
}

void AntiCheatManager::Update() {
    // Periodically decay violations
    for (auto& kv : m_violations) {
        kv.second = std::max(0, kv.second - 1);
    }
}

void AntiCheatManager::InspectPacket(uint32_t clientId, const Packet& pkt, bool incoming) {
    const auto& tag = pkt.GetTag();
    bool allowed = std::find(m_allowedTags.begin(), m_allowedTags.end(), tag) != m_allowedTags.end();
    if (!allowed) {
        int& v = m_violations[clientId];
        v++;
        Logger::Warn("AntiCheat: client %u sent disallowed packet '%s' (violation %d)",
                     clientId, tag.c_str(), v);
        BanIfNeeded(clientId);
    }
    // Additional checks: rate limiting, packet size anomalies, speed hacks via position deltas, etc.
}

void AntiCheatManager::BanIfNeeded(uint32_t clientId) {
    if (m_violations[clientId] >= m_maxViolations) {
        Logger::Error("AntiCheat: client %u exceeded max violations, banning", clientId);
        m_server->GetAdminManager()->BanPlayer(0 /*system*/, m_server->FindClientBySteamID(std::to_string(clientId)), 60);
    }
}