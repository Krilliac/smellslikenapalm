// src/Telemetry/TelemetryBlocker.cpp
#include "Telemetry/TelemetryBlocker.h"

TelemetryBlocker::TelemetryBlocker(size_t maxEventsPerInterval,
                                   Duration intervalDuration,
                                   Duration blockDuration)
    : m_maxEvents(maxEventsPerInterval)
    , m_interval(intervalDuration)
    , m_blockDuration(blockDuration)
{}

TelemetryBlocker::~TelemetryBlocker() = default;

bool TelemetryBlocker::OnEvent(const TelemetryClient& client) {
    auto now = Clock::now();
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& data = m_clients[client];

    // Expire any existing block
    if (data.blocked && now >= data.blockExpires) {
        data.blocked = false;
        data.eventsCount = 0;
        data.windowStart = now;
    }

    if (data.blocked) {
        return false;
    }

    // If window expired, reset counter
    if (now - data.windowStart >= m_interval) {
        data.windowStart = now;
        data.eventsCount = 0;
    }

    // Increment event count
    ++data.eventsCount;
    if (data.eventsCount > m_maxEvents) {
        // Exceeded rate → block
        data.blocked = true;
        data.blockExpires = now + m_blockDuration;
        return false;
    }

    return true;
}

void TelemetryBlocker::BlockClient(const TelemetryClient& client, Duration duration) {
    auto now = Clock::now();
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& data = m_clients[client];
    data.blocked = true;
    data.blockExpires = now + duration;
}

void TelemetryBlocker::UnblockClient(const TelemetryClient& client) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_clients.find(client);
    if (it != m_clients.end()) {
        it->second.blocked = false;
        it->second.eventsCount = 0;
        it->second.windowStart = Clock::now();
    }
}

void TelemetryBlocker::Update() {
    auto now = Clock::now();
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_clients.begin(); it != m_clients.end(); ) {
        auto& data = it->second;
        // Expire block if needed
        if (data.blocked && now >= data.blockExpires) {
            data.blocked = false;
            data.eventsCount = 0;
            data.windowStart = now;
        }
        // Cleanup stale entries (no events and no block for two intervals)
        if (!data.blocked && now - data.windowStart >= m_interval * 2) {
            it = m_clients.erase(it);
        } else {
            ++it;
        }
    }
}