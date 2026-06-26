// src/Security/AuthManager.cpp

#include "Security/AuthManager.h"
#include "Security/PasswordHasher.h"
#include "Config/SecurityConfig.h"
#include "Utils/Logger.h"

namespace {
// ---- Defensive bounds on attacker-controlled identity/credential strings ----
// These are deliberately generous upper limits whose only job is to reject
// absurd inputs that could be abused for CPU/memory exhaustion. They are far
// larger than any legitimate value and therefore never reject a real credential.
constexpr size_t kMaxUserLen     = 256;
constexpr size_t kMaxPasswordLen = 1024;
constexpr size_t kMaxTokenLen    = 4096;
constexpr size_t kMaxSteamIdLen  = 64;
constexpr size_t kMaxTicketLen   = 8192;

// Hard cap on the number of per-user records we will track. Authenticate()
// records failed-attempt / lockout state keyed by the (attacker-supplied) user
// name; without a cap a flood of distinct names would grow the map without
// bound (memory-exhaustion DoS). Registered users are never evicted.
constexpr size_t kMaxTrackedUsers = 4096;

// A user name must be non-empty, bounded, and free of NUL / control characters
// (which have no business in a credential identifier and tend to indicate an
// injection / log-spoofing attempt).
bool IsValidUserName(const std::string& user) {
    if (user.empty() || user.size() > kMaxUserLen) return false;
    for (unsigned char c : user) {
        if (c < 0x20 || c == 0x7f) return false; // control chars incl. NUL
    }
    return true;
}

bool IsPlausibleSteamId(const std::string& steamId) {
    if (steamId.empty() || steamId.size() > kMaxSteamIdLen) return false;
    for (unsigned char c : steamId) {
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

// A fixed, well-formed PBKDF2 hash used to perform a *dummy* verification when
// the supplied user is unknown, so Authenticate() spends roughly the same time
// whether or not the account exists. This defeats user-enumeration via timing.
// Computed once on first use (PasswordHasher::Verify itself is constant-time).
const std::string& DummyHash() {
    static const std::string kDummy =
        PasswordHasher::Hash("invalid-account-timing-equalizer-placeholder");
    return kDummy;
}
} // namespace

AuthManager::AuthManager(std::shared_ptr<SecurityConfig> config, int maxLoginAttempts)
    : m_config(std::move(config))
    , m_maxLoginAttempts(maxLoginAttempts > 0 ? maxLoginAttempts : 3)
    , m_eacProxy()
    , m_tokenManager() {
    Logger::Trace("[AuthManager::AuthManager] Entry: config=%p, maxLoginAttempts=%d",
                  (void*)m_config.get(), m_maxLoginAttempts);
    m_eacProxy.Initialize();
    Logger::Info("[AuthManager] Initialized (maxLoginAttempts=%d)", m_maxLoginAttempts);
    Logger::Trace("[AuthManager::AuthManager] Exit");
}

AuthManager::~AuthManager() {
    Logger::Trace("[AuthManager::~AuthManager] Entry");
    m_eacProxy.Shutdown();
    Logger::Trace("[AuthManager::~AuthManager] Exit");
}

bool AuthManager::ValidateSteamTicket(const std::string& steamId,
                                      const std::vector<uint8_t>& ticket) {
    Logger::Trace("[AuthManager::ValidateSteamTicket] Entry: steamId='%s'", steamId.c_str());
    // Reject malformed / oversized identity before handing it to the proxy.
    if (!IsPlausibleSteamId(steamId)) {
        Logger::Warn("[AuthManager::ValidateSteamTicket] Rejecting malformed steamId");
        return false;
    }
    if (ticket.empty() || ticket.size() > kMaxTicketLen) {
        Logger::Warn("[AuthManager::ValidateSteamTicket] Rejecting empty/oversized ticket (size=%zu)",
                     ticket.size());
        return false;
    }
    bool ok = m_eacProxy.ValidateSessionTicket(steamId, ticket);
    Logger::Trace("[AuthManager::ValidateSteamTicket] Exit: return %d", ok);
    return ok;
}

bool AuthManager::RegisterUser(const std::string& user, const std::string& password) {
    Logger::Trace("[AuthManager::RegisterUser] Entry: user='%s'", user.c_str());
    if (!IsValidUserName(user)) {
        Logger::Warn("[AuthManager::RegisterUser] Rejecting empty/malformed user name");
        return false;
    }
    if (password.size() > kMaxPasswordLen) {
        // Bound the work PBKDF2 is asked to do; never reject a real password by length.
        Logger::Warn("[AuthManager::RegisterUser] Rejecting oversized password (len=%zu)",
                     password.size());
        return false;
    }
    std::string hash = PasswordHasher::Hash(password);
    if (hash.empty()) {
        Logger::Error("[AuthManager::RegisterUser] Failed to hash password for '%s'", user.c_str());
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    Credential& cred = m_users[user];
    cred.hash = hash;
    cred.failedAttempts = 0;
    cred.lockedOut = false;
    cred.lastSeen = std::chrono::steady_clock::now();
    Logger::Info("[AuthManager::RegisterUser] Registered user '%s'", user.c_str());
    Logger::Trace("[AuthManager::RegisterUser] Exit: return true");
    return true;
}

bool AuthManager::Authenticate(const std::string& user, const std::string& password) {
    Logger::Trace("[AuthManager::Authenticate] Entry: user='%s'", user.c_str());

    // Missing credentials always fail and are not counted as attempts.
    if (user.empty() || password.empty()) {
        Logger::Warn("[AuthManager::Authenticate] Missing credentials");
        return false;
    }
    // Reject malformed / oversized inputs before they reach the hasher or the
    // tracking map (defends against CPU/memory exhaustion and odd identifiers).
    if (!IsValidUserName(user) || password.size() > kMaxPasswordLen) {
        Logger::Warn("[AuthManager::Authenticate] Rejecting malformed credentials");
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    // Look up WITHOUT inserting, so a flood of unknown names cannot grow the map
    // here; record creation (and its cap) is handled below for failed attempts.
    auto it = m_users.find(user);
    const bool known = (it != m_users.end());

    if (known && it->second.lockedOut) {
        Logger::Warn("[AuthManager::Authenticate] User '%s' is locked out", user.c_str());
        return false;
    }

    // Always run a (constant-time) PBKDF2 verification — against the real hash
    // when the user exists, otherwise against a fixed dummy hash — so the
    // response time does not reveal whether the account exists (anti-enumeration).
    const bool haveHash = known && !it->second.hash.empty();
    const std::string& hashToCheck = haveHash ? it->second.hash : DummyHash();
    const bool verified = PasswordHasher::Verify(password, hashToCheck);
    const bool ok = haveHash && verified;

    if (ok) {
        it->second.failedAttempts = 0;
        it->second.lockedOut = false;
        it->second.lastSeen = std::chrono::steady_clock::now();
        Logger::Info("[AuthManager::Authenticate] User '%s' authenticated", user.c_str());
        Logger::Trace("[AuthManager::Authenticate] Exit: return true");
        return true;
    }

    // Failed attempt. Resolve the record to charge it against, creating one for
    // an unknown user only while we are under the tracking cap (evicting the
    // oldest attempt-only record first) — a bounded attempt store that still
    // applies lockout to brute-force from unknown names.
    Credential* cred = nullptr;
    if (known) {
        cred = &it->second;
    } else {
        if (m_users.size() >= kMaxTrackedUsers) {
            EvictOldestTrackingRecord_locked();
        }
        if (m_users.size() < kMaxTrackedUsers) {
            cred = &m_users[user];
        } else {
            // At capacity with nothing evictable (all slots are registered
            // users). Refuse to grow the map; still safely reject the attempt.
            Logger::Warn("[AuthManager::Authenticate] Tracking map at capacity; "
                         "not recording attempt for unknown user");
        }
    }

    if (cred) {
        cred->failedAttempts++;
        cred->lastSeen = std::chrono::steady_clock::now();
        if (cred->failedAttempts >= m_maxLoginAttempts) {
            cred->lockedOut = true;
            Logger::Warn("[AuthManager::Authenticate] User '%s' locked out after %d failed attempts",
                         user.c_str(), cred->failedAttempts);
        } else {
            Logger::Debug("[AuthManager::Authenticate] User '%s' failed attempt %d/%d",
                          user.c_str(), cred->failedAttempts, m_maxLoginAttempts);
        }
    }
    Logger::Trace("[AuthManager::Authenticate] Exit: return false");
    return false;
}

void AuthManager::EvictOldestTrackingRecord_locked() {
    // Find the oldest record that is an attempt-only tracker (no stored hash).
    // Registered credentials (non-empty hash) are never evicted.
    auto oldest = m_users.end();
    for (auto it = m_users.begin(); it != m_users.end(); ++it) {
        if (!it->second.hash.empty()) continue; // skip registered users
        if (oldest == m_users.end() || it->second.lastSeen < oldest->second.lastSeen) {
            oldest = it;
        }
    }
    if (oldest != m_users.end()) {
        Logger::Debug("[AuthManager] Evicting stale auth-attempt record to enforce tracking cap");
        m_users.erase(oldest);
    }
}

bool AuthManager::IsLockedOut(const std::string& user) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_users.find(user);
    bool locked = (it != m_users.end()) && it->second.lockedOut;
    Logger::Trace("[AuthManager::IsLockedOut] user='%s' -> %d", user.c_str(), locked);
    return locked;
}

void AuthManager::ResetLockout(const std::string& user) {
    Logger::Trace("[AuthManager::ResetLockout] Entry: user='%s'", user.c_str());
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_users.find(user);
    if (it != m_users.end()) {
        it->second.failedAttempts = 0;
        it->second.lockedOut = false;
    }
    Logger::Trace("[AuthManager::ResetLockout] Exit");
}

std::string AuthManager::IssueSessionToken(const std::string& user) {
    Logger::Trace("[AuthManager::IssueSessionToken] Entry: user='%s'", user.c_str());
    // Never mint a token for a malformed/empty identity (would bind a secret to
    // a junk principal). Callers must treat an empty return as failure.
    if (!IsValidUserName(user)) {
        Logger::Warn("[AuthManager::IssueSessionToken] Rejecting malformed user");
        return std::string();
    }
    std::string token = m_tokenManager.GenerateToken(user);
    Logger::Trace("[AuthManager::IssueSessionToken] Exit");
    return token;
}

bool AuthManager::ValidateSessionToken(const std::string& user, const std::string& token) {
    // Reject malformed/oversized inputs up front; a valid token is bounded and a
    // valid user name is well-formed, so this never rejects a legitimate pair.
    if (!IsValidUserName(user)) {
        Logger::Warn("[AuthManager::ValidateSessionToken] Rejecting malformed user");
        return false;
    }
    if (token.empty() || token.size() > kMaxTokenLen) {
        Logger::Warn("[AuthManager::ValidateSessionToken] Rejecting empty/oversized token");
        return false;
    }
    return m_tokenManager.ValidateToken(user, token);
}
