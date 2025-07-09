// src/Network/LatencyManager.h
#pragma once

#include <cstdint>
#include <chrono>
#include <unordered_map>
#include <mutex>
#include <vector>

struct ClientAddress {
    std::string ip;
    uint16_t    port;
    bool operator==(ClientAddress const& o) const { return ip==o.ip && port==o.port; }
};

namespace std {
    template<> struct hash<ClientAddress> {
        size_t operator()(ClientAddress const& a) const noexcept {
            return hash<string>()(a.ip) ^ (hash<uint16_t>()(a.port)<<1);
        }
    };
}

// Tracks round‐trip latency and packet timing per client
class LatencyManager {
public:
    using Clock = std::chrono::steady_clock;
    struct Sample { 
        std::chrono::milliseconds rtt; 
        Clock::time_point timestamp;
    };

    LatencyManager(size_t maxSamples = 20);
    ~LatencyManager();

    // Called when sending a ping; returns a ping ID
    uint32_t SendPing(const ClientAddress& addr);

    // Called when a ping response arrives with matching ping ID
    void ReceivePong(const ClientAddress& addr, uint32_t pingId);

    // Get the latest smoothed latency for a client (in ms), or -1 if unknown
    int32_t GetLatency(const ClientAddress& addr) const;

    // Periodic maintenance (e.g., expire old samples)
    void Update();

private:
    struct Info {
        uint32_t nextPingId = 1;
        std::unordered_map<uint32_t, Clock::time_point> outstanding;
        std::vector<Sample> history;
        double smoothedRtt = 0.0;
    };

    size_t m_maxSamples;
    mutable std::mutex m_mutex;
    std::unordered_map<ClientAddress, Info> m_data;

    // Simple EMA smoothing factor
    static constexpr double kAlpha = 0.1;

    void AddSample(Info& info, std::chrono::milliseconds rtt);
};