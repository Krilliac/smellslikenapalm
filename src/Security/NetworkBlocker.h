#pragma once

#include <string>
#include <unordered_set>
#include <mutex>
#include <chrono>

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

class NetworkBlocker {
public:
    // Duration of a temporary block
    using Duration = std::chrono::seconds;

    NetworkBlocker();
    ~NetworkBlocker();

    // Block an address permanently
    void Block(const ClientAddress& addr);

    // Block an address for a specified duration
    void Block(const ClientAddress& addr, Duration duration);

    // Unblock an address immediately
    void Unblock(const ClientAddress& addr);

    // Check if an address is blocked
    bool IsBlocked(const ClientAddress& addr);

    // Must be called periodically (e.g., once per tick) to expire temporary blocks
    void Update();

private:
    struct Entry {
        bool permanent;
        std::chrono::steady_clock::time_point expiresAt;
    };

    std::unordered_map<ClientAddress, Entry> m_blockMap;
    std::mutex                               m_mutex;
};