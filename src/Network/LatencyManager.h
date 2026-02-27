// src/Network/LatencyManager.h
#pragma once

#include <cstdint>
#include <chrono>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <string>

#include "Time/TimeSync.h"  // for ClientAddress

class LatencyManager {
public:
    using Clock  = std::chrono::steady_clock;
    using Millis = std::chrono::milliseconds;

    explicit LatencyManager(size_t maxSamples = 100);
    ~LatencyManager();

    // Send a ping and record the send time; returns a ping ID
    uint32_t SendPing(const ClientAddress& addr);

    // Record a received pong and compute RTT
    void ReceivePong(const ClientAddress& addr, uint32_t pingId);

    // Get smoothed latency for a client (ms), or -1 if unknown
    int32_t GetLatency(const ClientAddress& addr) const;

    // Periodic cleanup of stale pings / trim history
    void Update();

private:
    struct Sample {
        Millis                    rtt;
        Clock::time_point         timestamp;
    };

    struct Info {
        uint32_t                                    nextPingId = 1;
        std::unordered_map<uint32_t, Clock::time_point> outstanding;
        std::vector<Sample>                         history;
        double                                      smoothedRtt = 0.0;
    };

    void AddSample(Info& info, Millis rtt);

    size_t                                            m_maxSamples;
    mutable std::mutex                                m_mutex;
    std::unordered_map<ClientAddress, Info>            m_data;

    static constexpr double kAlpha = 0.125;  // EMA smoothing factor
};
