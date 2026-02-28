// src/Telemetry/TelemetryBlocker.cpp
#include "Security/TelemetryBlocker.h"
#include "Utils/Logger.h"

TelemetryBlocker::TelemetryBlocker()
    : m_maxEvents(100)
    , m_interval(Duration(60))
    , m_blockDuration(Duration(300))
{
    Logger::Trace("[TelemetryBlocker::TelemetryBlocker] Default constructor called, maxEvents=%zu, interval=%lld, blockDuration=%lld",
                  m_maxEvents,
                  static_cast<long long>(m_interval.count()),
                  static_cast<long long>(m_blockDuration.count()));
}

TelemetryBlocker::TelemetryBlocker(size_t maxEventsPerInterval,
                                   Duration intervalDuration,
                                   Duration blockDuration)
    : m_maxEvents(maxEventsPerInterval)
    , m_interval(intervalDuration)
    , m_blockDuration(blockDuration)
{
    Logger::Trace("[TelemetryBlocker::TelemetryBlocker] Parameterized constructor called, maxEvents=%zu, interval=%lld, blockDuration=%lld",
                  m_maxEvents,
                  static_cast<long long>(m_interval.count()),
                  static_cast<long long>(m_blockDuration.count()));
}

TelemetryBlocker::~TelemetryBlocker() {
    Logger::Trace("[TelemetryBlocker::~TelemetryBlocker] Destructor called, %zu clients tracked", m_clients.size());
}

bool TelemetryBlocker::OnEvent(const TelemetryClient& client) {
    Logger::Trace("[TelemetryBlocker::OnEvent] Entry, client='%s'", client.id.c_str());
    auto now = Clock::now();
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& data = m_clients[client];

    // Expire any existing block
    if (data.blocked && now >= data.blockExpires) {
        Logger::Debug("[TelemetryBlocker::OnEvent] Block for client '%s' has expired, unblocking and resetting counters",
                      client.id.c_str());
        Logger::Info("[TelemetryBlocker::OnEvent] Expired block lifted for client '%s'", client.id.c_str());
        data.blocked = false;
        data.eventsCount = 0;
        data.windowStart = now;
    }

    if (data.blocked) {
        Logger::Debug("[TelemetryBlocker::OnEvent] Client '%s' is currently blocked, rejecting event (eventsCount=%zu)",
                      client.id.c_str(), data.eventsCount);
        Logger::Trace("[TelemetryBlocker::OnEvent] Exit, returning false (blocked)");
        return false;
    }

    // If window expired, reset counter
    if (now - data.windowStart >= m_interval) {
        Logger::Debug("[TelemetryBlocker::OnEvent] Rate-limit window expired for client '%s', resetting event counter from %zu to 0",
                      client.id.c_str(), data.eventsCount);
        data.windowStart = now;
        data.eventsCount = 0;
    }

    // Increment event count
    ++data.eventsCount;
    Logger::Trace("[TelemetryBlocker::OnEvent] Client '%s' event count incremented to %zu (max=%zu)",
                  client.id.c_str(), data.eventsCount, m_maxEvents);
    if (data.eventsCount > m_maxEvents) {
        // Exceeded rate -> block
        data.blocked = true;
        data.blockExpires = now + m_blockDuration;
        Logger::Warn("[TelemetryBlocker::OnEvent] Client '%s' exceeded rate limit (%zu > %zu events), blocking for %lld seconds",
                     client.id.c_str(), data.eventsCount, m_maxEvents,
                     static_cast<long long>(m_blockDuration.count()));
        Logger::Info("[TelemetryBlocker::OnEvent] Rate-limit block applied to client '%s'", client.id.c_str());
        Logger::Trace("[TelemetryBlocker::OnEvent] Exit, returning false (rate exceeded)");
        return false;
    }

    Logger::Debug("[TelemetryBlocker::OnEvent] Event accepted for client '%s' (%zu/%zu in current window)",
                  client.id.c_str(), data.eventsCount, m_maxEvents);
    Logger::Trace("[TelemetryBlocker::OnEvent] Exit, returning true");
    return true;
}

void TelemetryBlocker::BlockClient(const TelemetryClient& client, Duration duration) {
    Logger::Trace("[TelemetryBlocker::BlockClient] Entry, client='%s', duration=%lld seconds",
                  client.id.c_str(), static_cast<long long>(duration.count()));
    auto now = Clock::now();
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& data = m_clients[client];
    data.blocked = true;
    data.blockExpires = now + duration;
    Logger::Info("[TelemetryBlocker::BlockClient] Manually blocked client '%s' for %lld seconds",
                 client.id.c_str(), static_cast<long long>(duration.count()));
    Logger::Trace("[TelemetryBlocker::BlockClient] Exit");
}

