// src/Security/BanManager.cpp
#include "Security/BanManager.h"
#include "Utils/Logger.h"
#include <fstream>
#include <sstream>

BanManager::BanManager(std::shared_ptr<SecurityConfig> config)
    : m_config(config)
{}

BanManager::~BanManager() {
    SaveBans();
}

bool BanManager::LoadBans() {
    std::ifstream file(m_config->GetString("Security.BanListFile", "bans.txt"));
    if (!file.is_open()) {
        Logger::Warn("BanManager: could not open ban list file");
        return false;
    }
    m_bans.clear();
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        BanEntry entry = DeserializeEntry(line);
        m_bans[entry.steamId] = entry;
    }
    Logger::Info("BanManager: loaded %zu entries", m_bans.size());
    return true;
}

bool BanManager::SaveBans() const {
    std::ofstream file(m_config->GetString("Security.BanListFile", "bans.txt"), std::ios::trunc);
    if (!file.is_open()) {
        Logger::Error("BanManager: could not write ban list file");
        return false;
    }
    for (const auto& kv : m_bans) {
        file << SerializeEntry(kv.second) << "\n";
    }
    Logger::Info("BanManager: saved %zu entries", m_bans.size());
    return true;
}

void BanManager::AddBan(const std::string& steamId,
                        BanType type,
                        std::chrono::seconds duration,
                        const std::string& reason)
{
    BanEntry entry;
    entry.steamId = steamId;
    entry.type    = type;
    entry.reason  = reason;
    if (type == BanType::Temporary) {
        entry.expiresAt = std::chrono::steady_clock::now() + duration;
    }
    m_bans[steamId] = entry;
    Logger::Info("BanManager: %s banned (%s)%s",
                 steamId.c_str(),
                 type == BanType::Permanent ? "permanent" : "temporary",
                 reason.empty() ? "" : (" reason=\"" + reason + "\"").c_str());
    SaveBans();
}

bool BanManager::RemoveBan(const std::string& steamId) {
    auto it = m_bans.find(steamId);
    if (it == m_bans.end()) return false;
    m_bans.erase(it);
    Logger::Info("BanManager: %s unbanned", steamId.c_str());
    SaveBans();
    return true;
}

bool BanManager::IsBanned(const std::string& steamId) const {
    auto it = m_bans.find(steamId);
    if (it == m_bans.end()) return false;
    if (it->second.type == BanType::Permanent) return true;
    return std::chrono::steady_clock::now() < it->second.expiresAt;
}

std::chrono::seconds BanManager::GetRemainingBan(const std::string& steamId) const {
    auto it = m_bans.find(steamId);
    if (it == m_bans.end()) return std::chrono::seconds(-1);
    if (it->second.type == BanType::Permanent) return std::chrono::seconds::zero();
    auto rem = std::chrono::duration_cast<std::chrono::seconds>(it->second.expiresAt - std::chrono::steady_clock::now());
    return rem.count() > 0 ? rem : std::chrono::seconds(0);
}

void BanManager::CleanupExpired() {
    auto now = std::chrono::steady_clock::now();
    bool changed = false;
    for (auto it = m_bans.begin(); it != m_bans.end(); ) {
        if (it->second.type == BanType::Temporary && now >= it->second.expiresAt) {
            Logger::Info("BanManager: expired ban removed for %s", it->first.c_str());
            it = m_bans.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed) SaveBans();
}

std::vector<BanEntry> BanManager::GetAllBans() const {
    std::vector<BanEntry> vec;
    vec.reserve(m_bans.size());
    for (const auto& kv : m_bans) {
        vec.push_back(kv.second);
    }
    return vec;
}

std::string BanManager::SerializeEntry(const BanEntry& entry) const {
    std::ostringstream oss;
    oss << entry.steamId << "|"
        << (entry.type == BanType::Permanent ? "P" : "T") << "|";
    if (entry.type == BanType::Temporary) {
        auto expires = std::chrono::duration_cast<std::chrono::seconds>(
            entry.expiresAt.time_since_epoch()).count();
        oss << expires;
    }
    oss << "|"<< entry.reason;
    return oss.str();
}

BanEntry BanManager::DeserializeEntry(const std::string& line) const {
    BanEntry entry;
    std::istringstream iss(line);
    std::string token;
    std::getline(iss, entry.steamId, '|');
    std::getline(iss, token, '|');
    entry.type = (token == "P") ? BanType::Permanent : BanType::Temporary;
    if (entry.type == BanType::Temporary) {
        std::getline(iss, token, '|');
        auto secs = std::stoll(token);
        entry.expiresAt = std::chrono::steady_clock::time_point(std::chrono::seconds(secs));
    } else {
        std::getline(iss, token, '|');
    }
    std::getline(iss, entry.reason);
    return entry;
}