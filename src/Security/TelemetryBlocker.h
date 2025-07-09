// src/Telemetry/TelemetryBlocker.h
#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <chrono>

// Represents a client by identifier (e.g., SteamID or clientId)
struct TelemetryClient {
    std::string id;
    bool operator==(TelemetryClient const& o) const {
        return id == o.id;
    }
};

namespace std {
template<> struct hash<TelemetryClient> {
    size_t operator()(TelemetryClient const& c) const noexcept {
        return hash<string>()(c.id);
    }
};
}

// TelemetryBlocker enforces rate‐limits and blocks abusive telemetry senders.
class TelemetryBlocker {
public:
    using Clock = std::chrono::steady_clock;
    using Duration = std::chrono::seconds;

    TelemetryBlocker(size_t maxEventsPerInterval,
                     Duration intervalDuration,
                     Duration blockDuration);
    ~TelemetryBlocker();

    // Called when a client submits a telemetry event.
    // Returns true if allowed, false if client is currently blocked.
    bool OnEvent(const TelemetryClient& client);

    // Manually block a client immediately.
    void BlockClient(const TelemetryClient& client, Duration duration);

    // Unblock a client immediately.
    void UnblockClient(const TelemetryClient& client);

    // Periodic maintenance to reset counters and expire blocks.
    void Update();

private:
    struct ClientData {
        size_t eventsCount = 0;
        Clock::time_point windowStart;
        bool blocked = false;
        Clock::time_point blockExpires;
    };

    size_t m_maxEvents;
    Duration m_interval;
    Duration m_blockDuration;

    std::unordered_map<TelemetryClient, ClientData> m_clients;
    std::mutex m_mutex;
};