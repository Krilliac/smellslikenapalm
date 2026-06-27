// src/Game/MutatorManager.cpp – Implementation of server-side mutators

#include "Game/MutatorManager.h"
#include "Game/GameServer.h"
#include "Game/DamageSystem.h"
#include "Utils/Logger.h"
#include "Utils/StringUtils.h"

// ---------------------------------------------------------------------------
// Built-in mutators
// ---------------------------------------------------------------------------
namespace {

// Increases all damage by a fixed multiplier (default 1.5x). Implemented both as
// a global DamageSystem scale (affects every code path) and as a ModifyDamage
// hook (for any callers that route through the mutator chain directly).
class HardcoreDamageMutator : public IMutator {
public:
    std::string Name() const override { return "hardcore_damage"; }
    std::string Description() const override { return "Increases all weapon damage (1.5x)"; }

    void OnInit(GameServer* server) override {
        if (server) {
            if (auto* ds = server->GetDamageSystem()) {
                ds->SetGlobalDamageScale(m_scale);
                Logger::Info("[Mutator:hardcore_damage] Global damage scale set to %.2f", m_scale);
            }
        }
    }
    void OnShutdown(GameServer* server) override {
        if (server) {
            if (auto* ds = server->GetDamageSystem()) ds->SetGlobalDamageScale(1.0f);
        }
    }
    float ModifyDamage(float damage) override { return damage * m_scale; }

private:
    float m_scale = 1.5f;
};

// Forces friendly fire ON regardless of game-mode default.
class FriendlyFireMutator : public IMutator {
public:
    std::string Name() const override { return "friendly_fire"; }
    std::string Description() const override { return "Forces friendly fire on"; }
    void OnInit(GameServer* server) override {
        if (server) {
            if (auto* ds = server->GetDamageSystem()) {
                ds->SetFriendlyFireEnabled(true);
                Logger::Info("[Mutator:friendly_fire] Friendly fire forced ON");
            }
        }
    }
};

// Forces friendly fire OFF regardless of game-mode default.
class NoFriendlyFireMutator : public IMutator {
public:
    std::string Name() const override { return "no_friendly_fire"; }
    std::string Description() const override { return "Forces friendly fire off"; }
    void OnInit(GameServer* server) override {
        if (server) {
            if (auto* ds = server->GetDamageSystem()) {
                ds->SetFriendlyFireEnabled(false);
                Logger::Info("[Mutator:no_friendly_fire] Friendly fire forced OFF");
            }
        }
    }
};

// "Instagib": damage scaled massively so any clean hit is lethal.
class InstagibMutator : public IMutator {
public:
    std::string Name() const override { return "instagib"; }
    std::string Description() const override { return "One-hit kills (10x damage)"; }
    void OnInit(GameServer* server) override {
        if (server) {
            if (auto* ds = server->GetDamageSystem()) {
                ds->SetGlobalDamageScale(10.0f);
                Logger::Info("[Mutator:instagib] Global damage scale set to 10.0");
            }
        }
    }
    void OnShutdown(GameServer* server) override {
        if (server) {
            if (auto* ds = server->GetDamageSystem()) ds->SetGlobalDamageScale(1.0f);
        }
    }
    float ModifyDamage(float damage) override { return damage * 10.0f; }
};

} // namespace

// ---------------------------------------------------------------------------
// MutatorManager
// ---------------------------------------------------------------------------

MutatorManager::MutatorManager(GameServer* server)
    : m_server(server)
{
    RegisterBuiltins();
    Logger::Info("MutatorManager initialized (%zu built-in mutators registered)", m_factories.size());
}

MutatorManager::~MutatorManager()
{
    Shutdown();
}

void MutatorManager::RegisterBuiltins()
{
    RegisterFactory("hardcore_damage",  [] { return std::make_unique<HardcoreDamageMutator>(); });
    RegisterFactory("friendly_fire",    [] { return std::make_unique<FriendlyFireMutator>(); });
    RegisterFactory("no_friendly_fire", [] { return std::make_unique<NoFriendlyFireMutator>(); });
    RegisterFactory("instagib",         [] { return std::make_unique<InstagibMutator>(); });
}

void MutatorManager::RegisterFactory(const std::string& id, Factory factory)
{
    m_factories[StringUtils::ToLower(StringUtils::Trim(id))] = std::move(factory);
}

bool MutatorManager::IsActive(const std::string& id) const
{
    for (const auto& m : m_active) {
        if (StringUtils::EqualsIgnoreCase(m->Name(), id)) return true;
    }
    return false;
}

bool MutatorManager::Activate(const std::string& idRaw)
{
    std::string id = StringUtils::ToLower(StringUtils::Trim(idRaw));
    if (id.empty()) return false;
    if (IsActive(id)) {
        Logger::Warn("MutatorManager: mutator '%s' already active", id.c_str());
        return false;
    }
    auto it = m_factories.find(id);
    if (it == m_factories.end()) {
        Logger::Warn("MutatorManager: unknown mutator id '%s' (skipped)", id.c_str());
        return false;
    }
    auto mutator = it->second();
    if (!mutator) {
        Logger::Error("MutatorManager: factory for '%s' returned null", id.c_str());
        return false;
    }
    Logger::Info("MutatorManager: activating mutator '%s' — %s",
                 mutator->Name().c_str(), mutator->Description().c_str());
    mutator->OnInit(m_server);
    m_active.push_back(std::move(mutator));
    return true;
}

size_t MutatorManager::LoadFromConfig(const std::string& enabledList)
{
    size_t activated = 0;
    for (const auto& tok : StringUtils::Split(enabledList, ',')) {
        std::string id = StringUtils::Trim(tok);
        if (id.empty()) continue;
        if (Activate(id)) ++activated;
    }
    Logger::Info("MutatorManager: %zu mutators active after config load", activated);
    return activated;
}

void MutatorManager::Shutdown()
{
    for (auto& m : m_active) {
        if (m) m->OnShutdown(m_server);
    }
    m_active.clear();
}

void MutatorManager::DispatchRoundStart()
{
    for (auto& m : m_active) m->OnRoundStart();
}

void MutatorManager::DispatchRoundEnd()
{
    for (auto& m : m_active) m->OnRoundEnd();
}

void MutatorManager::DispatchPlayerSpawned(uint32_t clientId)
{
    for (auto& m : m_active) m->OnPlayerSpawned(clientId);
}

void MutatorManager::DispatchPlayerKilled(uint32_t killerId, uint32_t victimId)
{
    for (auto& m : m_active) m->OnPlayerKilled(killerId, victimId);
}

float MutatorManager::DispatchModifyDamage(float damage)
{
    for (auto& m : m_active) damage = m->ModifyDamage(damage);
    return damage;
}

std::vector<std::string> MutatorManager::GetActiveNames() const
{
    std::vector<std::string> out;
    out.reserve(m_active.size());
    for (const auto& m : m_active) out.push_back(m->Name());
    return out;
}

std::vector<std::string> MutatorManager::GetRegisteredIds() const
{
    std::vector<std::string> out;
    out.reserve(m_factories.size());
    for (const auto& kv : m_factories) out.push_back(kv.first);
    return out;
}

void MutatorManager::LogSummary() const
{
    Logger::Info("=== MutatorManager Summary ===");
    Logger::Info("Registered: %zu  Active: %zu", m_factories.size(), m_active.size());
    for (const auto& m : m_active) {
        Logger::Info("  [active] %s — %s", m->Name().c_str(), m->Description().c_str());
    }
    Logger::Info("==============================");
}
