// src/Network/NetworkMetrics.cpp
#include "Network/NetworkMetrics.h"
#include "Utils/Logger.h"
#include "Network/NetworkManager.h"
#include <sstream>

NetworkMetrics::NetworkMetrics()
    : m_intervalStart(std::chrono::steady_clock::now())
{
    Logger::Trace("[NetworkMetrics::NetworkMetrics] Entry: default constructor");
    Logger::Info("NetworkMetrics initialized (interval=%lds)", m_intervalDuration.count());
    Logger::Debug("[NetworkMetrics::NetworkMetrics] Interval start set to current time, duration=%ld seconds",
                  m_intervalDuration.count());
    Logger::Trace("[NetworkMetrics::NetworkMetrics] Exit");
}

NetworkMetrics::~NetworkMetrics() {
    Logger::Trace("[NetworkMetrics::~NetworkMetrics] Entry: destructor called");
    Logger::Debug("[NetworkMetrics::~NetworkMetrics] Destroying NetworkMetrics, clientMap size=%zu, intervalMap size=%zu",
                  m_clientMap.size(), m_intervalMap.size());
    Logger::Trace("[NetworkMetrics::~NetworkMetrics] Exit");
}

void NetworkMetrics::OnPacketSent(const ClientAddress& addr, uint32_t bytes) {
    Logger::Trace("[NetworkMetrics::OnPacketSent] Entry: addr=%s:%u, bytes=%u",
                  addr.ip.c_str(), addr.port, bytes);
    std::lock_guard<std::mutex> lock(m_mutex);
    Logger::Debug("[NetworkMetrics::OnPacketSent] Acquired mutex lock");
    auto& m = m_clientMap[addr];
    uint32_t prevPacketsSent = m.packetsSent;
    uint64_t prevBytesSent = m.bytesSent;
    m.packetsSent++;
    m.bytesSent += bytes;
    Logger::Debug("[NetworkMetrics::OnPacketSent] Client %s:%u packetsSent: %u -> %u, bytesSent: %llu -> %llu",
                  addr.ip.c_str(), addr.port, prevPacketsSent, m.packetsSent,
                  (unsigned long long)prevBytesSent, (unsigned long long)m.bytesSent);
    m_intervalMap[addr].sentCount++;
    Logger::Debug("[NetworkMetrics::OnPacketSent] Interval sentCount for %s:%u incremented to %u",
                  addr.ip.c_str(), addr.port, m_intervalMap[addr].sentCount);
    Logger::Trace("[NetworkMetrics::OnPacketSent] Exit");
}

void NetworkMetrics::OnPacketReceived(const ClientAddress& addr, uint32_t bytes, double latencyMs, bool dropped) {
    Logger::Trace("[NetworkMetrics::OnPacketReceived] Entry: addr=%s:%u, bytes=%u, latencyMs=%.3f, dropped=%s",
                  addr.ip.c_str(), addr.port, bytes, latencyMs, dropped ? "true" : "false");
    std::lock_guard<std::mutex> lock(m_mutex);
    Logger::Debug("[NetworkMetrics::OnPacketReceived] Acquired mutex lock");
    auto& m = m_clientMap[addr];
    uint32_t prevPacketsRecv = m.packetsReceived;
    m.packetsReceived++;
    m.bytesReceived += bytes;
    // update running average latency
    double prevLatency = m.averageLatencyMs;
    m.averageLatencyMs = ((m.averageLatencyMs * (m.packetsReceived - 1)) + latencyMs) / m.packetsReceived;
    Logger::Debug("[NetworkMetrics::OnPacketReceived] Client %s:%u packetsReceived: %u -> %u, bytesReceived updated, avgLatency: %.3f -> %.3f ms",
                  addr.ip.c_str(), addr.port, prevPacketsRecv, m.packetsReceived, prevLatency, m.averageLatencyMs);
    auto& iv = m_intervalMap[addr];
    iv.recvCount++;
    iv.totalLatency += latencyMs;
    if (dropped) {
        iv.dropCount++;
        Logger::Debug("[NetworkMetrics::OnPacketReceived] Packet marked as dropped for %s:%u, interval dropCount=%u",
                      addr.ip.c_str(), addr.port, iv.dropCount);
    }
    Logger::Debug("[NetworkMetrics::OnPacketReceived] Interval stats for %s:%u: recvCount=%u, totalLatency=%.3f, dropCount=%u",
                  addr.ip.c_str(), addr.port, iv.recvCount, iv.totalLatency, iv.dropCount);
    Logger::Trace("[NetworkMetrics::OnPacketReceived] Exit");
}

