#pragma once

#include <cstdint>
#include <chrono>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <functional>

struct ClientAddress {
    std::string ip;
    uint16_t    port;
    bool operator==(ClientAddress const& o) const {
        return ip == o.ip && port == o.port;
    }
};

namespace std {
template<> struct hash<ClientAddress> {
    size_t operator()(ClientAddress const& a) const noexcept {
        return hash<string>()(a.ip) ^ (hash<uint16_t>()(a.port) << 1);
    }
};
}

class PingManager {
public:
    using Clock = std::chrono::steady_clock;
    using Millis = std::chrono::milliseconds;
    struct PingSample {
        uint32_t               id;
        Clock::time_point      sentTime;
    };

    // Callback when a ping round‐trip is measured: (addr, rtt)
    using PingCallback = std::function<void(const ClientAddress&, Millis)>;

    explicit PingManager(PingCallback cb);
    ~PingManager();

    // Send a ping to client; returns ping ID
    uint32_t SendPing(const ClientAddress& addr,
                      std::function<void(const ClientAddress&, uint32_t)> sender);

    // Process a pong from client (addr,pingId)
    void ReceivePong(const ClientAddress& addr, uint32_t pingId);

    // Periodic maintenance: expire old pings and invoke callbacks
    void Update();

    // Get last measured RTT for client (or -1 if unknown)
    int32_t GetLastRtt(const ClientAddress& addr) const;

private:
    struct ClientInfo {
        uint32_t nextPingId = 1;
        std::unordered_map<uint32_t, Clock::time_point> outstanding;
        Millis lastRtt{0};
    };

    mutable std::mutex                     m_mutex;
    std::unordered_map<ClientAddress, ClientInfo> m_clients;
    PingCallback                           m_callback;
    Millis                                 m_timeout{Millis(5000)};  // expire pings older than 5s
};