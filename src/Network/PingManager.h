// src/Network/PingManager.h
#pragma once

#include <cstdint>
#include <chrono>
#include <functional>
#include <unordered_map>
#include <mutex>

#include "Time/TimeSync.h"  // for ClientAddress
#include "Utils/Logger.h"

class PingManager {
public:
    using Clock  = std::chrono::steady_clock;
    using Millis = std::chrono::milliseconds;
    using PingCallback = std::function<void(const ClientAddress&, Millis)>;

    explicit PingManager(PingCallback cb = nullptr);
    ~PingManager();

    // Send a ping; the provided sender function does the actual network send
    uint32_t SendPing(const ClientAddress& addr,
                      std::function<void(const ClientAddress&, uint32_t)> sender);

    // Process an incoming pong
    void ReceivePong(const ClientAddress& addr, uint32_t pingId);

    // Periodic cleanup of stale outstanding pings
    void Update();

    // Get last round-trip time (ms) or -1 if unknown
    int32_t GetLastRtt(const ClientAddress& addr) const;

private:
    struct ClientInfo {
        uint32_t                                         nextPingId = 1;
        std::unordered_map<uint32_t, Clock::time_point>  outstanding;
        Millis                                           lastRtt{0};
    };

    PingCallback                                         m_callback;
    mutable std::mutex                                   m_mutex;
    std::unordered_map<ClientAddress, ClientInfo>         m_clients;
    Millis                                               m_timeout{Millis(5000)};
};
