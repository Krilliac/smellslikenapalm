// src/Network/BandwidthManager.h

#pragma once

#include <cstdint>
#include <chrono>
#include <unordered_map>
#include <mutex>

struct ClientAddress {
    std::string ip;
    uint16_t port;
    bool operator==(ClientAddress const& o) const { return ip==o.ip && port==o.port; }
};

namespace std {
template<> struct hash<ClientAddress> {
    std::size_t operator()(ClientAddress const& a) const noexcept {
        return hash<string>()(a.ip) ^ (hash<uint16_t>()(a.port)<<1);
    }
};
}

class BandwidthManager {
public:
    BandwidthManager(uint32_t maxBytesPerSec);
    ~BandwidthManager();

    // Record that a packet of size bytes was sent to client
    void OnPacketSent(const ClientAddress& addr, uint32_t bytes);

    // Returns true if client may send more data, false if throttled
    bool CanReceivePacket(const ClientAddress& addr, uint32_t bytes);

    // Called each server tick to reset counters
    void Update();

private:
    struct Usage {
        uint32_t bytesSent = 0;
        uint32_t bytesReceived = 0;
    };

    uint32_t m_maxBytesPerSec;
    std::chrono::steady_clock::time_point m_windowStart;
    std::unordered_map<ClientAddress, Usage> m_usageMap;
    std::mutex m_mutex;
};