void TelemetryBlocker::UnblockClient(const TelemetryClient& client) {
    Logger::Trace("[TelemetryBlocker::UnblockClient] Entry, client='%s'", client.id.c_str());
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_clients.find(client);
    if (it != m_clients.end()) {
        Logger::Debug("[TelemetryBlocker::UnblockClient] Found client '%s', was blocked=%s, resetting state",
                      client.id.c_str(), it->second.blocked ? "true" : "false");
        it->second.blocked = false;
        it->second.eventsCount = 0;
        it->second.windowStart = Clock::now();
        Logger::Info("[TelemetryBlocker::UnblockClient] Client '%s' unblocked and counters reset", client.id.c_str());
    } else {
        Logger::Debug("[TelemetryBlocker::UnblockClient] Client '%s' not found in tracking map, nothing to unblock",
                      client.id.c_str());
    }
    Logger::Trace("[TelemetryBlocker::UnblockClient] Exit");
}

void TelemetryBlocker::Update() {
    Logger::Trace("[TelemetryBlocker::Update] Entry, tracked clients=%zu", m_clients.size());
    auto now = Clock::now();
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t unblockedCount = 0;
    size_t removedCount = 0;
    for (auto it = m_clients.begin(); it != m_clients.end(); ) {
        auto& data = it->second;
        // Expire block if needed
        if (data.blocked && now >= data.blockExpires) {
            Logger::Debug("[TelemetryBlocker::Update] Block expired for client '%s', unblocking",
                          it->first.id.c_str());
            data.blocked = false;
            data.eventsCount = 0;
            data.windowStart = now;
            ++unblockedCount;
        }
        // Cleanup stale entries (no events and no block for two intervals)
        if (!data.blocked && now - data.windowStart >= m_interval * 2) {
            Logger::Debug("[TelemetryBlocker::Update] Removing stale tracking entry for client '%s' (inactive for 2+ intervals)",
                          it->first.id.c_str());
            it = m_clients.erase(it);
            ++removedCount;
        } else {
            ++it;
        }
    }
    if (unblockedCount > 0) {
        Logger::Info("[TelemetryBlocker::Update] Unblocked %zu clients whose blocks expired", unblockedCount);
    }
    if (removedCount > 0) {
        Logger::Info("[TelemetryBlocker::Update] Removed %zu stale client entries", removedCount);
    }
    Logger::Trace("[TelemetryBlocker::Update] Exit, tracked clients=%zu", m_clients.size());
}

bool TelemetryBlocker::Initialize() {
    Logger::Trace("[TelemetryBlocker::Initialize] Entry");
    std::lock_guard<std::mutex> lock(m_mutex);
    m_clients.clear();
    Logger::Info("[TelemetryBlocker::Initialize] TelemetryBlocker initialized, client tracking map cleared");
    Logger::Debug("[TelemetryBlocker::Initialize] Configuration: maxEvents=%zu, interval=%lld, blockDuration=%lld",
                  m_maxEvents,
                  static_cast<long long>(m_interval.count()),
                  static_cast<long long>(m_blockDuration.count()));
    Logger::Trace("[TelemetryBlocker::Initialize] Exit, returning true");
    return true;
}

void TelemetryBlocker::BlockAllTelemetry() {
    Logger::Trace("[TelemetryBlocker::BlockAllTelemetry] Entry");
    // Block all current clients with a very long duration
    std::lock_guard<std::mutex> lock(m_mutex);
    auto now = Clock::now();
    size_t blockedCount = 0;
    for (auto& [client, data] : m_clients) {
        data.blocked = true;
        data.blockExpires = now + Duration(86400 * 365); // ~1 year
        Logger::Debug("[TelemetryBlocker::BlockAllTelemetry] Blocking client '%s' for ~1 year", client.id.c_str());
        ++blockedCount;
    }
    Logger::Info("[TelemetryBlocker::BlockAllTelemetry] All %zu tracked clients have been blocked for ~1 year", blockedCount);
    Logger::Warn("[TelemetryBlocker::BlockAllTelemetry] All telemetry blocked - this is a global telemetry shutdown");
    Logger::Trace("[TelemetryBlocker::BlockAllTelemetry] Exit");
}

void TelemetryBlocker::BlockCrashReporting() {
    Logger::Trace("[TelemetryBlocker::BlockCrashReporting] Entry");
    // Block the crash reporter client specifically
    TelemetryClient crashClient{"__crash_reporter__"};
    Logger::Debug("[TelemetryBlocker::BlockCrashReporting] Blocking crash reporter client '%s' for ~1 year",
                  crashClient.id.c_str());
    BlockClient(crashClient, Duration(86400 * 365));
    Logger::Info("[TelemetryBlocker::BlockCrashReporting] Crash reporting has been blocked");
    Logger::Warn("[TelemetryBlocker::BlockCrashReporting] Crash reporting disabled - crash data will not be sent");
    Logger::Trace("[TelemetryBlocker::BlockCrashReporting] Exit");
}
