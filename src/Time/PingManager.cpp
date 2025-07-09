#include "Network/PingManager.h"
#include "Utils/Logger.h"

PingManager::PingManager(PingCallback cb)
    : m_callback(std::move(cb))
{}

PingManager::~PingManager() = default;

uint32_t PingManager::SendPing(const ClientAddress& addr,
                               std::function<void(const ClientAddress&, uint32_t)> sender)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& info = m_clients[addr];
    uint32_t pid = info.nextPingId++;
    info.outstanding[pid] = Clock::now();
    // Invoke the provided send function to send a ping packet
    sender(addr, pid);
    return pid;
}

void PingManager::ReceivePong(const ClientAddress& addr, uint32_t pingId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_clients.find(addr);
    if (it == m_clients.end()) return;
    auto& info = it->second;
    auto oit = info.outstanding.find(pingId);
    if (oit == info.outstanding.end()) return;

    auto now = Clock::now();
    auto rtt  = std::chrono::duration_cast<Millis>(now - oit->second);
    info.lastRtt = rtt;
    info.outstanding.erase(oit);

    // Notify via callback
    if (m_callback) {
        m_callback(addr, rtt);
    }
}

void PingManager::Update() {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto now = Clock::now();
    for (auto& kv : m_clients) {
        auto& info = kv.second;
        // expire outstanding pings
        for (auto it = info.outstanding.begin(); it != info.outstanding.end(); ) {
            if (now - it->second > m_timeout) {
                Logger::Warn("PingManager: ping %u to %s:%u timed out",
                             it->first, kv.first.ip.c_str(), kv.first.port);
                it = info.outstanding.erase(it);
            } else {
                ++it;
            }
        }
    }
}

int32_t PingManager::GetLastRtt(const ClientAddress& addr) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_clients.find(addr);
    if (it == m_clients.end()) return -1;
    return static_cast<int32_t>(it->second.lastRtt.count());
}