// src/Telemetry/TelemetryBlocker.h
#pragma once

#include <cstdint>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

struct TelemetryClient {
    std::string ip;
    uint16_t    port = 0;

    bool operator==(const TelemetryClient& o) const {
        return ip == o.ip && port == o.port;
    }
};

namespace std {
template<> struct hash<TelemetryClient> {
    size_t operator()(const TelemetryClient& c) const noexcept {
        return hash<string>()(c.ip) ^ (hash<uint16_t>()(c.port) << 1);
    }
};
}

class TelemetryBlocker {
public:
    using Clock    = std::chrono::steady_clock;
    using Duration = std::chrono::milliseconds;

    TelemetryBlocker(size_t maxEventsPerInterval,
                     Duration intervalDuration,
                     Duration blockDuration);
    ~TelemetryBlocker();

    // Returns true if event is allowed, false if blocked
    bool OnEvent(const TelemetryClient& client);

    // Manually block/unblock a client
    void BlockClient(const TelemetryClient& client, Duration duration);
    void UnblockClient(const TelemetryClient& client);

    // Periodic cleanup / expiry
    void Update();

private:
    struct ClientData {
        Clock::time_point windowStart{};
        size_t            eventsCount = 0;
        bool              blocked = false;
        Clock::time_point blockExpires{};
    };

    size_t   m_maxEvents;
    Duration m_interval;
    Duration m_blockDuration;
    std::mutex m_mutex;
    std::unordered_map<TelemetryClient, ClientData> m_clients;
};
