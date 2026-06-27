#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
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

    // Per-client violation counts. Guarded by m_violationsMutex: OnReceive/OnSend
    // run on network threads while Update() runs on the main loop, so concurrent
    // access to this map is a data race without synchronization.
    mutable std::mutex                m_violationsMutex;
    std::unordered_map<uint32_t, int> m_violations;

    // Thresholds
    int                           m_maxViolations;

    // Helpers
    void InspectPacket(uint32_t clientId, const Packet& pkt, bool incoming);
    // `violations` is the already-read count for clientId, passed in so this never
    // re-locks m_violationsMutex (avoids recursive-lock deadlock from InspectPacket).
    void BanIfNeeded(uint32_t clientId, int violations);
};