// src/Security/BanManager.cpp
#include "Security/BanManager.h"
#include "Utils/Logger.h"
#include <fstream>
#include <sstream>

BanManager::BanManager(std::shared_ptr<SecurityConfig> config)
    : m_config(config)
{
    Logger::Trace("[BanManager::BanManager] Constructor called, config is %s",
                  config ? "non-null" : "null");
}

BanManager::~BanManager() {
    Logger::Trace("[BanManager::~BanManager] Destructor called, %zu bans in memory", m_bans.size());
    SaveBans();
    Logger::Trace("[BanManager::~BanManager] Destructor completed");
}

bool BanManager::LoadBans() {
    Logger::Trace("[BanManager::LoadBans] Entry");
    std::string banFilePath = m_config->GetBanListFile();
    Logger::Debug("[BanManager::LoadBans] Attempting to open ban list file: '%s'", banFilePath.c_str());
    std::ifstream file(banFilePath);
    if (!file.is_open()) {
        Logger::Warn("BanManager: could not open ban list file");
        Logger::Error("[BanManager::LoadBans] Failed to open ban list file '%s' - file may not exist or access denied",
                      banFilePath.c_str());
        Logger::Trace("[BanManager::LoadBans] Exit, returning false");
        return false;
    }
    Logger::Debug("[BanManager::LoadBans] Ban list file opened successfully");
    m_bans.clear();
    Logger::Debug("[BanManager::LoadBans] Cleared existing bans map before loading");
    std::string line;
    size_t lineNumber = 0;
    while (std::getline(file, line)) {
        ++lineNumber;
        // Skip blank lines and '#' comment lines (the shipped ban_list.txt has a
        // comment banner). Also skip lines with no '|' field separator - they are
        // not serialized ban entries and must not be parsed as such.
        const size_t firstNonWs = line.find_first_not_of(" \t\r\n");
        if (firstNonWs == std::string::npos) {
            Logger::Trace("[BanManager::LoadBans] Skipping blank line %zu", lineNumber);
            continue;
        }
        if (line[firstNonWs] == '#') {
            Logger::Trace("[BanManager::LoadBans] Skipping comment line %zu", lineNumber);
            continue;
        }
        if (line.find('|') == std::string::npos) {
            Logger::Warn("[BanManager::LoadBans] Skipping malformed (no '|') line %zu: '%s'",
                         lineNumber, line.c_str());
            continue;
        }
        Logger::Trace("[BanManager::LoadBans] Parsing line %zu: '%s'", lineNumber, line.c_str());
        BanEntry entry = DeserializeEntry(line);
        if (entry.steamId.empty()) {
            Logger::Warn("[BanManager::LoadBans] Skipping line %zu: empty SteamID", lineNumber);
            continue;
        }
        m_bans[entry.steamId] = entry;
        Logger::Debug("[BanManager::LoadBans] Loaded ban entry for SteamID '%s', type=%s, reason='%s'",
                      entry.steamId.c_str(),
                      entry.type == BanType::Permanent ? "Permanent" : "Temporary",
                      entry.reason.c_str());
    }
    Logger::Info("BanManager: loaded %zu entries", m_bans.size());
    Logger::Info("[BanManager::LoadBans] Successfully loaded all ban entries from '%s'", banFilePath.c_str());
    Logger::Trace("[BanManager::LoadBans] Exit, returning true");
    return true;
}

bool BanManager::SaveBans() const {
    Logger::Trace("[BanManager::SaveBans] Entry, %zu bans to save", m_bans.size());
    std::string banFilePath = m_config->GetBanListFile();
    Logger::Debug("[BanManager::SaveBans] Writing ban list to file: '%s'", banFilePath.c_str());
    std::ofstream file(banFilePath, std::ios::trunc);
    if (!file.is_open()) {
        Logger::Error("BanManager: could not write ban list file");
        Logger::Error("[BanManager::SaveBans] Failed to open ban list file '%s' for writing", banFilePath.c_str());
        Logger::Trace("[BanManager::SaveBans] Exit, returning false");
        return false;
    }
    Logger::Debug("[BanManager::SaveBans] Ban list file opened for writing");
    // Self-documenting header so the machine-managed file stays readable. Loader
    // skips '#'/blank lines, so this round-trips harmlessly.
    file << "# RS2V ban list (managed by the server — edits may be overwritten)\n"
            "# Format: SteamID|P|<unused>|reason            (permanent)\n"
            "#         SteamID|T|<expiryUnixSeconds>|reason  (temporary)\n";
    size_t count = 0;
    for (const auto& kv : m_bans) {
        std::string serialized = SerializeEntry(kv.second);
        file << serialized << "\n";
        Logger::Trace("[BanManager::SaveBans] Wrote entry %zu for SteamID '%s': '%s'",
                      count, kv.first.c_str(), serialized.c_str());
        ++count;
    }
    Logger::Info("BanManager: saved %zu entries", m_bans.size());
    Logger::Info("[BanManager::SaveBans] Successfully saved all %zu ban entries to '%s'", count, banFilePath.c_str());
    Logger::Trace("[BanManager::SaveBans] Exit, returning true");
    return true;
}

