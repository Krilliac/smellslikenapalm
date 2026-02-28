// src/Network/BandwidthManager.cpp

#include "Network/BandwidthManager.h"
#include "Utils/Logger.h"

BandwidthManager::BandwidthManager(uint32_t maxBytesPerSec)
    : m_maxBytesPerSec(maxBytesPerSec),
      m_windowStart(std::chrono::steady_clock::now())
{
    Logger::Trace("[BandwidthManager::BandwidthManager] Entry: maxBytesPerSec=%u", maxBytesPerSec);
    Logger::Info("BandwidthManager initialized: max %u bytes/sec per client", maxBytesPerSec);
    Logger::Debug("[BandwidthManager::BandwidthManager] Window start time initialized to current steady_clock::now()");
    Logger::Trace("[BandwidthManager::BandwidthManager] Exit: constructor complete");
}

BandwidthManager::~BandwidthManager() {
    Logger::Trace("[BandwidthManager::~BandwidthManager] Entry: destructor called");
    Logger::Debug("[BandwidthManager::~BandwidthManager] Destroying BandwidthManager with maxBytesPerSec=%u, usageMap size=%zu",
                  m_maxBytesPerSec, m_usageMap.size());
    Logger::Trace("[BandwidthManager::~BandwidthManager] Exit: destructor complete");
}

void BandwidthManager::OnPacketSent(const ClientAddress& addr, uint32_t bytes) {
    Logger::Trace("[BandwidthManager::OnPacketSent] Entry: addr=%s:%u, bytes=%u",
                  addr.ip.c_str(), addr.port, bytes);
    std::lock_guard<std::mutex> lock(m_mutex);
    Logger::Debug("[BandwidthManager::OnPacketSent] Acquired mutex lock");
    auto& usage = m_usageMap[addr];
    uint32_t previousBytesSent = usage.bytesSent;
    usage.bytesSent += bytes;
    Logger::Debug("[BandwidthManager::OnPacketSent] Client %s:%u bytesSent updated: %u -> %u",
                  addr.ip.c_str(), addr.port, previousBytesSent, usage.bytesSent);
    Logger::Trace("[BandwidthManager::OnPacketSent] Exit");
}

bool BandwidthManager::CanReceivePacket(const ClientAddress& addr, uint32_t bytes) {
    Logger::Trace("[BandwidthManager::CanReceivePacket] Entry: addr=%s:%u, bytes=%u",
                  addr.ip.c_str(), addr.port, bytes);
    std::lock_guard<std::mutex> lock(m_mutex);
    Logger::Debug("[BandwidthManager::CanReceivePacket] Acquired mutex lock");
    auto& usage = m_usageMap[addr];
    Logger::Debug("[BandwidthManager::CanReceivePacket] Current bytesReceived=%u, requested=%u, limit=%u",
                  usage.bytesReceived, bytes, m_maxBytesPerSec);
    if (usage.bytesReceived + bytes > m_maxBytesPerSec) {
        Logger::Warn("Client %s:%u exceeded receive limit (%u/%u)",
                     addr.ip.c_str(), addr.port, usage.bytesReceived, m_maxBytesPerSec);
        Logger::Debug("[BandwidthManager::CanReceivePacket] Rejecting packet: would exceed limit by %u bytes",
                      (usage.bytesReceived + bytes) - m_maxBytesPerSec);
        Logger::Trace("[BandwidthManager::CanReceivePacket] Exit: returning false");
        return false;
    }
    uint32_t previousBytesReceived = usage.bytesReceived;
    usage.bytesReceived += bytes;
    Logger::Debug("[BandwidthManager::CanReceivePacket] Accepting packet: bytesReceived updated %u -> %u (headroom=%u)",
                  previousBytesReceived, usage.bytesReceived, m_maxBytesPerSec - usage.bytesReceived);
    Logger::Trace("[BandwidthManager::CanReceivePacket] Exit: returning true");
    return true;
}

void BandwidthManager::Update() {
    Logger::Trace("[BandwidthManager::Update] Entry");
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_windowStart).count();
    Logger::Debug("[BandwidthManager::Update] Elapsed time since window start: %lld seconds", (long long)elapsed);
    if (elapsed >= 1) {
        Logger::Debug("[BandwidthManager::Update] Window expired (elapsed=%lld >= 1), resetting usage counters", (long long)elapsed);
        std::lock_guard<std::mutex> lock(m_mutex);
        Logger::Debug("[BandwidthManager::Update] Acquired mutex lock, resetting %zu client usage entries", m_usageMap.size());
        for (auto& kv : m_usageMap) {
            Logger::Trace("[BandwidthManager::Update] Resetting client %s:%u: bytesSent=%u->0, bytesReceived=%u->0",
                          kv.first.ip.c_str(), kv.first.port, kv.second.bytesSent, kv.second.bytesReceived);
            kv.second.bytesSent = 0;
            kv.second.bytesReceived = 0;
        }
        m_windowStart = now;
        Logger::Debug("[BandwidthManager::Update] Window start time reset to now");
    } else {
        Logger::Debug("[BandwidthManager::Update] Window still active (elapsed=%lld < 1), no reset needed", (long long)elapsed);
    }
    Logger::Trace("[BandwidthManager::Update] Exit");
}
