// src/Security/TokenManager.h
//
// Issues and validates per-user session / CSRF tokens. A token is an opaque,
// cryptographically-random string bound to the user it was issued for. Tokens
// optionally expire after a configurable lifetime.
//
// Semantics (matches the security test spec):
//   * GenerateToken(user)            -> fresh random token bound to `user`.
//   * ValidateToken(user, token)     -> true iff `token` was issued for `user`
//                                       and has not expired. A token issued for
//                                       one user never validates for another.

#pragma once

#include <string>
#include <chrono>
#include <mutex>
#include <unordered_map>

class TokenManager {
public:
    // Default token lifetime (0 == never expires).
    explicit TokenManager(std::chrono::seconds tokenLifetime = std::chrono::seconds(0));
    ~TokenManager();

    // Generate a fresh token for `user`. Each call returns a distinct token;
    // previously issued tokens for the same user remain valid until they expire
    // or are revoked.
    std::string GenerateToken(const std::string& user);

    // Validate that `token` was issued for `user` and is still live.
    bool ValidateToken(const std::string& user, const std::string& token);

    // Explicitly revoke a single token (no-op if unknown).
    void RevokeToken(const std::string& token);

    // Revoke every token belonging to `user`.
    void RevokeUser(const std::string& user);

    // Number of currently-tracked (issued, not yet purged) tokens.
    size_t ActiveTokenCount() const;

private:
    struct Entry {
        std::string user;
        std::chrono::steady_clock::time_point expiry;  // only meaningful if m_lifetime > 0
        std::chrono::steady_clock::time_point created;  // for bounded-cap LRU eviction
        bool hasExpiry;
    };

    // Hard cap on the number of tracked tokens. Tokens are issued in response to
    // (attacker-reachable) authentication, so without a ceiling the map is an
    // unbounded memory-exhaustion DoS vector. When the cap is hit we first purge
    // any expired entries and, failing that, evict the oldest one.
    static constexpr size_t kMaxTokens = 100000;

    bool IsExpired(const Entry& e) const;

    // Caller must hold m_mutex. Drops expired entries; returns how many removed.
    size_t PruneExpiredLocked();
    // Caller must hold m_mutex. Evicts the single oldest entry (by creation time).
    void EvictOldestLocked();

    std::chrono::seconds m_lifetime;
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, Entry> m_tokens; // token -> entry
};
