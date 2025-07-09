// src/Security/BanManager.h
#pragma once

#include <string>
#include <unordered_map>
#include <chrono>
#include <memory>
#include "Config/SecurityConfig.h"

enum class BanType {
    Temporary,
    Permanent
};

struct BanEntry {
    std::string     steamId;
    BanType         type;
    std::chrono::steady_clock::time_point expiresAt;  // ignored if permanent
    std::string     reason;
};

class BanManager {
public:
    explicit BanManager(std::shared_ptr<SecurityConfig> config);
    ~BanManager();

    // Load existing bans from persistent storage (e.g., file or database)
    bool LoadBans();

    // Persist current ban list to storage
    bool SaveBans() const;

    // Ban a user by SteamID
    void AddBan(const std::string& steamId,
                BanType type,
                std::chrono::seconds duration = std::chrono::seconds(0),
                const std::string& reason = "");

    // Remove a ban (unban)
    bool RemoveBan(const std::string& steamId);

    // Check if a SteamID is currently banned
    bool IsBanned(const std::string& steamId) const;

    // Get remaining ban time; returns zero for permanent, negative if not banned
    std::chrono::seconds GetRemainingBan(const std::string& steamId) const;

    // Periodic cleanup of expired temporary bans
    void CleanupExpired();

    // Retrieve all ban entries
    std::vector<BanEntry> GetAllBans() const;

private:
    std::shared_ptr<SecurityConfig> m_config;
    std::unordered_map<std::string, BanEntry> m_bans;

    // Helpers for serialization
    std::string SerializeEntry(const BanEntry& entry) const;
    BanEntry    DeserializeEntry(const std::string& line) const;
};