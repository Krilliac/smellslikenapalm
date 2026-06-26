// src/Time/LatencyManager.cpp
#include "Time/LatencyManager.h"
#include "Utils/Logger.h"

LatencyManager::LatencyManager(size_t maxSamples)
    : m_maxSamples(maxSamples)
{
    Logger::Trace("[LatencyManager::LatencyManager] Constructor entered — maxSamples=%zu", maxSamples);
    Logger::Info("[LatencyManager::LatencyManager] LatencyManager instance created with maxSamples=%zu", maxSamples);
}

LatencyManager::~LatencyManager() {
    Logger::Trace("[LatencyManager::~LatencyManager] Destructor entered");
    Logger::Info("[LatencyManager::~LatencyManager] LatencyManager instance destroyed");
}

uint32_t LatencyManager::SendPing(const ClientAddress& addr) {
    Logger::Trace("[LatencyManager::SendPing] Entry — addr=%s:%u", addr.ip.c_str(), addr.port);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& info = m_data[addr];
    uint32_t pid = info.nextPingId++;
    info.outstanding[pid] = Clock::now();
    Logger::Debug("[LatencyManager::SendPing] Created ping id=%u for addr=%s:%u, total outstanding pings for this addr: %zu",
                  pid, addr.ip.c_str(), addr.port, info.outstanding.size());
    Logger::Info("[LatencyManager::SendPing] Ping sent — id=%u to %s:%u", pid, addr.ip.c_str(), addr.port);
    Logger::Trace("[LatencyManager::SendPing] Exit — returning pingId=%u", pid);
    return pid;
}

void LatencyManager::ReceivePong(const ClientAddress& addr, uint32_t pingId) {
    Logger::Trace("[LatencyManager::ReceivePong] Entry — addr=%s:%u, pingId=%u", addr.ip.c_str(), addr.port, pingId);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_data.find(addr);
    if (it == m_data.end()) {
        Logger::Warn("[LatencyManager::ReceivePong] No data entry found for addr=%s:%u, ignoring pong", addr.ip.c_str(), addr.port);
        Logger::Trace("[LatencyManager::ReceivePong] Exit — early return, unknown address");
        return;
    }
    Info& info = it->second;
    auto oit = info.outstanding.find(pingId);
    if (oit == info.outstanding.end()) {
        Logger::Warn("[LatencyManager::ReceivePong] No outstanding ping found with id=%u for addr=%s:%u (may have expired or been duplicated)",
                     pingId, addr.ip.c_str(), addr.port);
        Logger::Trace("[LatencyManager::ReceivePong] Exit — early return, unknown pingId");
        return;
    }
    auto sentTime = oit->second;
    info.outstanding.erase(oit);
    Logger::Debug("[LatencyManager::ReceivePong] Matched outstanding ping id=%u, erased from outstanding map, remaining outstanding: %zu",
                  pingId, info.outstanding.size());
    auto now = Clock::now();
    auto rtt = std::chrono::duration_cast<std::chrono::milliseconds>(now - sentTime);
    Logger::Debug("[LatencyManager::ReceivePong] RTT computed for ping id=%u: %lld ms", pingId, static_cast<long long>(rtt.count()));
    AddSample(info, rtt);
    Logger::Info("[LatencyManager::ReceivePong] Pong received from %s:%u — pingId=%u, rtt=%lld ms",
                 addr.ip.c_str(), addr.port, pingId, static_cast<long long>(rtt.count()));
    Logger::Trace("[LatencyManager::ReceivePong] Exit");
}

