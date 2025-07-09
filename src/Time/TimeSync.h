#pragma once

#include <cstdint>
#include <chrono>
#include <unordered_map>
#include <mutex>
#include <functional>

struct ClientAddress {
    std::string ip;
    uint16_t    port;
    bool operator==(ClientAddress const& o) const {
        return ip == o.ip && port == o.port;
    }
};

namespace std {
template<> struct hash<ClientAddress> {
    size_t operator()(ClientAddress const& a) const noexcept {
        return hash<string>()(a.ip) ^ (hash<uint16_t>()(a.port) << 1);
    }
};
}

// Callback when a client’s clock offset is updated:
//   clientId, offset (ms), round‐trip time (ms)
using TimeSyncCallback = std::function<void(uint32_t, int64_t, int64_t)>;

class TimeSync {
public:
    using Clock = std::chrono::steady_clock;
    using Millis = std::chrono::milliseconds;

    explicit TimeSync(TimeSyncCallback cb = nullptr);
    ~TimeSync();

    // Issue a time sync request to a client; returns ping ID
    uint32_t SendTimeSyncRequest(uint32_t clientId,
                                 const ClientAddress& addr,
                                 std::function<void(const ClientAddress&, const std::vector<uint8_t>&)> sender);

    // Handle an incoming sync request from client: respond with server time
    std::vector<uint8_t> HandleRequest(const std::vector<uint8_t>& payload);

    // Process an incoming sync response from client:
    //   payload = [pingId:uint32][clientSendTime:int64][serverRecvTime:int64]
    void HandleResponse(uint32_t clientId,
                        const ClientAddress& addr,
                        const std::vector<uint8_t>& payload);

    // Periodic cleanup of stale outstanding requests
    void Update();

    // Get latest offset (clientTime = serverTime + offset)
    // or 0 if unknown
    int64_t GetOffset(uint32_t clientId) const;

    // Get latest measured RTT for client or -1 if unknown
    int64_t GetRtt(uint32_t clientId) const;

private:
    struct Info {
        uint32_t nextId = 1;
        std::unordered_map<uint32_t, Clock::time_point> outstanding;
        int64_t offset = 0;
        int64_t rtt = -1;
    };

    mutable std::mutex                            m_mutex;
    std::unordered_map<uint32_t, Info>            m_clients;
    TimeSyncCallback                              m_callback;
    Millis                                        m_timeout{Millis(5000)};

    // Helpers to serialize/deserialize 64-bit times
    static void WriteInt64(std::vector<uint8_t>& buf, int64_t v);
    static int64_t ReadInt64(const std::vector<uint8_t>& buf, size_t& off);
};