// src/Network/LatencyManager.cpp
#include "Network/LatencyManager.h"
#include "Utils/Logger.h"

LatencyManager::LatencyManager(size_t maxSamples)
    : m_maxSamples(maxSamples)
{}

LatencyManager::~LatencyManager() = default;

uint32_t LatencyManager::SendPing(const ClientAddress& addr) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& info = m_data[addr];
    uint32_t pid = info.nextPingId++;
    info.outstanding[pid] = Clock::now();
    return pid;
}

void LatencyManager::ReceivePong(const ClientAddress& addr, uint32_t pingId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_data.find(addr);
    if (it == m_data.end()) return;
    Info& info = it->second;
    auto oit = info.outstanding.find(pingId);
    if (oit == info.outstanding.end()) return;
    auto sentTime = oit->second;
    info.outstanding.erase(oit);
    auto now = Clock::now();
    auto rtt = std::chrono::duration_cast<std::chrono::milliseconds>(now - sentTime);
    AddSample(info, rtt);
}

int32_t LatencyManager::GetLatency(const ClientAddress& addr) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_data.find(addr);
    if (it == m_data.end() || it->second.history.empty()) return -1;
    return static_cast<int32_t>(it->second.smoothedRtt + 0.5);
}

void LatencyManager::Update() {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto now = Clock::now();
    for (auto& kv : m_data) {
        auto& info = kv.second;
        // expire old outstanding pings older than 5s
        for (auto it = info.outstanding.begin(); it != info.outstanding.end(); ) {
            if (now - it->second > std::chrono::seconds(5)) {
                it = info.outstanding.erase(it);
            } else {
                ++it;
            }
        }
        // trim history
        if (info.history.size() > m_maxSamples) {
            info.history.erase(info.history.begin(), 
                info.history.begin() + (info.history.size() - m_maxSamples));
        }
    }
}

void LatencyManager::AddSample(Info& info, std::chrono::milliseconds rtt) {
    // store sample
    info.history.push_back({rtt, Clock::now()});
    // update EMA
    if (info.history.size() == 1) {
        info.smoothedRtt = rtt.count();
    } else {
        info.smoothedRtt = kAlpha * rtt.count() + (1.0 - kAlpha) * info.smoothedRtt;
    }
}