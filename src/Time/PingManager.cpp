#include "Time/PingManager.h"
#include "Utils/Logger.h"

PingManager::PingManager(PingCallback cb)
    : m_callback(std::move(cb))
{
    Logger::Trace("[PingManager::PingManager] Constructor entered — callback %s provided",
                  m_callback ? "was" : "was NOT");
    Logger::Info("[PingManager::PingManager] PingManager instance created");
}

PingManager::~PingManager() {
    Logger::Trace("[PingManager::~PingManager] Destructor entered");
    Logger::Info("[PingManager::~PingManager] PingManager instance destroyed");
}

uint32_t PingManager::SendPing(const ClientAddress& addr,
                               std::function<void(const ClientAddress&, uint32_t)> sender)
{
    Logger::Trace("[PingManager::SendPing] Entry — addr=%s:%u", addr.ip.c_str(), addr.port);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& info = m_clients[addr];
    uint32_t pid = info.nextPingId++;
    info.outstanding[pid] = Clock::now();
    Logger::Debug("[PingManager::SendPing] Created ping id=%u for addr=%s:%u, total outstanding pings for this client: %zu",
                  pid, addr.ip.c_str(), addr.port, info.outstanding.size());
    // Invoke the provided send function to send a ping packet
    Logger::Debug("[PingManager::SendPing] Invoking sender function with addr=%s:%u, pingId=%u", addr.ip.c_str(), addr.port, pid);
    sender(addr, pid);
    Logger::Info("[PingManager::SendPing] Ping sent — id=%u to %s:%u", pid, addr.ip.c_str(), addr.port);
    Logger::Trace("[PingManager::SendPing] Exit — returning pingId=%u", pid);
    return pid;
}

void PingManager::ReceivePong(const ClientAddress& addr, uint32_t pingId) {
    Logger::Trace("[PingManager::ReceivePong] Entry — addr=%s:%u, pingId=%u", addr.ip.c_str(), addr.port, pingId);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_clients.find(addr);
    if (it == m_clients.end()) {
        Logger::Warn("[PingManager::ReceivePong] No client entry found for addr=%s:%u, ignoring pong", addr.ip.c_str(), addr.port);
        Logger::Trace("[PingManager::ReceivePong] Exit — early return, unknown address");
        return;
    }
    auto& info = it->second;
    auto oit = info.outstanding.find(pingId);
    if (oit == info.outstanding.end()) {
        Logger::Warn("[PingManager::ReceivePong] No outstanding ping found with id=%u for addr=%s:%u (may have timed out or been duplicated)",
                     pingId, addr.ip.c_str(), addr.port);
        Logger::Trace("[PingManager::ReceivePong] Exit — early return, unknown pingId");
        return;
    }

    auto now = Clock::now();
    auto rtt  = std::chrono::duration_cast<Millis>(now - oit->second);
    info.lastRtt = rtt;
    info.outstanding.erase(oit);
    Logger::Debug("[PingManager::ReceivePong] Matched outstanding ping id=%u, computed rtt=%lld ms, updated lastRtt, remaining outstanding: %zu",
                  pingId, static_cast<long long>(rtt.count()), info.outstanding.size());

    // Notify via callback
    if (m_callback) {
        Logger::Debug("[PingManager::ReceivePong] Invoking ping callback for addr=%s:%u with rtt=%lld ms",
                      addr.ip.c_str(), addr.port, static_cast<long long>(rtt.count()));
        m_callback(addr, rtt);
    } else {
        Logger::Debug("[PingManager::ReceivePong] No callback registered, skipping notification");
    }
    Logger::Info("[PingManager::ReceivePong] Pong received from %s:%u — pingId=%u, rtt=%lld ms",
                 addr.ip.c_str(), addr.port, pingId, static_cast<long long>(rtt.count()));
    Logger::Trace("[PingManager::ReceivePong] Exit");
}

void PingManager::Update() {
    Logger::Trace("[PingManager::Update] Entry");
    std::lock_guard<std::mutex> lock(m_mutex);
    auto now = Clock::now();
    int totalExpired = 0;
    for (auto& kv : m_clients) {
        auto& info = kv.second;
        // expire outstanding pings
        size_t outstandingBefore = info.outstanding.size();
        for (auto it = info.outstanding.begin(); it != info.outstanding.end(); ) {
            if (now - it->second > m_timeout) {
                Logger::Warn("PingManager: ping %u to %s:%u timed out",
                             it->first, kv.first.ip.c_str(), kv.first.port);
                Logger::Debug("[PingManager::Update] Expiring outstanding ping id=%u for addr=%s:%u (exceeded timeout)",
                              it->first, kv.first.ip.c_str(), kv.first.port);
                it = info.outstanding.erase(it);
                totalExpired++;
            } else {
                ++it;
            }
        }
        if (outstandingBefore != info.outstanding.size()) {
            Logger::Debug("[PingManager::Update] Expired %zu outstanding pings for addr=%s:%u, remaining: %zu",
                          outstandingBefore - info.outstanding.size(), kv.first.ip.c_str(), kv.first.port, info.outstanding.size());
        }
    }
    Logger::Debug("[PingManager::Update] Update complete — expired %d pings across %zu clients", totalExpired, m_clients.size());
    Logger::Trace("[PingManager::Update] Exit");
}

int32_t PingManager::GetLastRtt(const ClientAddress& addr) const {
    Logger::Trace("[PingManager::GetLastRtt] Entry — addr=%s:%u", addr.ip.c_str(), addr.port);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_clients.find(addr);
    if (it == m_clients.end()) {
        Logger::Debug("[PingManager::GetLastRtt] No client entry found for addr=%s:%u, returning -1", addr.ip.c_str(), addr.port);
        Logger::Trace("[PingManager::GetLastRtt] Exit — returning -1");
        return -1;
    }
    int32_t result = static_cast<int32_t>(it->second.lastRtt.count());
    Logger::Debug("[PingManager::GetLastRtt] Last RTT for addr=%s:%u is %d ms", addr.ip.c_str(), addr.port, result);
    Logger::Trace("[PingManager::GetLastRtt] Exit — returning rtt=%d ms", result);
    return result;
}
