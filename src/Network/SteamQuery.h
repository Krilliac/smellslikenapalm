// src/Network/SteamQuery.h
#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "Math/Vector3.h"

struct SteamServerInfo {
    std::string serverName;
    std::string mapName;
    std::string gameMode;
    uint32_t    currentPlayers;
    uint32_t    maxPlayers;
    uint16_t    pingMs;
};

class SteamQuery {
public:
    SteamQuery(const std::string& masterServerIp = "hl2master.steampowered.com", uint16_t masterServerPort = 27011);
    ~SteamQuery();

    // Query the master server for list of RS2V servers
    std::vector<std::pair<std::string,uint16_t>> QueryMaster();

    // Query a single game server for its info
    bool QueryServer(const std::string& ip, uint16_t port, SteamServerInfo& outInfo);

private:
    std::string m_masterIp;
    uint16_t    m_masterPort;

    // Internal helpers for UDP socket I/O
    bool SendAndReceive(const std::vector<uint8_t>& req,
                        std::vector<uint8_t>& resp,
                        const std::string& ip,
                        uint16_t port,
                        uint32_t timeoutMs = 500);

    // Build A2S_INFO request
    std::vector<uint8_t> BuildInfoRequest();

    // Parse A2S_INFO response
    bool ParseInfoResponse(const std::vector<uint8_t>& buf, SteamServerInfo& info);
};