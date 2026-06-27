// src/Game/MutatorManager.h – Server-side gameplay mutators
//
// IMPORTANT: these are server-side C++ mutators, NOT the game's UnrealScript
// mutators. There is no UnrealScript VM in this emulator (see
// docs/SCRIPTING_ASSESSMENT.md); instead, a mutator here is a small object that
// hooks server gameplay events (round start/end, spawn, kill) and can tweak
// server-controlled parameters such as the global damage scale or friendly fire.

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <cstdint>

class GameServer;

// Interface implemented by every mutator. Hooks are optional; the default
// implementations do nothing so a mutator only overrides what it cares about.
class IMutator {
public:
    virtual ~IMutator() = default;

    // Stable identifier used in config (Mutators.enabled_list).
    virtual std::string Name() const = 0;
    virtual std::string Description() const { return ""; }

    // Lifecycle / gameplay hooks.
    virtual void OnInit(GameServer* /*server*/) {}
    virtual void OnShutdown(GameServer* /*server*/) {}
    virtual void OnRoundStart() {}
    virtual void OnRoundEnd() {}
    virtual void OnPlayerSpawned(uint32_t /*clientId*/) {}
    virtual void OnPlayerKilled(uint32_t /*killerId*/, uint32_t /*victimId*/) {}

    // Damage pipeline hook: receive the current damage value and return the
    // modified value. Mutators are chained in registration order.
    virtual float ModifyDamage(float damage) { return damage; }
};

// Loads, owns and dispatches mutators.
class MutatorManager {
public:
    using Factory = std::function<std::unique_ptr<IMutator>()>;

    explicit MutatorManager(GameServer* server);
    ~MutatorManager();

    // Register a built-in or custom mutator factory under an id.
    void RegisterFactory(const std::string& id, Factory factory);

    // Instantiate the mutators named in a comma-separated list (typically from
    // Mutators.enabled_list). Unknown ids are logged and skipped. Calls OnInit
    // on each activated mutator. Returns the number activated.
    size_t LoadFromConfig(const std::string& enabledList);

    // Activate a single mutator by id. Returns false if unknown / already active.
    bool Activate(const std::string& id);

    void Shutdown();

    // Event dispatch — call these from the game loop / callbacks.
    void DispatchRoundStart();
    void DispatchRoundEnd();
    void DispatchPlayerSpawned(uint32_t clientId);
    void DispatchPlayerKilled(uint32_t killerId, uint32_t victimId);
    float DispatchModifyDamage(float damage);

    size_t ActiveCount() const { return m_active.size(); }
    std::vector<std::string> GetActiveNames() const;
    std::vector<std::string> GetRegisteredIds() const;
    void LogSummary() const;

private:
    void RegisterBuiltins();
    bool IsActive(const std::string& id) const;

    GameServer*                                      m_server;
    std::unordered_map<std::string, Factory>         m_factories;
    std::vector<std::unique_ptr<IMutator>>           m_active;
};