void BanManager::AddBan(const std::string& steamId,
                        BanType type,
                        std::chrono::seconds duration,
                        const std::string& reason)
{
    Logger::Trace("[BanManager::AddBan] Entry, steamId='%s', type=%s, duration=%lld seconds, reason='%s'",
                  steamId.c_str(),
                  type == BanType::Permanent ? "Permanent" : "Temporary",
                  static_cast<long long>(duration.count()),
                  reason.c_str());

    // Validate the identity key before it becomes a persistent map key. An empty
    // or absurdly long SteamID is never legitimate; storing it would pollute the
    // ban table (and an empty key would later match an empty GetSteamID()).
    constexpr size_t kMaxSteamIdLen = 128;
    if (steamId.empty() || steamId.size() > kMaxSteamIdLen) {
        Logger::Warn("[BanManager::AddBan] Rejecting ban with invalid SteamID (empty=%s, len=%zu)",
                     steamId.empty() ? "true" : "false", steamId.size());
        return;
    }

    BanEntry entry;
    entry.steamId = steamId;
    entry.type    = type;
    entry.reason  = reason;
    if (type == BanType::Temporary) {
        entry.expiresAt = std::chrono::system_clock::now() + duration;
        Logger::Debug("[BanManager::AddBan] Temporary ban for '%s' set to expire after %lld seconds",
                      steamId.c_str(), static_cast<long long>(duration.count()));
    } else {
        Logger::Debug("[BanManager::AddBan] Permanent ban for '%s' - no expiry", steamId.c_str());
    }
    m_bans[steamId] = entry;
    Logger::Info("BanManager: %s banned (%s)%s",
                 steamId.c_str(),
                 type == BanType::Permanent ? "permanent" : "temporary",
                 reason.empty() ? "" : (" reason=\"" + reason + "\"").c_str());
    Logger::Info("[BanManager::AddBan] Ban entry created and stored for SteamID '%s', total bans now: %zu",
                 steamId.c_str(), m_bans.size());
    Logger::Debug("[BanManager::AddBan] Persisting bans to disk after adding new ban");
    SaveBans();
    Logger::Trace("[BanManager::AddBan] Exit");
}

bool BanManager::RemoveBan(const std::string& steamId) {
    Logger::Trace("[BanManager::RemoveBan] Entry, steamId='%s'", steamId.c_str());
    Logger::Debug("[BanManager::RemoveBan] Searching for ban entry for SteamID '%s'", steamId.c_str());
    auto it = m_bans.find(steamId);
    if (it == m_bans.end()) {
        Logger::Debug("[BanManager::RemoveBan] No ban entry found for SteamID '%s'", steamId.c_str());
        Logger::Trace("[BanManager::RemoveBan] Exit, returning false");
        return false;
    }
    Logger::Debug("[BanManager::RemoveBan] Found ban entry for SteamID '%s' (type=%s, reason='%s'), removing",
                  steamId.c_str(),
                  it->second.type == BanType::Permanent ? "Permanent" : "Temporary",
                  it->second.reason.c_str());
    m_bans.erase(it);
    Logger::Info("BanManager: %s unbanned", steamId.c_str());
    Logger::Info("[BanManager::RemoveBan] Successfully removed ban for SteamID '%s', total bans now: %zu",
                 steamId.c_str(), m_bans.size());
    Logger::Debug("[BanManager::RemoveBan] Persisting bans to disk after removing ban");
    SaveBans();
    Logger::Trace("[BanManager::RemoveBan] Exit, returning true");
    return true;
}

