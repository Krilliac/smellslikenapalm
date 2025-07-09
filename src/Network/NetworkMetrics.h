// src/Network/NetworkMetrics.h
#pragma once

#include <cstdint>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "Network/ClientConnection.h"

struct ClientMetrics {
    uint32_t packetsSent       = 0;
    uint32_t packetsReceived   = 0;
    uint64_t bytesSent         = 0;
    uint64_t bytesReceived     = 0;
    double   averageLatencyMs  = 0.0;
    double   packetLossPercent = 0.0;
    std::chrono::steady_clock::time_point lastUpdate;
};

class NetworkMetrics {
public:
    NetworkMetrics();
    ~NetworkMetrics();

    // Called by NetworkManager/ConnectionManager
    void OnPacketSent(const ClientAddress& addr, uint32_t bytes);
    void OnPacketReceived(const ClientAddress& addr, uint32_t bytes, double latencyMs, bool dropped);

    // Periodic update (called once per tick)
    void Update();

    // Retrieve metrics
    ClientMetrics GetClientMetrics(const ClientAddress& addr);
    std::vector<ClientMetrics> GetAllClientMetrics();

    // Broadcast summary to admins or logging
    void BroadcastMetricsReport();

private:
    std::mutex m_mutex;
    std::unordered_map<ClientAddress, ClientMetrics> m_clientMap;

    // Per-interval counters
    struct IntervalData {
        uint32_t sentCount     = 0;
        uint32_t recvCount     = 0;
        uint32_t dropCount     = 0;
        double   totalLatency  = 0.0;
    };
    std::unordered_map<ClientAddress, IntervalData> m_intervalMap;

    std::chrono::steady_clock::time_point m_intervalStart;
    std::chrono::seconds m_intervalDuration{5}; // recalc every 5s

    void ResetInterval();
};