// src/Security/EACProxy.h
//
// EACProxy is the server-side facade the rest of the emulator talks to when it
// needs to validate a connecting client's Steam session ticket and track that
// client's anti-cheat authentication state. It deliberately exposes a small,
// virtual surface so it can be mocked in tests and, later, swapped for a real
// EAC backend without touching callers.
//
// This is an EMULATED proxy: there is no real Easy Anti-Cheat backend in this
// repo, so ticket validation applies deterministic structural checks (Steam64
// id shape, non-empty/non-poisoned ticket) rather than contacting Steam.

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <unordered_set>

class SecurityConfig;

class EACProxy {
public:
    EACProxy();
    explicit EACProxy(std::shared_ptr<SecurityConfig> config);
    virtual ~EACProxy();

    // Bring the proxy up. Returns false if it could not start (e.g. already
    // shut down by config). Idempotent-ish: calling twice keeps it running.
    virtual bool Initialize();

    // Tear the proxy down and forget all authenticated clients.
    virtual void Shutdown();

    // Whether the proxy is currently running.
    virtual bool IsRunning() const;

    // Validate a Steam session ticket for `steamId`. On success the client is
    // recorded as authenticated (see IsClientAuthenticated). On failure any
    // prior authentication for that id is cleared.
    virtual bool ValidateSessionTicket(const std::string& steamId,
                                       const std::vector<uint8_t>& ticket);

    // Has this steamId passed ValidateSessionTicket since the last Shutdown?
    virtual bool IsClientAuthenticated(const std::string& steamId) const;

    // Drop a single client's authenticated state (e.g. on disconnect).
    virtual void RemoveClient(const std::string& steamId);

    // Minimal hook used by the EAC anti-cheat path: inspect a raw remote-memory
    // read payload. Returns true if the payload looks clean, false if it is
    // malformed or matches an obviously-suspicious pattern. Provided so the
    // proxy can sit in front of EACServerEmulator-style scanning.
    virtual bool HandleRemoteMemoryRead(const std::vector<uint8_t>& data);

protected:
    // Structural validation of a Steam64 id ("7656119xxxxxxxxxx", 17 digits).
    static bool IsWellFormedSteamId(const std::string& steamId);

private:
    std::shared_ptr<SecurityConfig> m_config;
    bool m_running;
    mutable std::mutex m_mutex;
    std::unordered_set<std::string> m_authenticated;
};