bool BanManager::IsBanned(const std::string& steamId) const {
    Logger::Trace("[BanManager::IsBanned] Entry, steamId='%s'", steamId.c_str());
    auto it = m_bans.find(steamId);
    if (it == m_bans.end()) {
        Logger::Debug("[BanManager::IsBanned] SteamID '%s' not found in ban list - not banned", steamId.c_str());
        Logger::Trace("[BanManager::IsBanned] Exit, returning false");
        return false;
    }
    if (it->second.type == BanType::Permanent) {
        Logger::Debug("[BanManager::IsBanned] SteamID '%s' has a permanent ban - banned", steamId.c_str());
        Logger::Info("[BanManager::IsBanned] Ban check: SteamID '%s' is PERMANENTLY banned (reason='%s')",
                     steamId.c_str(), it->second.reason.c_str());
        Logger::Trace("[BanManager::IsBanned] Exit, returning true (permanent)");
        return true;
    }
    bool stillBanned = std::chrono::system_clock::now() < it->second.expiresAt;
    Logger::Debug("[BanManager::IsBanned] SteamID '%s' has a temporary ban, stillBanned=%s",
                  steamId.c_str(), stillBanned ? "true" : "false");
    if (stillBanned) {
        Logger::Info("[BanManager::IsBanned] Ban check: SteamID '%s' is TEMPORARILY banned (reason='%s')",
                     steamId.c_str(), it->second.reason.c_str());
    } else {
        Logger::Debug("[BanManager::IsBanned] Temporary ban for SteamID '%s' has expired", steamId.c_str());
    }
    Logger::Trace("[BanManager::IsBanned] Exit, returning %s", stillBanned ? "true" : "false");
    return stillBanned;
}

std::chrono::seconds BanManager::GetRemainingBan(const std::string& steamId) const {
    Logger::Trace("[BanManager::GetRemainingBan] Entry, steamId='%s'", steamId.c_str());
    auto it = m_bans.find(steamId);
    if (it == m_bans.end()) {
        Logger::Debug("[BanManager::GetRemainingBan] SteamID '%s' not found in ban list, returning -1", steamId.c_str());
        Logger::Trace("[BanManager::GetRemainingBan] Exit, returning -1 seconds");
        return std::chrono::seconds(-1);
    }
    if (it->second.type == BanType::Permanent) {
        Logger::Debug("[BanManager::GetRemainingBan] SteamID '%s' has permanent ban, returning 0 (infinite)", steamId.c_str());
        Logger::Trace("[BanManager::GetRemainingBan] Exit, returning 0 seconds (permanent)");
        return std::chrono::seconds::zero();
    }
    auto rem = std::chrono::duration_cast<std::chrono::seconds>(it->second.expiresAt - std::chrono::system_clock::now());
    auto result = rem.count() > 0 ? rem : std::chrono::seconds(0);
    Logger::Debug("[BanManager::GetRemainingBan] SteamID '%s' has %lld seconds remaining on temporary ban",
                  steamId.c_str(), static_cast<long long>(result.count()));
    Logger::Trace("[BanManager::GetRemainingBan] Exit, returning %lld seconds", static_cast<long long>(result.count()));
    return result;
}

void BanManager::CleanupExpired() {
    Logger::Trace("[BanManager::CleanupExpired] Entry, total bans=%zu", m_bans.size());
    auto now = std::chrono::system_clock::now();
    bool changed = false;
    size_t removedCount = 0;
    for (auto it = m_bans.begin(); it != m_bans.end(); ) {
        if (it->second.type == BanType::Temporary && now >= it->second.expiresAt) {
            Logger::Info("BanManager: expired ban removed for %s", it->first.c_str());
            Logger::Debug("[BanManager::CleanupExpired] Removing expired temporary ban for SteamID '%s' (reason='%s')",
                          it->first.c_str(), it->second.reason.c_str());
            it = m_bans.erase(it);
            changed = true;
            ++removedCount;
        } else {
            Logger::Trace("[BanManager::CleanupExpired] Ban for SteamID '%s' is still active (type=%s)",
                          it->first.c_str(),
                          it->second.type == BanType::Permanent ? "Permanent" : "Temporary");
            ++it;
        }
    }
    if (changed) {
        Logger::Info("[BanManager::CleanupExpired] Removed %zu expired bans, %zu bans remaining",
                     removedCount, m_bans.size());
        Logger::Debug("[BanManager::CleanupExpired] Persisting bans to disk after cleanup");
        SaveBans();
    } else {
        Logger::Debug("[BanManager::CleanupExpired] No expired bans found");
    }
    Logger::Trace("[BanManager::CleanupExpired] Exit");
}

