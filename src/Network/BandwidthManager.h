// src/Network/BandwidthManager.h

#pragma once

#include <cstdint>
#include <cstddef>
#include <chrono>
#include <string>
#include <unordered_map>
#include <mutex>
#include "Network/ClientAddress.h"

class BandwidthManager {
public:
    BandwidthManager(uint32_t maxBytesPerSec);
    ~BandwidthManager();

    // Record that a packet of size bytes was sent to client
    void OnPacketSent(const ClientAddress& addr, uint32_t bytes);

    // Returns true if client may send more data, false if throttled
    bool CanReceivePacket(const ClientAddress& addr, uint32_t bytes);

    // Called each server tick to reset counters
    void Update();

private:
    struct Usage {
        uint32_t bytesSent = 0;
        uint32_t bytesReceived = 0;
    };

    // DoS guard: hard cap on the number of distinct (ip,port) entries tracked.
    // UDP source addresses are unauthenticated and trivially spoofable, so an
    // attacker can otherwise force unbounded growth of m_usageMap (one entry per
    // forged address). Once this many clients are tracked, packets from
    // not-yet-seen addresses are rate-limited using a transient counter instead
    // of inserting a permanent entry (see CanReceivePacket). Sized far above any
    // realistic concurrent player/connection count so legitimate clients are
    // never affected. Idle entries are also pruned each window (see Update).
    static constexpr std::size_t kMaxTrackedClients = 8192;

    uint32_t m_maxBytesPerSec;
    std::chrono::steady_clock::time_point m_windowStart;
    std::unordered_map<ClientAddress, Usage> m_usageMap;
    std::mutex m_mutex;
};