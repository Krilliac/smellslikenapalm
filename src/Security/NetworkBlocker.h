#pragma once

#include <string>
#include <vector>
#include <unordered_set>
#include <mutex>
#include <chrono>
#include <unordered_map>
#include "Network/ClientAddress.h"

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

    // Initialization
    bool Initialize();

    // Block a domain name (e.g., for DNS-level blocking)
    void BlockDomain(const std::string& domain);

private:
    std::vector<std::string> m_blockedDomains;
    struct Entry {
        bool permanent;
        std::chrono::steady_clock::time_point expiresAt;
    };

    std::unordered_map<ClientAddress, Entry> m_blockMap;
    std::mutex                               m_mutex;
};