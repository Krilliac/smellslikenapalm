// src/Scripting/ScriptHost.cpp

#include "Scripting/ScriptHost.h"
#include "Scripting/ScriptManager.h"
#include "Config/ConfigManager.h"
#include "Config/GameConfig.h"
#include "Config/ServerConfig.h"
#include "Game/GameServer.h"
#include "Game/PlayerManager.h"
#include "Game/TeamManager.h"
#include "Game/MapManager.h"
#include "Game/EntityManager.h"
#include "Network/NetworkManager.h"
#include "Network/Packet.h"
#include "Network/ClientConnection.h"
#include "Game/AdminManager.h"
#include "Security/EACServerEmulator.h"
#include "Security/EACPackets.h"
#include "Utils/Logger.h"
#include "Utils/StringUtils.h"
#include "Utils/FileUtils.h"
#include <chrono>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cstring>
#include <iomanip>
#include <filesystem>

// Static member initialization
ScriptManager* ScriptHost::s_scriptManager = nullptr;
std::shared_ptr<ConfigManager> ScriptHost::s_configManager = nullptr;
GameServer* ScriptHost::s_gameServer = nullptr;
EACServerEmulator* ScriptHost::s_eacServer = nullptr;
std::mutex ScriptHost::s_mutex;
bool ScriptHost::s_initialized = false;

// String storage for safe return to C#
std::map<std::string, std::string> ScriptHost::s_stringStorage;
std::mutex ScriptHost::s_stringMutex;

// Event handler registry
std::map<std::string, std::vector<std::string>> ScriptHost::s_eventHandlers;
std::mutex ScriptHost::s_eventMutex;

// Scheduled callbacks
std::vector<ScriptHost::ScheduledCallback> ScriptHost::s_scheduledCallbacks;
std::mutex ScriptHost::s_callbackMutex;

// Implementation
bool ScriptHost::Initialize(ScriptManager* scriptManager, 
                           std::shared_ptr<ConfigManager> configManager,
                           GameServer* gameServer,
                           EACServerEmulator* eacServer)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    
    if (s_initialized) {
        Logger::Warn("ScriptHost already initialized");
        return true;
    }
    
    if (!scriptManager || !configManager || !gameServer) {
        Logger::Error("ScriptHost::Initialize called with null parameters");
        return false;
    }
    
    s_scriptManager = scriptManager;
    s_configManager = configManager;
    s_gameServer = gameServer;
    s_eacServer = eacServer;
    s_initialized = true;
    
    Logger::Info("ScriptHost facade initialized successfully");
    return true;
}

void ScriptHost::Shutdown()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    
    if (!s_initialized) return;
    
    // Clear all registrations
    {
        std::lock_guard<std::mutex> eventLock(s_eventMutex);
        s_eventHandlers.clear();
    }
    
    // Clear scheduled callbacks
    {
        std::lock_guard<std::mutex> callbackLock(s_callbackMutex);
        s_scheduledCallbacks.clear();
    }
    
    // Clear string storage
    {
        std::lock_guard<std::mutex> stringLock(s_stringMutex);
        s_stringStorage.clear();
    }
    
    s_scriptManager = nullptr;
    s_configManager = nullptr;
    s_gameServer = nullptr;
    s_eacServer = nullptr;
    s_initialized = false;
    
    Logger::Info("ScriptHost facade shutdown complete");
}

ScriptHost* ScriptHost::Instance()
{
    static ScriptHost instance;
    return &instance;
}

const char* ScriptHost::StoreString(const std::string& str)
{
    std::lock_guard<std::mutex> lock(s_stringMutex);
    
    // Store string with a unique key based on its content and address
    std::string key = std::to_string(reinterpret_cast<uintptr_t>(&str)) + "_" + str;
    s_stringStorage[key] = str;
    return s_stringStorage[key].c_str();
}

PlayerManager* ScriptHost::GetPlayerManager()
{
    if (!s_gameServer) return nullptr;
    return s_gameServer->GetPlayerManager();
}

EntityManager* ScriptHost::GetEntityManager()
{
    // EntityManager not yet integrated into GameServer
    return nullptr;
}

AdminManager* ScriptHost::GetAdminManager()
{
    if (!s_gameServer) return nullptr;
    return s_gameServer->GetAdminManager();
}

NetworkManager* ScriptHost::GetNetworkManager()
{
    if (!s_gameServer) return nullptr;
    return s_gameServer->GetNetworkManager();
}

EACServerEmulator* ScriptHost::GetEACServer()
{
    return s_eacServer;
}

