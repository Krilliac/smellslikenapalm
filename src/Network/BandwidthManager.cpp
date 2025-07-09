// src/Network/BandwidthManager.cpp

#include "Network/BandwidthManager.h"
#include "Utils/Logger.h"

BandwidthManager::BandwidthManager(uint32_t maxBytesPerSec)
    : m_maxBytesPerSec(maxBytesPerSec),
      m_windowStart(std::chrono::steady_clock::now())
{
    Logger::Info("BandwidthManager initialized: max %u bytes/sec per client", maxBytesPerSec);
}

BandwidthManager::~BandwidthManager() = default;

void BandwidthManager::OnPacketSent(const ClientAddress& addr, uint32_t bytes) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& usage = m_usageMap[addr];
    usage.bytesSent += bytes;
}

bool BandwidthManager::CanReceivePacket(const ClientAddress& addr, uint32_t bytes) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& usage = m_usageMap[addr];
    if (usage.bytesReceived + bytes > m_maxBytesPerSec) {
        Logger::Warn("Client %s:%u exceeded receive limit (%u/%u)",
                     addr.ip.c_str(), addr.port, usage.bytesReceived, m_maxBytesPerSec);
        return false;
    }
    usage.bytesReceived += bytes;
    return true;
}

void BandwidthManager::Update() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_windowStart).count();
    if (elapsed >= 1) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& kv : m_usageMap) {
            kv.second.bytesSent = 0;
            kv.second.bytesReceived = 0;
        }
        m_windowStart = now;
    }
}