int32_t LatencyManager::GetLatency(const ClientAddress& addr) const {
    Logger::Trace("[LatencyManager::GetLatency] Entry — addr=%s:%u", addr.ip.c_str(), addr.port);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_data.find(addr);
    if (it == m_data.end() || it->second.history.empty()) {
        Logger::Debug("[LatencyManager::GetLatency] No latency data available for addr=%s:%u (data exists=%s, history empty=%s), returning -1",
                      addr.ip.c_str(), addr.port,
                      it != m_data.end() ? "true" : "false",
                      (it != m_data.end() && it->second.history.empty()) ? "true" : "N/A");
        Logger::Trace("[LatencyManager::GetLatency] Exit — returning -1");
        return -1;
    }
    int32_t result = static_cast<int32_t>(it->second.smoothedRtt + 0.5);
    Logger::Debug("[LatencyManager::GetLatency] Smoothed RTT for addr=%s:%u is %.2f, rounded to %d ms",
                  addr.ip.c_str(), addr.port, it->second.smoothedRtt, result);
    Logger::Trace("[LatencyManager::GetLatency] Exit — returning latency=%d ms", result);
    return result;
}

void LatencyManager::Update() {
    Logger::Trace("[LatencyManager::Update] Entry");
    std::lock_guard<std::mutex> lock(m_mutex);
    auto now = Clock::now();
    int totalExpired = 0;
    int totalTrimmed = 0;
    for (auto& kv : m_data) {
        auto& info = kv.second;
        // expire old outstanding pings older than 5s
        size_t outstandingBefore = info.outstanding.size();
        for (auto it = info.outstanding.begin(); it != info.outstanding.end(); ) {
            if (now - it->second > std::chrono::seconds(5)) {
                Logger::Debug("[LatencyManager::Update] Expiring outstanding ping id=%u for addr=%s:%u (older than 5s)",
                              it->first, kv.first.ip.c_str(), kv.first.port);
                it = info.outstanding.erase(it);
                totalExpired++;
            } else {
                ++it;
            }
        }
        if (outstandingBefore != info.outstanding.size()) {
            Logger::Debug("[LatencyManager::Update] Expired %zu outstanding pings for addr=%s:%u, remaining: %zu",
                          outstandingBefore - info.outstanding.size(), kv.first.ip.c_str(), kv.first.port, info.outstanding.size());
        }
        // trim history
        if (info.history.size() > m_maxSamples) {
            size_t toRemove = info.history.size() - m_maxSamples;
            Logger::Debug("[LatencyManager::Update] Trimming history for addr=%s:%u — removing %zu oldest samples (history size=%zu, max=%zu)",
                          kv.first.ip.c_str(), kv.first.port, toRemove, info.history.size(), m_maxSamples);
            info.history.erase(info.history.begin(),
                info.history.begin() + (info.history.size() - m_maxSamples));
            totalTrimmed += static_cast<int>(toRemove);
        }
    }
    Logger::Debug("[LatencyManager::Update] Update complete — expired %d pings, trimmed %d history samples across %zu clients",
                  totalExpired, totalTrimmed, m_data.size());
    Logger::Trace("[LatencyManager::Update] Exit");
}

void LatencyManager::AddSample(Info& info, std::chrono::milliseconds rtt) {
    Logger::Trace("[LatencyManager::AddSample] Entry — rtt=%lld ms, current history size=%zu",
                  static_cast<long long>(rtt.count()), info.history.size());
    // store sample
    info.history.push_back({rtt, Clock::now()});
    Logger::Debug("[LatencyManager::AddSample] Sample added to history, new history size=%zu", info.history.size());
    // update EMA
    if (info.history.size() == 1) {
        info.smoothedRtt = static_cast<double>(rtt.count());
        Logger::Debug("[LatencyManager::AddSample] First sample — smoothedRtt initialized to %lld ms (no EMA blending)",
                      static_cast<long long>(rtt.count()));
    } else {
        double prevSmoothed = info.smoothedRtt;
        info.smoothedRtt = kAlpha * rtt.count() + (1.0 - kAlpha) * info.smoothedRtt;
        Logger::Debug("[LatencyManager::AddSample] EMA update — alpha=%.2f, raw rtt=%lld ms, previous smoothed=%.2f ms, new smoothed=%.2f ms",
                      kAlpha, static_cast<long long>(rtt.count()), prevSmoothed, info.smoothedRtt);
    }
    Logger::Trace("[LatencyManager::AddSample] Exit — smoothedRtt=%.2f ms", info.smoothedRtt);
}