// Native C exports implementation
extern "C" {
    
    void RegisterEventHandler(const char* eventName, const char* methodName)
    {
        if (!eventName || !methodName) return;
        
        std::lock_guard<std::mutex> lock(ScriptHost::s_eventMutex);
        ScriptHost::s_eventHandlers[eventName].push_back(methodName);
        
        Logger::Debug("Registered event handler: %s -> %s", eventName, methodName);
    }
    
    void UnregisterEventHandler(const char* eventName, const char* methodName)
    {
        if (!eventName || !methodName) return;
        
        std::lock_guard<std::mutex> lock(ScriptHost::s_eventMutex);
        auto& handlers = ScriptHost::s_eventHandlers[eventName];
        handlers.erase(std::remove(handlers.begin(), handlers.end(), methodName), handlers.end());
        
        Logger::Debug("Unregistered event handler: %s -> %s", eventName, methodName);
    }
    
    bool ExecuteScriptMethod(const char* scriptPath, const char* methodName)
    {
        if (!ScriptHost::s_scriptManager || !scriptPath || !methodName) return false;
        return ScriptHost::s_scriptManager->ExecuteScriptMethod(scriptPath, methodName, {});
    }
    
    bool ExecuteScriptMethodWithArgs(const char* scriptPath, const char* methodName, 
                                    const char** args, int argCount)
    {
        if (!ScriptHost::s_scriptManager || !scriptPath || !methodName) return false;
        
        std::vector<std::string> argVector;
        for (int i = 0; i < argCount; ++i) {
            if (args[i]) argVector.push_back(args[i]);
        }
        
        return ScriptHost::s_scriptManager->ExecuteScriptMethod(scriptPath, methodName, argVector);
    }
    
    // Logging functions
    void LogInfo(const char* message)
    {
        if (message) Logger::Info("[Script] %s", message);
    }
    
    void LogWarning(const char* message)
    {
        if (message) Logger::Warn("[Script] %s", message);
    }
    
    void LogError(const char* message)
    {
        if (message) Logger::Error("[Script] %s", message);
    }
    
    void LogDebug(const char* message)
    {
        if (message) Logger::Debug("[Script] %s", message);
    }
    
    // Chat and communication
    void SendChatToPlayer(const char* playerId, const char* message)
    {
        if (!playerId || !message || !ScriptHost::s_gameServer) return;
        uint32_t clientId = static_cast<uint32_t>(std::strtoul(playerId, nullptr, 10));
        auto conn = ScriptHost::s_gameServer->GetClientConnection(clientId);
        if (conn) {
            conn->SendChatMessage(message);
        }
    }

    void BroadcastChat(const char* message)
    {
        if (!message || !ScriptHost::s_gameServer) return;
        ScriptHost::s_gameServer->BroadcastChatMessage(message);
    }

    void SendPrivateMessage(const char* /*fromPlayerId*/, const char* toPlayerId, const char* message)
    {
        if (!toPlayerId || !message || !ScriptHost::s_gameServer) return;
        uint32_t clientId = static_cast<uint32_t>(std::strtoul(toPlayerId, nullptr, 10));
        auto conn = ScriptHost::s_gameServer->GetClientConnection(clientId);
        if (conn) {
            conn->SendChatMessage(message);
        }
    }
    
    // Player management
    void KickPlayer(const char* playerId, const char* reason)
    {
        if (!playerId || !ScriptHost::s_gameServer) return;
        auto* admin = ScriptHost::GetAdminManager();
        if (admin) {
            admin->KickPlayer(0, playerId); // 0 = server/script-initiated
        }
        Logger::Info("[ScriptHost] KickPlayer: %s reason='%s'", playerId, reason ? reason : "");
    }

    void BanPlayer(const char* playerId, const char* reason, int durationHours)
    {
        if (!playerId || !ScriptHost::s_gameServer) return;
        auto* admin = ScriptHost::GetAdminManager();
        if (admin) {
            admin->BanPlayer(0, playerId, durationHours * 60);
        }
        Logger::Info("[ScriptHost] BanPlayer: %s for %dh reason='%s'", playerId, durationHours, reason ? reason : "");
    }

    void UnbanPlayer(const char* playerId)
    {
        if (!playerId) return;
        Logger::Info("[ScriptHost] UnbanPlayer: %s", playerId);
    }

    int GetPlayerAdminLevel(const char* playerId)
    {
        if (!playerId || !ScriptHost::s_gameServer) return 0;
        auto* admin = ScriptHost::GetAdminManager();
        if (admin && admin->IsAdmin(playerId)) return 1;
        return 0;
    }

    bool IsPlayerOnline(const char* playerId)
    {
        if (!playerId || !ScriptHost::s_gameServer) return false;
        uint32_t clientId = static_cast<uint32_t>(std::strtoul(playerId, nullptr, 10));
        auto* pm = ScriptHost::GetPlayerManager();
        return pm && pm->GetPlayer(clientId) != nullptr;
    }

    const char* GetPlayerName(const char* playerId)
    {
        if (!playerId || !ScriptHost::s_gameServer) return ScriptHost::StoreString("");
        uint32_t clientId = static_cast<uint32_t>(std::strtoul(playerId, nullptr, 10));
        auto conn = ScriptHost::s_gameServer->GetClientConnection(clientId);
        if (conn) return ScriptHost::StoreString(conn->GetPlayerName());
        return ScriptHost::StoreString("");
    }

    void GetPlayerPosition(const char* playerId, float* x, float* y, float* z)
    {
        if (!playerId || !ScriptHost::s_gameServer) { if (x) *x=0; if (y) *y=0; if (z) *z=0; return; }
        uint32_t clientId = static_cast<uint32_t>(std::strtoul(playerId, nullptr, 10));
        auto* pm = ScriptHost::GetPlayerManager();
        if (pm) {
            auto player = pm->GetPlayer(clientId);
            if (player) {
                Vector3 pos = player->GetPosition();
                if (x) *x = pos.x;
                if (y) *y = pos.y;
                if (z) *z = pos.z;
                return;
            }
        }
        if (x) *x=0; if (y) *y=0; if (z) *z=0;
    }

    void SetPlayerPosition(const char* playerId, float x, float y, float z)
    {
        if (!playerId || !ScriptHost::s_gameServer) return;
        uint32_t clientId = static_cast<uint32_t>(std::strtoul(playerId, nullptr, 10));
        auto* pm = ScriptHost::GetPlayerManager();
        if (pm) {
            auto player = pm->GetPlayer(clientId);
            if (player) player->SetPosition(Vector3(x, y, z));
        }
    }

    int GetPlayerTeam(const char* playerId)
    {
        if (!playerId || !ScriptHost::s_gameServer) return -1;
        uint32_t clientId = static_cast<uint32_t>(std::strtoul(playerId, nullptr, 10));
        auto* pm = ScriptHost::GetPlayerManager();
        if (pm) {
            auto player = pm->GetPlayer(clientId);
            if (player) return static_cast<int>(player->GetTeam());
        }
        return -1;
    }

    void SetPlayerTeam(const char* playerId, int teamId)
    {
        if (!playerId || !ScriptHost::s_gameServer) return;
        uint32_t clientId = static_cast<uint32_t>(std::strtoul(playerId, nullptr, 10));
        auto* pm = ScriptHost::GetPlayerManager();
        if (pm) {
            auto player = pm->GetPlayer(clientId);
            if (player) player->SetTeam(static_cast<uint32_t>(teamId));
        }
    }

    int GetPlayerHealth(const char* playerId)
    {
        if (!playerId || !ScriptHost::s_gameServer) return 0;
        uint32_t clientId = static_cast<uint32_t>(std::strtoul(playerId, nullptr, 10));
        auto* pm = ScriptHost::GetPlayerManager();
        if (pm) {
            auto player = pm->GetPlayer(clientId);
            if (player) return player->GetHealth();
        }
        return 0;
    }

    void SetPlayerHealth(const char* playerId, int health)
    {
        if (!playerId || !ScriptHost::s_gameServer) return;
        uint32_t clientId = static_cast<uint32_t>(std::strtoul(playerId, nullptr, 10));
        auto* pm = ScriptHost::GetPlayerManager();
        if (pm) {
            auto player = pm->GetPlayer(clientId);
            if (player) player->SetHealth(health);
        }
    }
    
    // Server information
    const char* GetDataDirectory()
    {
        if (ScriptHost::s_gameServer) {
            auto cfg = ScriptHost::s_gameServer->GetServerConfig();
            if (cfg) return ScriptHost::StoreString(cfg->GetDataDirectory());
        }
        return ScriptHost::StoreString("data/");
    }

    const char* GetServerName()
    {
        if (ScriptHost::s_gameServer) {
            auto cfg = ScriptHost::s_gameServer->GetServerConfig();
            if (cfg) return ScriptHost::StoreString(cfg->GetServerName());
        }
        return ScriptHost::StoreString("RS2V Server");
    }

    int GetPlayerCount()
    {
        auto* pm = ScriptHost::GetPlayerManager();
        if (pm) return static_cast<int>(pm->GetAllPlayers().size());
        return 0;
    }

    int GetMaxPlayers()
    {
        if (ScriptHost::s_gameServer) {
            auto cfg = ScriptHost::s_gameServer->GetServerConfig();
            if (cfg) return cfg->GetMaxPlayers();
        }
        return 64;
    }

    int GetCurrentTickRate()
    {
        if (ScriptHost::s_gameServer) {
            auto cfg = ScriptHost::s_gameServer->GetServerConfig();
            if (cfg) return cfg->GetTickRate();
        }
        return 60;
    }

    int GetScriptReloadCount()
    {
        if (!ScriptHost::s_scriptManager) return 0;
        return ScriptHost::s_scriptManager->GetReloadCount();
    }

    const char* GetCurrentMap()
    {
        if (ScriptHost::s_gameServer) {
            auto* mm = ScriptHost::s_gameServer->GetMapManager();
            if (mm) {
                auto objs = mm->GetMapObjectives(); // Bounds check
                // MapManager stores current map name indirectly via bounds/objectives
            }
            auto cfg = ScriptHost::s_gameServer->GetGameConfig();
            if (cfg) return ScriptHost::StoreString(cfg->GetGameSettings().mapName);
        }
        return ScriptHost::StoreString("unknown");
    }

    const char* GetCurrentGameMode()
    {
        if (ScriptHost::s_gameServer) {
            auto cfg = ScriptHost::s_gameServer->GetGameConfig();
            if (cfg) return ScriptHost::StoreString(cfg->GetGameSettings().gameMode);
        }
        return ScriptHost::StoreString("unknown");
    }

    long GetServerUptime()
    {
        static auto startTime = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count();
    }

    const char* GetServerVersion() { return ScriptHost::StoreString("1.0.0"); }

    // Player lists
    int GetConnectedPlayerCount()
    {
        auto* pm = ScriptHost::GetPlayerManager();
        if (pm) return static_cast<int>(pm->GetAllPlayers().size());
        return 0;
    }

    const char* GetConnectedPlayerAt(int index)
    {
        auto* pm = ScriptHost::GetPlayerManager();
        if (pm) {
            auto players = pm->GetAllPlayers();
            if (index >= 0 && index < static_cast<int>(players.size())) {
                return ScriptHost::StoreString(
                    std::to_string(players[index]->GetConnection()->GetClientId()));
            }
        }
        return ScriptHost::StoreString("");
    }

    int GetPlayerCountPerTeam(int teamId)
    {
        auto* pm = ScriptHost::GetPlayerManager();
        if (!pm) return 0;
        int count = 0;
        for (auto& player : pm->GetAllPlayers()) {
            if (static_cast<int>(player->GetTeam()) == teamId) count++;
        }
        return count;
    }
    
    // Team management
    int GetTeamCount() { return 2; }

    const char* GetTeamName(int teamId)
    {
        if (ScriptHost::s_gameServer) {
            auto* tm = ScriptHost::s_gameServer->GetTeamManager();
            if (tm) return ScriptHost::StoreString(tm->GetTeamName(static_cast<uint32_t>(teamId)));
        }
        return ScriptHost::StoreString(teamId == 1 ? "North Vietnam" : "South Vietnam");
    }

    void SetTeamSize(int teamId, int maxSize)
    {
        if (!ScriptHost::s_gameServer) return;
        auto* tm = ScriptHost::s_gameServer->GetTeamManager();
        if (tm) tm->SetTeamMaxSize(static_cast<uint32_t>(teamId), maxSize);
    }

    int GetTeamSize(int teamId)
    {
        if (!ScriptHost::s_gameServer) return 32;
        auto* tm = ScriptHost::s_gameServer->GetTeamManager();
        if (tm) return tm->GetTeamSize(static_cast<uint32_t>(teamId));
        return 32;
    }

    int GetTeamScore(int teamId)
    {
        if (!ScriptHost::s_gameServer) return 0;
        auto* tm = ScriptHost::s_gameServer->GetTeamManager();
        if (tm) return tm->GetTeamScore(static_cast<uint32_t>(teamId));
        return 0;
    }

    void SetTeamScore(int teamId, int score)
    {
        if (!ScriptHost::s_gameServer) return;
        auto* tm = ScriptHost::s_gameServer->GetTeamManager();
        if (tm) tm->SetTeamScore(static_cast<uint32_t>(teamId), score);
    }
    
    // Entity management -- stubbed (EntityManager not integrated)
    bool SpawnEntity(const char* /*className*/, float /*x*/, float /*y*/, float /*z*/) { return false; }
    bool SpawnEntityWithId(const char* /*className*/, float /*x*/, float /*y*/, float /*z*/, int* /*outEntityId*/) { return false; }
    void RemoveEntity(int /*entityId*/) {}
    bool IsEntityValid(int /*entityId*/) { return false; }
    void MoveEntityTo(int /*entityId*/, float /*x*/, float /*y*/, float /*z*/) {}
    void GetEntityPosition(int /*entityId*/, float* x, float* y, float* z) { if (x) *x=0; if (y) *y=0; if (z) *z=0; }
    float GetEntityHealth(int /*entityId*/) { return 0.0f; }
    void SetEntityHealth(int /*entityId*/, float /*health*/) {}
    const char* GetEntityClass(int /*entityId*/) { return ScriptHost::StoreString(""); }
    int GetEntityCount() { return 0; }
    int GetEntityCountByClass(const char* /*className*/) { return 0; }
    
    // Configuration management
    void SetConfigInt(const char* key, int value)
    {
        if (!key || !ScriptHost::s_configManager) return;
        ScriptHost::s_configManager->SetInt(key, value);
    }

    int GetConfigInt(const char* key, int defaultValue)
    {
        if (!key || !ScriptHost::s_configManager) return defaultValue;
        return ScriptHost::s_configManager->GetInt(key, defaultValue);
    }

    void SetConfigFloat(const char* key, float value)
    {
        if (!key || !ScriptHost::s_configManager) return;
        ScriptHost::s_configManager->SetFloat(key, value);
    }

    float GetConfigFloat(const char* key, float defaultValue)
    {
        if (!key || !ScriptHost::s_configManager) return defaultValue;
        return ScriptHost::s_configManager->GetFloat(key, defaultValue);
    }

    void SetConfigBool(const char* key, bool value)
    {
        if (!key || !ScriptHost::s_configManager) return;
        ScriptHost::s_configManager->SetBool(key, value);
    }

    bool GetConfigBool(const char* key, bool defaultValue)
    {
        if (!key || !ScriptHost::s_configManager) return defaultValue;
        return ScriptHost::s_configManager->GetBool(key, defaultValue);
    }

    void SetConfigString(const char* key, const char* value)
    {
        if (!key || !value || !ScriptHost::s_configManager) return;
        ScriptHost::s_configManager->SetString(key, value);
    }

    const char* GetConfigString(const char* key, const char* defaultValue)
    {
        if (!key || !ScriptHost::s_configManager) return ScriptHost::StoreString(defaultValue ? defaultValue : "");
        return ScriptHost::StoreString(ScriptHost::s_configManager->GetString(key, defaultValue ? defaultValue : ""));
    }

    void ReloadConfig()
    {
        if (ScriptHost::s_configManager) {
            ScriptHost::s_configManager->ReloadConfiguration();
            Logger::Info("[ScriptHost] Configuration reloaded");
        }
    }

    bool SaveConfig()
    {
        if (ScriptHost::s_configManager) {
            return ScriptHost::s_configManager->SaveAllConfigurations();
        }
        return false;
    }
    
    // Scheduling and timing
    void ScheduleCallback(float delaySeconds, const char* methodName)
    {
        if (!methodName || delaySeconds < 0) return;
        
        std::lock_guard<std::mutex> lock(ScriptHost::s_callbackMutex);
        
        ScriptHost::ScheduledCallback callback;
        callback.delaySeconds = delaySeconds;
        callback.methodName = methodName;
        callback.scheduledTime = std::chrono::steady_clock::now() + 
                                std::chrono::milliseconds(static_cast<int>(delaySeconds * 1000));
        callback.executed = false;
        
        ScriptHost::s_scheduledCallbacks.push_back(callback);
    }
    
    void CancelScheduledCallbacks(const char* methodName)
    {
        if (!methodName) return;
        
        std::lock_guard<std::mutex> lock(ScriptHost::s_callbackMutex);
        
        ScriptHost::s_scheduledCallbacks.erase(
            std::remove_if(ScriptHost::s_scheduledCallbacks.begin(), 
                          ScriptHost::s_scheduledCallbacks.end(),
                          [methodName](const ScriptHost::ScheduledCallback& cb) {
                              return cb.methodName == methodName;
                          }),
            ScriptHost::s_scheduledCallbacks.end()
        );
    }
    
    void ProcessScheduledCallbacks()
    {
        std::lock_guard<std::mutex> lock(ScriptHost::s_callbackMutex);
        
        auto now = std::chrono::steady_clock::now();
        
        for (auto& callback : ScriptHost::s_scheduledCallbacks) {
            if (!callback.executed && now >= callback.scheduledTime) {
                if (ScriptHost::s_scriptManager) {
                    ScriptHost::s_scriptManager->ExecuteScriptMethod("__Global__", callback.methodName, {});
                }
                callback.executed = true;
            }
        }
        
        // Remove executed callbacks
        ScriptHost::s_scheduledCallbacks.erase(
            std::remove_if(ScriptHost::s_scheduledCallbacks.begin(), 
                          ScriptHost::s_scheduledCallbacks.end(),
                          [](const ScriptHost::ScheduledCallback& cb) {
                              return cb.executed;
                          }),
            ScriptHost::s_scheduledCallbacks.end()
        );
    }
    
    long GetCurrentTimeMillis()
    {
        auto now = std::chrono::system_clock::now();
        auto duration = now.time_since_epoch();
        return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    }
    
    const char* GetCurrentTimeString()
    {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto tm = *std::localtime(&time_t);
        
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
        return ScriptHost::StoreString(oss.str());
    }
    
    // Debug drawing - broadcast debug shapes to connected clients via network
    void DebugDrawLine(float x1, float y1, float z1, float x2, float y2, float z2,
                      float duration, float thickness, float r, float g, float b)
    {
        if (!ScriptHost::s_gameServer) return;
        auto* nm = ScriptHost::s_gameServer->GetNetworkManager();
        if (!nm) return;
        Packet pkt("DEBUG_DRAW_LINE");
        pkt.WriteFloat(x1); pkt.WriteFloat(y1); pkt.WriteFloat(z1);
        pkt.WriteFloat(x2); pkt.WriteFloat(y2); pkt.WriteFloat(z2);
        pkt.WriteFloat(duration); pkt.WriteFloat(thickness);
        pkt.WriteFloat(r); pkt.WriteFloat(g); pkt.WriteFloat(b);
        nm->BroadcastPacket(pkt);
    }

    void DebugDrawSphere(float x, float y, float z, float radius, float duration,
                        float r, float g, float b)
    {
        if (!ScriptHost::s_gameServer) return;
        auto* nm = ScriptHost::s_gameServer->GetNetworkManager();
        if (!nm) return;
        Packet pkt("DEBUG_DRAW_SPHERE");
        pkt.WriteFloat(x); pkt.WriteFloat(y); pkt.WriteFloat(z);
        pkt.WriteFloat(radius); pkt.WriteFloat(duration);
        pkt.WriteFloat(r); pkt.WriteFloat(g); pkt.WriteFloat(b);
        nm->BroadcastPacket(pkt);
    }

    void DebugDrawBox(float x, float y, float z, float sizeX, float sizeY, float sizeZ,
                     float duration, float r, float g, float b)
    {
        if (!ScriptHost::s_gameServer) return;
        auto* nm = ScriptHost::s_gameServer->GetNetworkManager();
        if (!nm) return;
        Packet pkt("DEBUG_DRAW_BOX");
        pkt.WriteFloat(x); pkt.WriteFloat(y); pkt.WriteFloat(z);
        pkt.WriteFloat(sizeX); pkt.WriteFloat(sizeY); pkt.WriteFloat(sizeZ);
        pkt.WriteFloat(duration);
        pkt.WriteFloat(r); pkt.WriteFloat(g); pkt.WriteFloat(b);
        nm->BroadcastPacket(pkt);
    }

    void DebugDrawArrow(float x1, float y1, float z1, float x2, float y2, float z2,
                       float duration, float thickness, float r, float g, float b)
    {
        if (!ScriptHost::s_gameServer) return;
        auto* nm = ScriptHost::s_gameServer->GetNetworkManager();
        if (!nm) return;
        Packet pkt("DEBUG_DRAW_ARROW");
        pkt.WriteFloat(x1); pkt.WriteFloat(y1); pkt.WriteFloat(z1);
        pkt.WriteFloat(x2); pkt.WriteFloat(y2); pkt.WriteFloat(z2);
        pkt.WriteFloat(duration); pkt.WriteFloat(thickness);
        pkt.WriteFloat(r); pkt.WriteFloat(g); pkt.WriteFloat(b);
        nm->BroadcastPacket(pkt);
    }

    void DebugDrawText(float x, float y, float z, const char* text, float duration,
                      float r, float g, float b)
    {
        if (!ScriptHost::s_gameServer || !text) return;
        auto* nm = ScriptHost::s_gameServer->GetNetworkManager();
        if (!nm) return;
        Packet pkt("DEBUG_DRAW_TEXT");
        pkt.WriteFloat(x); pkt.WriteFloat(y); pkt.WriteFloat(z);
        pkt.WriteString(text); pkt.WriteFloat(duration);
        pkt.WriteFloat(r); pkt.WriteFloat(g); pkt.WriteFloat(b);
        nm->BroadcastPacket(pkt);
    }

    void ClearDebugDrawings()
    {
        if (!ScriptHost::s_gameServer) return;
        auto* nm = ScriptHost::s_gameServer->GetNetworkManager();
        if (!nm) return;
        Packet pkt("DEBUG_DRAW_CLEAR");
        nm->BroadcastPacket(pkt);
    }
    
    // Script management
    bool ReloadScript(const char* scriptPath)
    {
        if (!scriptPath || !ScriptHost::s_scriptManager) return false;
        return ScriptHost::s_scriptManager->ReloadScript(scriptPath);
    }

    bool IsScriptLoaded(const char* scriptPath)
    {
        if (!scriptPath || !ScriptHost::s_scriptManager) return false;
        return ScriptHost::s_scriptManager->IsScriptLoaded(scriptPath);
    }

    int GetLoadedScriptCount()
    {
        if (!ScriptHost::s_scriptManager) return 0;
        return static_cast<int>(ScriptHost::s_scriptManager->GetLoadedScripts().size());
    }

    const char* GetLoadedScriptAt(int index)
    {
        if (!ScriptHost::s_scriptManager) return ScriptHost::StoreString("");
        auto scripts = ScriptHost::s_scriptManager->GetLoadedScripts();
        if (index >= 0 && index < static_cast<int>(scripts.size())) {
            return ScriptHost::StoreString(scripts[index]);
        }
        return ScriptHost::StoreString("");
    }
    
    bool EnableScript(const char* scriptName)
    {
        if (!scriptName) return false;
        
        auto dataDir = GetDataDirectory();
        std::string enabledPath = std::string(dataDir) + "/scripts/enabled/" + scriptName;
        std::string disabledPath = std::string(dataDir) + "/scripts/disabled/" + scriptName;
        
        if (!FileExists(disabledPath.c_str())) return false;
        
        try {
            std::filesystem::rename(disabledPath, enabledPath);
            return true;
        } catch (const std::exception& e) {
            Logger::Error("Failed to enable script %s: %s", scriptName, e.what());
            return false;
        }
    }
    
    bool DisableScript(const char* scriptName)
    {
        if (!scriptName) return false;
        
        auto dataDir = GetDataDirectory();
        std::string enabledPath = std::string(dataDir) + "/scripts/enabled/" + scriptName;
        std::string disabledPath = std::string(dataDir) + "/scripts/disabled/" + scriptName;
        
        if (!FileExists(enabledPath.c_str())) return false;
        
        try {
            std::filesystem::rename(enabledPath, disabledPath);
            return true;
        } catch (const std::exception& e) {
            Logger::Error("Failed to disable script %s: %s", scriptName, e.what());
            return false;
        }
    }
    
    // Game state management
    void StartMatch()
    {
        if (!ScriptHost::s_gameServer) return;
        Logger::Info("[ScriptHost] StartMatch requested via script");
        ScriptHost::s_gameServer->BroadcastChatMessage("[Server] Match started.");
    }

    void EndMatch()
    {
        if (!ScriptHost::s_gameServer) return;
        Logger::Info("[ScriptHost] EndMatch requested via script");
        ScriptHost::s_gameServer->BroadcastChatMessage("[Server] Match ended.");
    }

    void PauseMatch()
    {
        if (!ScriptHost::s_gameServer) return;
        Logger::Info("[ScriptHost] PauseMatch requested via script");
        ScriptHost::s_gameServer->BroadcastChatMessage("[Server] Match paused.");
    }

    void ResumeMatch()
    {
        if (!ScriptHost::s_gameServer) return;
        Logger::Info("[ScriptHost] ResumeMatch requested via script");
        ScriptHost::s_gameServer->BroadcastChatMessage("[Server] Match resumed.");
    }

    bool IsMatchActive()
    {
        // Match is active when the server is running
        return ScriptHost::s_gameServer != nullptr;
    }

    bool IsMatchPaused()
    {
        // Not paused if server exists (no formal pause state yet)
        return false;
    }

    int GetMatchTimeRemaining()
    {
        if (!ScriptHost::s_gameServer) return 0;
        auto cfg = ScriptHost::s_gameServer->GetGameConfig();
        if (cfg) return cfg->GetGameSettings().roundTimeLimit;
        return 0;
    }

    void SetMatchTimeLimit(int seconds)
    {
        Logger::Info("[ScriptHost] SetMatchTimeLimit: %d seconds", seconds);
    }

    void ChangeMap(const char* mapName)
    {
        if (!mapName || !ScriptHost::s_gameServer) return;
        Logger::Info("[ScriptHost] ChangeMap requested: %s", mapName);
        auto* mm = ScriptHost::s_gameServer->GetMapManager();
        if (mm) {
            mm->LoadMap(mapName);
        }
    }

    void ChangeGameMode(const char* gameMode)
    {
        if (!gameMode) return;
        Logger::Info("[ScriptHost] ChangeGameMode requested: %s", gameMode);
    }
    
    // Network and performance
    int GetAveragePlayerPing()
    {
        if (!ScriptHost::s_gameServer) return 0;
        auto connections = ScriptHost::s_gameServer->GetAllConnections();
        if (connections.empty()) return 0;
        int totalPing = 0;
        for (auto& conn : connections) {
            totalPing += conn->GetPing();
        }
        return totalPing / static_cast<int>(connections.size());
    }

    int GetPlayerPing(const char* playerId)
    {
        if (!playerId || !ScriptHost::s_gameServer) return 0;
        uint32_t clientId = static_cast<uint32_t>(std::strtoul(playerId, nullptr, 10));
        auto conn = ScriptHost::s_gameServer->GetClientConnection(clientId);
        if (conn) return conn->GetPing();
        return 0;
    }

    float GetServerCpuUsage() { return 0.0f; } // Platform-specific, not portably obtainable
    long GetServerMemoryUsage() { return 0; }   // Platform-specific

    int GetNetworkPacketsPerSecond()
    {
        if (!ScriptHost::s_gameServer) return 0;
        auto* nm = ScriptHost::s_gameServer->GetNetworkManager();
        if (nm) return nm->GetPacketsPerSecond();
        return 0;
    }

    float GetServerFrameRate()
    {
        if (ScriptHost::s_gameServer) {
            auto cfg = ScriptHost::s_gameServer->GetServerConfig();
            if (cfg) return static_cast<float>(cfg->GetTickRate());
        }
        return 60.0f;
    }
    
    // File and data management
    bool FileExists(const char* path)
    {
        if (!path) return false;
        std::ifstream file(path);
        return file.good();
    }
    
    bool WriteFile(const char* path, const char* content)
    {
        if (!path || !content) return false;
        
        try {
            std::ofstream file(path);
            if (!file.is_open()) return false;
            
            file << content;
            file.close();
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }
    
    const char* ReadFile(const char* path)
    {
        if (!path || !FileExists(path)) {
            return ScriptHost::StoreString("");
        }
        
        try {
            std::ifstream file(path);
            if (!file.is_open()) return ScriptHost::StoreString("");
            
            std::stringstream buffer;
            buffer << file.rdbuf();
            file.close();
            
            return ScriptHost::StoreString(buffer.str());
        } catch (const std::exception&) {
            return ScriptHost::StoreString("");
        }
    }
    
    bool DeleteFile(const char* path)
    {
        if (!path) return false;
        
        try {
            return std::filesystem::remove(path);
        } catch (const std::exception&) {
            return false;
        }
    }
    
    bool CreateDirectory(const char* path)
    {
        if (!path) return false;
        
        try {
            return std::filesystem::create_directories(path);
        } catch (const std::exception&) {
            return false;
        }
    }
    
    long GetFileSize(const char* path)
    {
        if (!path || !FileExists(path)) return -1;
        
        try {
            return static_cast<long>(std::filesystem::file_size(path));
        } catch (const std::exception&) {
            return -1;
        }
    }
    
    long GetFileModificationTime(const char* path)
    {
        if (!path || !FileExists(path)) return 0;
        
        try {
            auto ftime = std::filesystem::last_write_time(path);
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
            );
            return std::chrono::duration_cast<std::chrono::seconds>(sctp.time_since_epoch()).count();
        } catch (const std::exception&) {
            return 0;
        }
    }
    
    // Weapon/inventory and statistics
    void GivePlayerWeapon(const char* playerId, const char* weaponClass)
    {
        if (!playerId || !weaponClass || !ScriptHost::s_gameServer) return;
        uint32_t clientId = static_cast<uint32_t>(std::strtoul(playerId, nullptr, 10));
        auto* pm = ScriptHost::GetPlayerManager();
        if (pm) {
            auto player = pm->GetPlayer(clientId);
            if (player) player->AddItem(weaponClass, 1);
        }
    }

    void RemovePlayerWeapon(const char* playerId, const char* weaponClass)
    {
        if (!playerId || !weaponClass || !ScriptHost::s_gameServer) return;
        uint32_t clientId = static_cast<uint32_t>(std::strtoul(playerId, nullptr, 10));
        auto* pm = ScriptHost::GetPlayerManager();
        if (pm) {
            auto player = pm->GetPlayer(clientId);
            if (player) player->RemoveItem(weaponClass, 1);
        }
    }

    bool PlayerHasWeapon(const char* playerId, const char* weaponClass)
    {
        if (!playerId || !weaponClass || !ScriptHost::s_gameServer) return false;
        uint32_t clientId = static_cast<uint32_t>(std::strtoul(playerId, nullptr, 10));
        auto* pm = ScriptHost::GetPlayerManager();
        if (pm) {
            auto player = pm->GetPlayer(clientId);
            if (player) {
                for (auto& item : player->GetInventory()) {
                    if (item.name == weaponClass) return true;
                }
            }
        }
        return false;
    }

    const char* GetPlayerPrimaryWeapon(const char* playerId)
    {
        if (!playerId || !ScriptHost::s_gameServer) return ScriptHost::StoreString("");
        uint32_t clientId = static_cast<uint32_t>(std::strtoul(playerId, nullptr, 10));
        auto* pm = ScriptHost::GetPlayerManager();
        if (pm) {
            auto player = pm->GetPlayer(clientId);
            if (player) {
                auto& inv = player->GetInventory();
                if (!inv.empty()) return ScriptHost::StoreString(inv[0].name);
            }
        }
        return ScriptHost::StoreString("");
    }

    void SetPlayerAmmo(const char* playerId, const char* weaponClass, int ammo)
    {
        if (!playerId || !weaponClass || !ScriptHost::s_gameServer) return;
        uint32_t clientId = static_cast<uint32_t>(std::strtoul(playerId, nullptr, 10));
        auto* pm = ScriptHost::GetPlayerManager();
        if (pm) {
            auto player = pm->GetPlayer(clientId);
            if (player) {
                // Update ammo by removing and re-adding with new quantity
                player->RemoveItem(weaponClass, 9999);
                if (ammo > 0) player->AddItem(weaponClass, ammo);
            }
        }
    }

    int GetPlayerAmmo(const char* playerId, const char* weaponClass)
    {
        if (!playerId || !weaponClass || !ScriptHost::s_gameServer) return 0;
        uint32_t clientId = static_cast<uint32_t>(std::strtoul(playerId, nullptr, 10));
        auto* pm = ScriptHost::GetPlayerManager();
        if (pm) {
            auto player = pm->GetPlayer(clientId);
            if (player) {
                for (auto& item : player->GetInventory()) {
                    if (item.name == weaponClass) return item.quantity;
                }
            }
        }
        return 0;
    }

    int GetPlayerKills(const char* playerId)
    {
        if (!playerId || !ScriptHost::s_gameServer) return 0;
        uint32_t clientId = static_cast<uint32_t>(std::strtoul(playerId, nullptr, 10));
        auto* pm = ScriptHost::GetPlayerManager();
        if (pm) return pm->GetPlayerKills(clientId);
        return 0;
    }

    int GetPlayerDeaths(const char* playerId)
    {
        if (!playerId || !ScriptHost::s_gameServer) return 0;
        uint32_t clientId = static_cast<uint32_t>(std::strtoul(playerId, nullptr, 10));
        auto* pm = ScriptHost::GetPlayerManager();
        if (pm) return pm->GetPlayerDeaths(clientId);
        return 0;
    }

    void SetPlayerKills(const char* playerId, int kills)
    {
        if (!playerId || !ScriptHost::s_gameServer) return;
        uint32_t clientId = static_cast<uint32_t>(std::strtoul(playerId, nullptr, 10));
        auto* pm = ScriptHost::GetPlayerManager();
        if (pm) pm->SetPlayerKills(clientId, kills);
    }

    void SetPlayerDeaths(const char* playerId, int deaths)
    {
        if (!playerId || !ScriptHost::s_gameServer) return;
        uint32_t clientId = static_cast<uint32_t>(std::strtoul(playerId, nullptr, 10));
        auto* pm = ScriptHost::GetPlayerManager();
        if (pm) pm->SetPlayerDeaths(clientId, deaths);
    }

    void AddPlayerKill(const char* playerId)
    {
        if (!playerId || !ScriptHost::s_gameServer) return;
        uint32_t clientId = static_cast<uint32_t>(std::strtoul(playerId, nullptr, 10));
        auto* pm = ScriptHost::GetPlayerManager();
        if (pm) pm->AddPlayerKill(clientId);
    }

    void AddPlayerDeath(const char* playerId)
    {
        if (!playerId || !ScriptHost::s_gameServer) return;
        uint32_t clientId = static_cast<uint32_t>(std::strtoul(playerId, nullptr, 10));
        auto* pm = ScriptHost::GetPlayerManager();
        if (pm) pm->AddPlayerDeath(clientId);
    }

    int GetPlayerScore(const char* playerId)
    {
        if (!playerId || !ScriptHost::s_gameServer) return 0;
        uint32_t clientId = static_cast<uint32_t>(std::strtoul(playerId, nullptr, 10));
        auto* pm = ScriptHost::GetPlayerManager();
        if (pm) return pm->GetPlayerScore(clientId);
        return 0;
    }

    void SetPlayerScore(const char* playerId, int score)
    {
        if (!playerId || !ScriptHost::s_gameServer) return;
        uint32_t clientId = static_cast<uint32_t>(std::strtoul(playerId, nullptr, 10));
        auto* pm = ScriptHost::GetPlayerManager();
        if (pm) pm->SetPlayerScore(clientId, score);
    }

    void AddPlayerScore(const char* playerId, int points)
    {
        if (!playerId || !ScriptHost::s_gameServer) return;
        uint32_t clientId = static_cast<uint32_t>(std::strtoul(playerId, nullptr, 10));
        auto* pm = ScriptHost::GetPlayerManager();
        if (pm) pm->AddPlayerScore(clientId, points);
    }
    
    // EAC Memory Operations (NEW IMPLEMENTATIONS)
    bool RemoteRead(uint32_t clientId, uint64_t address, uint32_t length)
    {
        auto eac = ScriptHost::GetEACServer();
        if (!eac) {
            Logger::Error("RemoteRead: EAC server not available");
            return false;
        }
        
        return eac->RequestMemoryRead(clientId, address, length,
            [clientId, address, length](const uint8_t* data, uint32_t len) {
                if (ScriptHost::s_scriptManager) {
                    ScriptHost::s_scriptManager->OnRemoteMemoryRead(clientId, address, data, len);
                }
            });
    }
    
    bool RemoteWrite(uint32_t clientId, uint64_t address, const uint8_t* buf, uint32_t length)
    {
        auto eac = ScriptHost::GetEACServer();
        if (!eac || !buf) {
            Logger::Error("RemoteWrite: EAC server not available or invalid buffer");
            return false;
        }
        
        return eac->RequestMemoryWrite(clientId, address, buf, length,
            [clientId](bool success) {
                if (ScriptHost::s_scriptManager) {
                    ScriptHost::s_scriptManager->OnRemoteMemoryWriteAck(clientId, success);
                }
            });
    }
    
    bool RemoteAlloc(uint32_t clientId, uint32_t length, uint64_t protect)
    {
        auto eac = ScriptHost::GetEACServer();
        if (!eac) {
            Logger::Error("RemoteAlloc: EAC server not available");
            return false;
        }
        
        return eac->RequestMemoryAlloc(clientId, length, protect,
            [clientId](uint64_t baseAddr) {
                if (ScriptHost::s_scriptManager) {
                    ScriptHost::s_scriptManager->OnRemoteMemoryAlloc(clientId, baseAddr);
                }
            });
    }
    
    void BroadcastRemoteRead(uint64_t address, uint32_t length)
    {
        auto eac = ScriptHost::GetEACServer();
        if (!eac) {
            Logger::Error("BroadcastRemoteRead: EAC server not available");
            return;
        }
        
        eac->BroadcastMemoryRead(address, length,
            [](uint32_t clientId, const uint8_t* data, uint32_t len) {
                if (ScriptHost::s_scriptManager) {
                    ScriptHost::s_scriptManager->OnBroadcastMemoryRead(clientId, data, len);
                }
            });
    }
    
    uint32_t GetConnectedClientId(int index)
    {
        auto eac = ScriptHost::GetEACServer();
        if (!eac) return 0;
        
        // This would need to be implemented in EACServerEmulator
        // For now, return a placeholder
        return static_cast<uint32_t>(index + 1);
    }
    
    int GetConnectedClientCount()
    {
        auto eac = ScriptHost::GetEACServer();
        if (!eac) return 0;
        
        // This would need to be implemented in EACServerEmulator
        // For now, return the player count as a proxy
        return GetPlayerCount();
    }
    
    // Memory management
    void FreeString(const char* str)
    {
        // Strings are managed internally by ScriptHost, no action needed
        // This function exists for API completeness
    }
}