void NetworkMetrics::Update() {
    Logger::Trace("[NetworkMetrics::Update] Entry");
    auto now = std::chrono::steady_clock::now();
    if (now - m_intervalStart < m_intervalDuration) {
        Logger::Debug("[NetworkMetrics::Update] Interval not yet elapsed, skipping update");
        Logger::Trace("[NetworkMetrics::Update] Exit: interval not elapsed");
        return;
    }
    Logger::Debug("[NetworkMetrics::Update] Interval elapsed, computing per-client metrics");
    std::lock_guard<std::mutex> lock(m_mutex);
    Logger::Debug("[NetworkMetrics::Update] Acquired mutex lock, processing %zu interval entries", m_intervalMap.size());

    // Compute packet loss % per client
    for (auto& kv : m_intervalMap) {
        const auto& addr = kv.first;
        auto& iv = kv.second;
        auto& cm = m_clientMap[addr];
        uint32_t total = iv.recvCount + iv.dropCount;
        double prevLoss = cm.packetLossPercent;
        cm.packetLossPercent = total ? (100.0 * iv.dropCount / total) : 0.0;
        Logger::Debug("[NetworkMetrics::Update] Client %s:%u: recvCount=%u, dropCount=%u, total=%u, packetLoss: %.2f%% -> %.2f%%",
                      addr.ip.c_str(), addr.port, iv.recvCount, iv.dropCount, total, prevLoss, cm.packetLossPercent);
        // Optional: update latency over interval
        if (iv.recvCount) {
            double avg = iv.totalLatency / iv.recvCount;
            double prevAvg = cm.averageLatencyMs;
            cm.averageLatencyMs = avg;
            Logger::Debug("[NetworkMetrics::Update] Client %s:%u: interval avgLatency: %.3f -> %.3f ms",
                          addr.ip.c_str(), addr.port, prevAvg, cm.averageLatencyMs);
        } else {
            Logger::Debug("[NetworkMetrics::Update] Client %s:%u: no packets received this interval, latency unchanged",
                          addr.ip.c_str(), addr.port);
        }
    }

    ResetInterval();
    m_intervalStart = now;
    Logger::Info("[NetworkMetrics::Update] Metrics interval reset, all interval counters cleared");
    Logger::Trace("[NetworkMetrics::Update] Exit");
}

ClientMetrics NetworkMetrics::GetClientMetrics(const ClientAddress& addr) {
    Logger::Trace("[NetworkMetrics::GetClientMetrics] Entry: addr=%s:%u", addr.ip.c_str(), addr.port);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_clientMap.find(addr);
    if (it != m_clientMap.end()) {
        Logger::Debug("[NetworkMetrics::GetClientMetrics] Found metrics for %s:%u: sent=%u, recv=%u, loss=%.2f%%, latency=%.3fms",
                      addr.ip.c_str(), addr.port, it->second.packetsSent, it->second.packetsReceived,
                      it->second.packetLossPercent, it->second.averageLatencyMs);
        Logger::Trace("[NetworkMetrics::GetClientMetrics] Exit: returning valid metrics");
        return it->second;
    }
    Logger::Debug("[NetworkMetrics::GetClientMetrics] No metrics found for %s:%u, returning default ClientMetrics",
                  addr.ip.c_str(), addr.port);
    Logger::Trace("[NetworkMetrics::GetClientMetrics] Exit: returning empty metrics");
    return ClientMetrics{};
}

std::vector<ClientMetrics> NetworkMetrics::GetAllClientMetrics() {
    Logger::Trace("[NetworkMetrics::GetAllClientMetrics] Entry");
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<ClientMetrics> vec;
    for (auto& kv : m_clientMap) {
        vec.push_back(kv.second);
    }
    Logger::Debug("[NetworkMetrics::GetAllClientMetrics] Collected metrics for %zu clients", vec.size());
    Logger::Trace("[NetworkMetrics::GetAllClientMetrics] Exit: returning %zu entries", vec.size());
    return vec;
}

void NetworkMetrics::BroadcastMetricsReport() {
    Logger::Trace("[NetworkMetrics::BroadcastMetricsReport] Entry");
    std::lock_guard<std::mutex> lock(m_mutex);
    Logger::Debug("[NetworkMetrics::BroadcastMetricsReport] Building metrics report for %zu clients", m_clientMap.size());
    std::ostringstream oss;
    oss << "--- Network Metrics Report ---\n";
    for (auto& kv : m_clientMap) {
        const auto& addr = kv.first;
        const auto& m = kv.second;
        oss << addr.ip << ":" << addr.port
            << " Sent=" << m.packetsSent << " Recv=" << m.packetsReceived
            << " Loss=" << m.packetLossPercent << "%"
            << " Latency=" << m.averageLatencyMs << "ms\n";
        Logger::Trace("[NetworkMetrics::BroadcastMetricsReport] Client %s:%u: Sent=%u, Recv=%u, Loss=%.2f%%, Latency=%.3fms",
                      addr.ip.c_str(), addr.port, m.packetsSent, m.packetsReceived, m.packetLossPercent, m.averageLatencyMs);
    }
    oss << "------------------------------";
    Logger::Info("%s", oss.str().c_str());
    Logger::Trace("[NetworkMetrics::BroadcastMetricsReport] Exit");
}

void NetworkMetrics::ResetInterval() {
    Logger::Trace("[NetworkMetrics::ResetInterval] Entry: intervalMap size=%zu", m_intervalMap.size());
    m_intervalMap.clear();
    Logger::Debug("[NetworkMetrics::ResetInterval] Interval map cleared");
    Logger::Trace("[NetworkMetrics::ResetInterval] Exit");
}
