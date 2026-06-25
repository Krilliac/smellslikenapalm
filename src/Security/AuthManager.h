// src/Security/AuthManager.h
//
// AuthManager is the high-level entry point that ties the security primitives
// together:
//   * Steam/EAC session-ticket validation        (via EACProxy)
//   * Local credential authentication            (via PasswordHasher)
//   * Per-user session tokens                     (via TokenManager)
//   * Brute-force lockout after repeated failures
//
// It is intentionally self-contained and does NOT reach into GameServer or the
// login/network flow — wiring it into the connect path is a separate follow-up.
//
// Credential model: callers register a user with RegisterUser(name, password)
// (the password is stored only as a salted PBKDF2 hash). Authenticate(name,
// password) verifies against that hash and applies lockout. ValidateSteamTicket
// delegates to the EACProxy.

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "Security/EACProxy.h"
#include "Security/TokenManager.h"

class SecurityConfig;

class AuthManager {
public:
    // `maxLoginAttempts` is the number of consecutive failed Authenticate()
    // calls tolerated before a user is locked out. The SecurityConfig is kept
    // for forward-compatibility / future policy lookups.
    explicit AuthManager(std::shared_ptr<SecurityConfig> config,
                         int maxLoginAttempts = 3);
    ~AuthManager();

    // ---- Steam / EAC ------------------------------------------------------
    // Validate a Steam session ticket. Delegates to the internal EACProxy.
    bool ValidateSteamTicket(const std::string& steamId,
                             const std::vector<uint8_t>& ticket);

    // ---- Local credentials ------------------------------------------------
    // Register (or replace) a user's credentials. Returns false for an empty
    // user name. The password is stored only as a salted hash.
    bool RegisterUser(const std::string& user, const std::string& password);

    // Authenticate a user. Returns true on success. Empty user or password
    // always fails. Each failure increments the user's attempt counter; once it
    // reaches maxLoginAttempts the user is locked out and further attempts fail
    // immediately. A success resets the counter and clears any lockout.
    bool Authenticate(const std::string& user, const std::string& password);

    // True if `user` is currently locked out due to excessive failed attempts.
    bool IsLockedOut(const std::string& user) const;

    // Clear lockout / failed-attempt state for a user.
    void ResetLockout(const std::string& user);

    // ---- Session tokens ---------------------------------------------------
    // Issue a session token for an already-authenticated user.
    std::string IssueSessionToken(const std::string& user);
    bool ValidateSessionToken(const std::string& user, const std::string& token);

    // Access to the underlying EAC proxy (e.g. to check IsClientAuthenticated).
    EACProxy& GetEACProxy() { return m_eacProxy; }

private:
    struct Credential {
        std::string hash;
        int         failedAttempts = 0;
        bool        lockedOut = false;
    };

    std::shared_ptr<SecurityConfig> m_config;
    int m_maxLoginAttempts;

    EACProxy     m_eacProxy;
    TokenManager m_tokenManager;

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, Credential> m_users;
};