std::vector<BanEntry> BanManager::GetAllBans() const {
    Logger::Trace("[BanManager::GetAllBans] Entry");
    std::vector<BanEntry> vec;
    vec.reserve(m_bans.size());
    Logger::Debug("[BanManager::GetAllBans] Building ban list vector, total bans=%zu", m_bans.size());
    for (const auto& kv : m_bans) {
        vec.push_back(kv.second);
        Logger::Trace("[BanManager::GetAllBans] Added ban entry for SteamID '%s' (type=%s)",
                      kv.first.c_str(),
                      kv.second.type == BanType::Permanent ? "Permanent" : "Temporary");
    }
    Logger::Debug("[BanManager::GetAllBans] Returning %zu ban entries", vec.size());
    Logger::Trace("[BanManager::GetAllBans] Exit, returning vector of size %zu", vec.size());
    return vec;
}

std::string BanManager::SerializeEntry(const BanEntry& entry) const {
    Logger::Trace("[BanManager::SerializeEntry] Entry, steamId='%s', type=%s, reason='%s'",
                  entry.steamId.c_str(),
                  entry.type == BanType::Permanent ? "Permanent" : "Temporary",
                  entry.reason.c_str());
    std::ostringstream oss;
    oss << entry.steamId << "|"
        << (entry.type == BanType::Permanent ? "P" : "T") << "|";
    if (entry.type == BanType::Temporary) {
        // Persist the expiry as an ABSOLUTE system_clock (wall-clock) deadline. expiresAt is a
        // system_clock time_point whose epoch is stable across process restarts/reboots, so the
        // raw seconds-since-epoch round-trips correctly (see DeserializeEntry).
        auto expires = std::chrono::duration_cast<std::chrono::seconds>(
            entry.expiresAt.time_since_epoch()).count();
        oss << expires;
        Logger::Debug("[BanManager::SerializeEntry] Temporary ban expiry serialized as %lld seconds since epoch",
                      static_cast<long long>(expires));
    } else {
        Logger::Debug("[BanManager::SerializeEntry] Permanent ban - no expiry to serialize");
    }
    oss << "|"<< entry.reason;
    std::string result = oss.str();
    Logger::Trace("[BanManager::SerializeEntry] Exit, serialized='%s'", result.c_str());
    return result;
}

BanEntry BanManager::DeserializeEntry(const std::string& line) const {
    Logger::Trace("[BanManager::DeserializeEntry] Entry, line='%s'", line.c_str());
    BanEntry entry;
    std::istringstream iss(line);
    std::string token;
    std::getline(iss, entry.steamId, '|');
    Logger::Debug("[BanManager::DeserializeEntry] Parsed steamId='%s'", entry.steamId.c_str());
    std::getline(iss, token, '|');
    entry.type = (token == "P") ? BanType::Permanent : BanType::Temporary;
    Logger::Debug("[BanManager::DeserializeEntry] Parsed type='%s' -> %s",
                  token.c_str(),
                  entry.type == BanType::Permanent ? "Permanent" : "Temporary");
    if (entry.type == BanType::Temporary) {
        std::getline(iss, token, '|');
        long long secs = 0;
        try {
            secs = std::stoll(token);
        } catch (const std::exception& e) {
            // Malformed expiry field: don't crash the server loading bans - treat
            // as an already-expired temporary ban and warn.
            Logger::Warn("[BanManager::DeserializeEntry] Bad expiry token '%s' (%s); treating as expired",
                         token.c_str(), e.what());
            secs = 0;
        }
        // The stored value is an ABSOLUTE system_clock (wall-clock) deadline, which is
        // restart-stable. Clamp to a sane range before constructing the time_point: a
        // pathological value (negative, or near LLONG_MAX from a corrupted ban file) would
        // overflow the duration->time_point conversion (UB) on construction. ~100 years of
        // seconds is far beyond any real ban and safely inside the representable range. An
        // already-past deadline simply makes IsBanned return false (the ban is gone).
        constexpr long long kMaxBanSeconds = 100LL * 365 * 24 * 60 * 60;
        if (secs < 0) secs = 0;
        if (secs > kMaxBanSeconds) secs = kMaxBanSeconds;
        entry.expiresAt = std::chrono::system_clock::time_point(std::chrono::seconds(secs));
        Logger::Debug("[BanManager::DeserializeEntry] Parsed temporary ban expiry: %lld seconds since epoch", secs);
    } else {
        std::getline(iss, token, '|');
        Logger::Debug("[BanManager::DeserializeEntry] Permanent ban - skipped expiry field (token='%s')", token.c_str());
    }
    std::getline(iss, entry.reason);
    Logger::Debug("[BanManager::DeserializeEntry] Parsed reason='%s'", entry.reason.c_str());
    Logger::Trace("[BanManager::DeserializeEntry] Exit, returning entry for SteamID '%s'", entry.steamId.c_str());
    return entry;
}
