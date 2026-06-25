// src/Security/AuthManager.cpp

#include "Security/AuthManager.h"
#include "Security/PasswordHasher.h"
#include "Config/SecurityConfig.h"
#include "Utils/Logger.h"

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
    bool ok = m_eacProxy.ValidateSessionTicket(steamId, ticket);
    Logger::Trace("[AuthManager::ValidateSteamTicket] Exit: return %d", ok);
    return ok;
}

bool AuthManager::RegisterUser(const std::string& user, const std::string& password) {
    Logger::Trace("[AuthManager::RegisterUser] Entry: user='%s'", user.c_str());
    if (user.empty()) {
        Logger::Warn("[AuthManager::RegisterUser] Rejecting empty user name");
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

    std::lock_guard<std::mutex> lock(m_mutex);
    Credential& cred = m_users[user]; // creates a tracking record for unknown users

    if (cred.lockedOut) {
        Logger::Warn("[AuthManager::Authenticate] User '%s' is locked out", user.c_str());
        return false;
    }

    bool ok = !cred.hash.empty() && PasswordHasher::Verify(password, cred.hash);

    if (ok) {
        cred.failedAttempts = 0;
        cred.lockedOut = false;
        Logger::Info("[AuthManager::Authenticate] User '%s' authenticated", user.c_str());
        Logger::Trace("[AuthManager::Authenticate] Exit: return true");
        return true;
    }

    cred.failedAttempts++;
    if (cred.failedAttempts >= m_maxLoginAttempts) {
        cred.lockedOut = true;
        Logger::Warn("[AuthManager::Authenticate] User '%s' locked out after %d failed attempts",
                     user.c_str(), cred.failedAttempts);
    } else {
        Logger::Debug("[AuthManager::Authenticate] User '%s' failed attempt %d/%d",
                      user.c_str(), cred.failedAttempts, m_maxLoginAttempts);
    }
    Logger::Trace("[AuthManager::Authenticate] Exit: return false");
    return false;
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
    std::string token = m_tokenManager.GenerateToken(user);
    Logger::Trace("[AuthManager::IssueSessionToken] Exit");
    return token;
}

bool AuthManager::ValidateSessionToken(const std::string& user, const std::string& token) {
    return m_tokenManager.ValidateToken(user, token);
}
