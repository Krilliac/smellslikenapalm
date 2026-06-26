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
    // DoS guard: do not create a new map entry for a never-before-seen address
    // once the tracked-client cap is reached. bytesSent is bookkeeping only (it
    // is not consulted by any throttle decision), so silently not tracking the
    // sent bytes for an over-cap address is harmless and bounds memory growth.
    auto it = m_usageMap.find(addr);
    if (it == m_usageMap.end()) {
        if (m_usageMap.size() >= kMaxTrackedClients) {
            Logger::Warn("[BandwidthManager::OnPacketSent] tracked-client cap %zu reached; not tracking sent bytes for new addr %s:%u",
                         kMaxTrackedClients, addr.ip.c_str(), addr.port);
            Logger::Trace("[BandwidthManager::OnPacketSent] Exit (over cap)");
            return;
        }
        it = m_usageMap.emplace(addr, Usage{}).first;
    }
    Usage& usage = it->second;
    uint32_t previousBytesSent = usage.bytesSent;
    // Saturating add: a uint32 wrap here would corrupt accounting; clamp instead.
    if (bytes > UINT32_MAX - usage.bytesSent) {
        usage.bytesSent = UINT32_MAX;
        Logger::Warn("[BandwidthManager::OnPacketSent] bytesSent saturated at UINT32_MAX for %s:%u (overflow prevented)",
                     addr.ip.c_str(), addr.port);
    } else {
        usage.bytesSent += bytes;
    }
    Logger::Debug("[BandwidthManager::OnPacketSent] Client %s:%u bytesSent updated: %u -> %u",
                  addr.ip.c_str(), addr.port, previousBytesSent, usage.bytesSent);
    Logger::Trace("[BandwidthManager::OnPacketSent] Exit");
}

bool BandwidthManager::CanReceivePacket(const ClientAddress& addr, uint32_t bytes) {
    Logger::Trace("[BandwidthManager::CanReceivePacket] Entry: addr=%s:%u, bytes=%u",
                  addr.ip.c_str(), addr.port, bytes);
    std::lock_guard<std::mutex> lock(m_mutex);
    Logger::Debug("[BandwidthManager::CanReceivePacket] Acquired mutex lock");
    // DoS guard: an unknown address must not unconditionally insert a permanent
    // map entry (UDP source spoofing). When at capacity, evaluate this packet
    // against a transient zero-initialized counter so the throttle verdict for a
    // first-seen address is bit-for-bit identical to the un-capped path, while
    // the map stops growing.
    Usage transientUsage;
    Usage* usagePtr;
    auto it = m_usageMap.find(addr);
    if (it != m_usageMap.end()) {
        usagePtr = &it->second;
    } else if (m_usageMap.size() < kMaxTrackedClients) {
        usagePtr = &m_usageMap[addr];
    } else {
        Logger::Warn("[BandwidthManager::CanReceivePacket] tracked-client cap %zu reached; evaluating new addr %s:%u without tracking",
                     kMaxTrackedClients, addr.ip.c_str(), addr.port);
        usagePtr = &transientUsage;
    }
    Usage& usage = *usagePtr;
    Logger::Debug("[BandwidthManager::CanReceivePacket] Current bytesReceived=%u, requested=%u, limit=%u",
                  usage.bytesReceived, bytes, m_maxBytesPerSec);
    // Overflow-safe form of (bytesReceived + bytes > maxBytesPerSec): computing
    // the sum in uint32 could wrap and falsely pass the check, letting an
    // attacker bypass the cap. The first clause short-circuits before the
    // subtraction can underflow. Verdict is unchanged for all non-wrapping
    // inputs.
    if (usage.bytesReceived > m_maxBytesPerSec ||
        bytes > m_maxBytesPerSec - usage.bytesReceived) {
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
    // Lock for the whole function so m_windowStart is read and written under the
    // same mutex that the send/recv paths take (the previous code read it
    // unlocked, racing the reset below).
    std::lock_guard<std::mutex> lock(m_mutex);
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_windowStart).count();
    Logger::Debug("[BandwidthManager::Update] Elapsed time since window start: %lld seconds", (long long)elapsed);
    // Clock-anomaly guard: steady_clock is monotonic, but if m_windowStart is
    // ever ahead of now (corruption / undefined platform behavior) elapsed goes
    // negative and the window would never expire. Treat that as an expired
    // window and re-anchor so throttle counters cannot get permanently stuck.
    if (elapsed < 0) {
        Logger::Warn("[BandwidthManager::Update] Negative elapsed (%lld s) - clock anomaly; forcing window reset", (long long)elapsed);
        elapsed = 1;
    }
    if (elapsed >= 1) {
        Logger::Debug("[BandwidthManager::Update] Window expired (elapsed=%lld >= 1), resetting usage counters", (long long)elapsed);
        Logger::Debug("[BandwidthManager::Update] Resetting %zu client usage entries", m_usageMap.size());
        // Prune entries that saw no traffic this window and reset the rest. An
        // erased idle address gets a fresh zero-initialized entry on its next
        // packet, so the throttle decision is identical to resetting in place,
        // while churned/stale addresses no longer accumulate in the map.
        for (auto it = m_usageMap.begin(); it != m_usageMap.end(); ) {
            if (it->second.bytesSent == 0 && it->second.bytesReceived == 0) {
                Logger::Trace("[BandwidthManager::Update] Evicting idle client %s:%u",
                              it->first.ip.c_str(), it->first.port);
                it = m_usageMap.erase(it);   // erase returns next valid iterator
            } else {
                Logger::Trace("[BandwidthManager::Update] Resetting client %s:%u: bytesSent=%u->0, bytesReceived=%u->0",
                              it->first.ip.c_str(), it->first.port, it->second.bytesSent, it->second.bytesReceived);
                it->second.bytesSent = 0;
                it->second.bytesReceived = 0;
                ++it;
            }
        }
        m_windowStart = now;
        Logger::Debug("[BandwidthManager::Update] Window start time reset to now");
    } else {
        Logger::Debug("[BandwidthManager::Update] Window still active (elapsed=%lld < 1), no reset needed", (long long)elapsed);
    }
    Logger::Trace("[BandwidthManager::Update] Exit");
}
