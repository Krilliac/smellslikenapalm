#pragma once

#include <memory>
#include <vector>
#include "Config/SecurityConfig.h"
#include "Network/Packet.h"

class GameServer;
struct ReceivedPacket;

class AntiCheatManager {
public:
    explicit AntiCheatManager(GameServer* server, std::shared_ptr<SecurityConfig> cfg);
    ~AntiCheatManager();

    // Initialize anti-cheat subsystems
    bool Initialize();

    // Inspect incoming and outgoing packets
    void OnReceive(uint32_t clientId, const Packet& pkt, const PacketMetadata& meta);
    void OnSend(uint32_t clientId, const Packet& pkt);

    // Periodic update
    void Update();

private:
    GameServer*                  m_server;
    std::shared_ptr<SecurityConfig> m_config;

    // Whitelist of allowed packet tags
    std::vector<std::string>     m_allowedTags;

    // Per-client violation counts
    std::unordered_map<uint32_t, int> m_violations;

    // Thresholds
    int                           m_maxViolations;

    // Helpers
    void InspectPacket(uint32_t clientId, const Packet& pkt, bool incoming);
    void BanIfNeeded(uint32_t clientId);
};