// src/Network/NetworkMetrics.cpp
#include "Network/NetworkMetrics.h"
#include "Utils/Logger.h"
#include "Network/NetworkManager.h"
#include <sstream>

NetworkMetrics::NetworkMetrics()
    : m_intervalStart(std::chrono::steady_clock::now())
{
    Logger::Info("NetworkMetrics initialized (interval=%lds)", m_intervalDuration.count());
}

NetworkMetrics::~NetworkMetrics() = default;

void NetworkMetrics::OnPacketSent(const ClientAddress& addr, uint32_t bytes) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& m = m_clientMap[addr];
    m.packetsSent++;
    m.bytesSent += bytes;
    m_intervalMap[addr].sentCount++;
}

void NetworkMetrics::OnPacketReceived(const ClientAddress& addr, uint32_t bytes, double latencyMs, bool dropped) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& m = m_clientMap[addr];
    m.packetsReceived++;
    m.bytesReceived += bytes;
    // update running average latency
    m.averageLatencyMs = ((m.averageLatencyMs * (m.packetsReceived - 1)) + latencyMs) / m.packetsReceived;
    auto& iv = m_intervalMap[addr];
    iv.recvCount++;
    iv.totalLatency += latencyMs;
    if (dropped) iv.dropCount++;
}

void NetworkMetrics::Update() {
    auto now = std::chrono::steady_clock::now();
    if (now - m_intervalStart < m_intervalDuration) return;
    std::lock_guard<std::mutex> lock(m_mutex);

    // Compute packet loss % per client
    for (auto& kv : m_intervalMap) {
        const auto& addr = kv.first;
        auto& iv = kv.second;
        auto& cm = m_clientMap[addr];
        uint32_t total = iv.recvCount + iv.dropCount;
        cm.packetLossPercent = total ? (100.0 * iv.dropCount / total) : 0.0;
        // Optional: update latency over interval
        if (iv.recvCount) {
            double avg = iv.totalLatency / iv.recvCount;
            cm.averageLatencyMs = avg;
        }
    }

    ResetInterval();
    m_intervalStart = now;
}

ClientMetrics NetworkMetrics::GetClientMetrics(const ClientAddress& addr) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_clientMap.find(addr);
    return it != m_clientMap.end() ? it->second : ClientMetrics{};
}

std::vector<ClientMetrics> NetworkMetrics::GetAllClientMetrics() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<ClientMetrics> vec;
    for (auto& kv : m_clientMap) {
        vec.push_back(kv.second);
    }
    return vec;
}

void NetworkMetrics::BroadcastMetricsReport() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::ostringstream oss;
    oss << "--- Network Metrics Report ---\n";
    for (auto& kv : m_clientMap) {
        const auto& addr = kv.first;
        const auto& m = kv.second;
        oss << addr.ip << ":" << addr.port
            << " Sent=" << m.packetsSent << " Recv=" << m.packetsReceived
            << " Loss=" << m.packetLossPercent << "%"
            << " Latency=" << m.averageLatencyMs << "ms\n";
    }
    oss << "------------------------------";
    Logger::Info("%s", oss.str().c_str());
}

void NetworkMetrics::ResetInterval() {
    m_intervalMap.clear();
}