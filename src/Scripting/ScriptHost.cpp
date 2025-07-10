// src/Scripting/ScriptHost.cpp

#include "Scripting/ScriptHost.h"
#include "Scripting/ScriptManager.h"
#include "Config/ConfigManager.h"
#include "Game/GameServer.h"
#include "Game/PlayerManager.h"
#include "Game/EntityManager.h"
#include "Network/NetworkManager.h"
#include "Admin/AdminManager.h"
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
    if (!s_gameServer) return nullptr;
    return s_gameServer->GetEntityManager();
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
        auto playerMgr = ScriptHost::GetPlayerManager();
        if (playerMgr && playerId && message) {
            playerMgr->SendChatMessage(playerId, message);
        }
    }
    
    void BroadcastChat(const char* message)
    {
        auto playerMgr = ScriptHost::GetPlayerManager();
        if (playerMgr && message) {
            playerMgr->BroadcastMessage(message);
        }
    }
    
    void SendPrivateMessage(const char* fromPlayerId, const char* toPlayerId, const char* message)
    {
        auto playerMgr = ScriptHost::GetPlayerManager();
        if (playerMgr && fromPlayerId && toPlayerId && message) {
            playerMgr->SendPrivateMessage(fromPlayerId, toPlayerId, message);
        }
    }
    
    // Player management
    void KickPlayer(const char* playerId, const char* reason)
    {
        auto playerMgr = ScriptHost::GetPlayerManager();
        if (playerMgr && playerId) {
            std::string reasonStr = reason ? reason : "Kicked by script";
            playerMgr->KickPlayer(playerId, reasonStr);
        }
    }
    
    void BanPlayer(const char* playerId, const char* reason, int durationHours)
    {
        auto adminMgr = ScriptHost::GetAdminManager();
        if (adminMgr && playerId) {
            std::string reasonStr = reason ? reason : "Banned by script";
            adminMgr->BanPlayer(playerId, reasonStr, durationHours);
        }
    }
    
    void UnbanPlayer(const char* playerId)
    {
        auto adminMgr = ScriptHost::GetAdminManager();
        if (adminMgr && playerId) {
            adminMgr->UnbanPlayer(playerId);
        }
    }
    
    int GetPlayerAdminLevel(const char* playerId)
    {
        auto adminMgr = ScriptHost::GetAdminManager();
        if (!adminMgr || !playerId) return 0;
        return adminMgr->GetPlayerAdminLevel(playerId);
    }
    
    bool IsPlayerOnline(const char* playerId)
    {
        auto playerMgr = ScriptHost::GetPlayerManager();
        if (!playerMgr || !playerId) return false;
        return playerMgr->IsPlayerConnected(playerId);
    }
    
    const char* GetPlayerName(const char* playerId)
    {
        auto playerMgr = ScriptHost::GetPlayerManager();
        if (!playerMgr || !playerId) return ScriptHost::StoreString("");
        
        auto name = playerMgr->GetPlayerName(playerId);
        return ScriptHost::StoreString(name);
    }
    
    void GetPlayerPosition(const char* playerId, float* x, float* y, float* z)
    {
        auto playerMgr = ScriptHost::GetPlayerManager();
        if (!playerMgr || !playerId || !x || !y || !z) return;
        
        auto pos = playerMgr->GetPlayerPosition(playerId);
        *x = pos.x;
        *y = pos.y;
        *z = pos.z;
    }
    
    void SetPlayerPosition(const char* playerId, float x, float y, float z)
    {
        auto playerMgr = ScriptHost::GetPlayerManager();
        if (playerMgr && playerId) {
            playerMgr->SetPlayerPosition(playerId, {x, y, z});
        }
    }
    
    int GetPlayerTeam(const char* playerId)
    {
        auto playerMgr = ScriptHost::GetPlayerManager();
        if (!playerMgr || !playerId) return -1;
        return playerMgr->GetPlayerTeam(playerId);
    }
    
    void SetPlayerTeam(const char* playerId, int teamId)
    {
        auto playerMgr = ScriptHost::GetPlayerManager();
        if (playerMgr && playerId) {
            playerMgr->SetPlayerTeam(playerId, teamId);
        }
    }
    
    int GetPlayerHealth(const char* playerId)
    {
        auto playerMgr = ScriptHost::GetPlayerManager();
        if (!playerMgr || !playerId) return 0;
        return playerMgr->GetPlayerHealth(playerId);
    }
    
    void SetPlayerHealth(const char* playerId, int health)
    {
        auto playerMgr = ScriptHost::GetPlayerManager();
        if (playerMgr && playerId) {
            playerMgr->SetPlayerHealth(playerId, health);
        }
    }
    
    // Server information
    const char* GetDataDirectory()
    {
        if (!ScriptHost::s_configManager) return ScriptHost::StoreString("data/");
        
        auto dataDir = ScriptHost::s_configManager->GetString("General.data_directory", "data/");
        return ScriptHost::StoreString(dataDir);
    }
    
    const char* GetServerName()
    {
        if (!ScriptHost::s_configManager) return ScriptHost::StoreString("RS2V Server");
        
        auto name = ScriptHost::s_configManager->GetString("General.server_name", "RS2V Server");
        return ScriptHost::StoreString(name);
    }
    
    int GetPlayerCount()
    {
        auto playerMgr = ScriptHost::GetPlayerManager();
        return playerMgr ? playerMgr->GetConnectedPlayerCount() : 0;
    }
    
    int GetMaxPlayers()
    {
        if (!ScriptHost::s_configManager) return 64;
        return ScriptHost::s_configManager->GetInt("General.max_players", 64);
    }
    
    int GetCurrentTickRate()
    {
        if (!ScriptHost::s_gameServer) return 60;
        return ScriptHost::s_gameServer->GetCurrentTickRate();
    }
    
    int GetScriptReloadCount()
    {
        if (!ScriptHost::s_scriptManager) return 0;
        return ScriptHost::s_scriptManager->GetReloadCount();
    }
    
    const char* GetCurrentMap()
    {
        if (!ScriptHost::s_gameServer) return ScriptHost::StoreString("unknown");
        
        auto map = ScriptHost::s_gameServer->GetCurrentMapName();
        return ScriptHost::StoreString(map);
    }
    
    const char* GetCurrentGameMode()
    {
        if (!ScriptHost::s_gameServer) return ScriptHost::StoreString("unknown");
        
        auto mode = ScriptHost::s_gameServer->GetCurrentGameMode();
        return ScriptHost::StoreString(mode);
    }
    
    long GetServerUptime()
    {
        if (!ScriptHost::s_gameServer) return 0;
        return ScriptHost::s_gameServer->GetUptimeSeconds();
    }
    
    const char* GetServerVersion()
    {
        return ScriptHost::StoreString("1.0.0");
    }
    
    // Player lists
    int GetConnectedPlayerCount()
    {
        auto playerMgr = ScriptHost::GetPlayerManager();
        return playerMgr ? playerMgr->GetConnectedPlayerCount() : 0;
    }
    
    const char* GetConnectedPlayerAt(int index)
    {
        auto playerMgr = ScriptHost::GetPlayerManager();
        if (!playerMgr) return ScriptHost::StoreString("");
        
        auto players = playerMgr->GetConnectedPlayerIds();
        if (index >= 0 && index < static_cast<int>(players.size())) {
            return ScriptHost::StoreString(players[index]);
        }
        return ScriptHost::StoreString("");
    }
    
    int GetPlayerCountPerTeam(int teamId)
    {
        auto playerMgr = ScriptHost::GetPlayerManager();
        return playerMgr ? playerMgr->GetTeamPlayerCount(teamId) : 0;
    }
    
    // Team management
    int GetTeamCount()
    {
        if (!ScriptHost::s_gameServer) return 2;
        return ScriptHost::s_gameServer->GetTeamCount();
    }
    
    const char* GetTeamName(int teamId)
    {
        if (!ScriptHost::s_gameServer) return ScriptHost::StoreString("Team");
        
        auto name = ScriptHost::s_gameServer->GetTeamName(teamId);
        return ScriptHost::StoreString(name);
    }
    
    void SetTeamSize(int teamId, int maxSize)
    {
        if (ScriptHost::s_gameServer) {
            ScriptHost::s_gameServer->SetTeamMaxSize(teamId, maxSize);
        }
    }
    
    int GetTeamSize(int teamId)
    {
        if (!ScriptHost::s_gameServer) return 32;
        return ScriptHost::s_gameServer->GetTeamMaxSize(teamId);
    }
    
    int GetTeamScore(int teamId)
    {
        if (!ScriptHost::s_gameServer) return 0;
        return ScriptHost::s_gameServer->GetTeamScore(teamId);
    }
    
    void SetTeamScore(int teamId, int score)
    {
        if (ScriptHost::s_gameServer) {
            ScriptHost::s_gameServer->SetTeamScore(teamId, score);
        }
    }
    
    // Entity management
    bool SpawnEntity(const char* className, float x, float y, float z)
    {
        auto entityMgr = ScriptHost::GetEntityManager();
        if (!entityMgr || !className) return false;
        
        int entityId;
        return entityMgr->SpawnEntity(className, {x, y, z}, &entityId);
    }
    
    bool SpawnEntityWithId(const char* className, float x, float y, float z, int* outEntityId)
    {
        auto entityMgr = ScriptHost::GetEntityManager();
        if (!entityMgr || !className || !outEntityId) return false;
        
        return entityMgr->SpawnEntity(className, {x, y, z}, outEntityId);
    }
    
    void RemoveEntity(int entityId)
    {
        auto entityMgr = ScriptHost::GetEntityManager();
        if (entityMgr) {
            entityMgr->RemoveEntity(entityId);
        }
    }
    
    bool IsEntityValid(int entityId)
    {
        auto entityMgr = ScriptHost::GetEntityManager();
        return entityMgr ? entityMgr->IsEntityValid(entityId) : false;
    }
    
    void MoveEntityTo(int entityId, float x, float y, float z)
    {
        auto entityMgr = ScriptHost::GetEntityManager();
        if (entityMgr) {
            entityMgr->SetEntityPosition(entityId, {x, y, z});
        }
    }
    
    void GetEntityPosition(int entityId, float* x, float* y, float* z)
    {
        auto entityMgr = ScriptHost::GetEntityManager();
        if (!entityMgr || !x || !y || !z) return;
        
        auto pos = entityMgr->GetEntityPosition(entityId);
        *x = pos.x;
        *y = pos.y;
        *z = pos.z;
    }
    
    float GetEntityHealth(int entityId)
    {
        auto entityMgr = ScriptHost::GetEntityManager();
        return entityMgr ? entityMgr->GetEntityHealth(entityId) : 0.0f;
    }
    
    void SetEntityHealth(int entityId, float health)
    {
        auto entityMgr = ScriptHost::GetEntityManager();
        if (entityMgr) {
            entityMgr->SetEntityHealth(entityId, health);
        }
    }
    
    const char* GetEntityClass(int entityId)
    {
        auto entityMgr = ScriptHost::GetEntityManager();
        if (!entityMgr) return ScriptHost::StoreString("");
        
        auto className = entityMgr->GetEntityClass(entityId);
        return ScriptHost::StoreString(className);
    }
    
    int GetEntityCount()
    {
        auto entityMgr = ScriptHost::GetEntityManager();
        return entityMgr ? entityMgr->GetEntityCount() : 0;
    }
    
    int GetEntityCountByClass(const char* className)
    {
        auto entityMgr = ScriptHost::GetEntityManager();
        return (entityMgr && className) ? entityMgr->GetEntityCountByClass(className) : 0;
    }
    
    // Configuration management
    void SetConfigInt(const char* key, int value)
    {
        if (ScriptHost::s_configManager && key) {
            ScriptHost::s_configManager->SetInt(key, value);
        }
    }
    
    int GetConfigInt(const char* key, int defaultValue)
    {
        if (!ScriptHost::s_configManager || !key) return defaultValue;
        return ScriptHost::s_configManager->GetInt(key, defaultValue);
    }
    
    void SetConfigFloat(const char* key, float value)
    {
        if (ScriptHost::s_configManager && key) {
            ScriptHost::s_configManager->SetFloat(key, value);
        }
    }
    
    float GetConfigFloat(const char* key, float defaultValue)
    {
        if (!ScriptHost::s_configManager || !key) return defaultValue;
        return ScriptHost::s_configManager->GetFloat(key, defaultValue);
    }
    
    void SetConfigBool(const char* key, bool value)
    {
        if (ScriptHost::s_configManager && key) {
            ScriptHost::s_configManager->SetBool(key, value);
        }
    }
    
    bool GetConfigBool(const char* key, bool defaultValue)
    {
        if (!ScriptHost::s_configManager || !key) return defaultValue;
        return ScriptHost::s_configManager->GetBool(key, defaultValue);
    }
    
    void SetConfigString(const char* key, const char* value)
    {
        if (ScriptHost::s_configManager && key && value) {
            ScriptHost::s_configManager->SetString(key, value);
        }
    }
    
    const char* GetConfigString(const char* key, const char* defaultValue)
    {
        if (!ScriptHost::s_configManager || !key) {
            return ScriptHost::StoreString(defaultValue ? defaultValue : "");
        }
        
        auto value = ScriptHost::s_configManager->GetString(key, defaultValue ? defaultValue : "");
        return ScriptHost::StoreString(value);
    }
    
    void ReloadConfig()
    {
        if (ScriptHost::s_configManager) {
            ScriptHost::s_configManager->ReloadConfiguration();
        }
    }
    
    bool SaveConfig()
    {
        if (!ScriptHost::s_configManager) return false;
        return ScriptHost::s_configManager->SaveAllConfigurations();
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
    
    // Debug drawing implementations
    void DebugDrawLine(float x1, float y1, float z1, float x2, float y2, float z2, 
                      float duration, float thickness, float r, float g, float b)
    {
        auto eac = ScriptHost::GetEACServer();
        if (!eac) {
            Logger::Debug("DebugDrawLine: (%.2f,%.2f,%.2f) to (%.2f,%.2f,%.2f) [No EAC]", x1, y1, z1, x2, y2, z2);
            return;
        }

        DebugDrawPacket packet;
        packet.type = static_cast<uint8_t>(EACMessageType::DebugDrawCommand);
        packet.drawType = DebugDrawType::Line;
        packet.p[0] = x1; packet.p[1] = y1; packet.p[2] = z1;
        packet.p[3] = x2; packet.p[4] = y2; packet.p[5] = z2;
        packet.p[6] = r; packet.p[7] = g; packet.p[8] = b;
        packet.p[9] = thickness; packet.p[10] = duration;
        
        eac->BroadcastDebugDraw(packet);
    }
    
    void DebugDrawSphere(float x, float y, float z, float radius, float duration, 
                        float r, float g, float b)
    {
        auto eac = ScriptHost::GetEACServer();
        if (!eac) {
            Logger::Debug("DebugDrawSphere: (%.2f,%.2f,%.2f) radius %.2f [No EAC]", x, y, z, radius);
            return;
        }

        DebugDrawPacket packet;
        packet.type = static_cast<uint8_t>(EACMessageType::DebugDrawCommand);
        packet.drawType = DebugDrawType::Sphere;
        packet.p[0] = x; packet.p[1] = y; packet.p[2] = z;
        packet.p[3] = radius;
        packet.p[6] = r; packet.p[7] = g; packet.p[8] = b;
        packet.p[10] = duration;
        
        eac->BroadcastDebugDraw(packet);
    }
    
    void DebugDrawBox(float x, float y, float z, float sizeX, float sizeY, float sizeZ, 
                     float duration, float r, float g, float b)
    {
        Logger::Debug("DebugDrawBox: (%.2f,%.2f,%.2f) size (%.2f,%.2f,%.2f)", 
                     x, y, z, sizeX, sizeY, sizeZ);
    }
    
    void DebugDrawArrow(float x1, float y1, float z1, float x2, float y2, float z2, 
                       float duration, float thickness, float r, float g, float b)
    {
        auto eac = ScriptHost::GetEACServer();
        if (!eac) {
            Logger::Debug("DebugDrawArrow: (%.2f,%.2f,%.2f) to (%.2f,%.2f,%.2f) [No EAC]", x1, y1, z1, x2, y2, z2);
            return;
        }

        DebugDrawPacket packet;
        packet.type = static_cast<uint8_t>(EACMessageType::DebugDrawCommand);
        packet.drawType = DebugDrawType::Arrow;
        packet.p[0] = x1; packet.p[1] = y1; packet.p[2] = z1;
        packet.p[3] = x2; packet.p[4] = y2; packet.p[5] = z2;
        packet.p[6] = r; packet.p[7] = g; packet.p[8] = b;
        packet.p[9] = thickness; packet.p[10] = duration;
        
        eac->BroadcastDebugDraw(packet);
    }
    
    void DebugDrawText(float x, float y, float z, const char* text, float duration, 
                      float r, float g, float b)
    {
        auto eac = ScriptHost::GetEACServer();
        if (!eac) {
            Logger::Debug("DebugDrawText: '%s' at (%.2f,%.2f,%.2f) [No EAC]", text ? text : "", x, y, z);
            return;
        }

        DebugDrawPacket packet;
        packet.type = static_cast<uint8_t>(EACMessageType::DebugDrawCommand);
        packet.drawType = DebugDrawType::Text;
        packet.p[0] = x; packet.p[1] = y; packet.p[2] = z;
        packet.p[6] = r; packet.p[7] = g; packet.p[8] = b;
        packet.p[10] = duration;
        
        if (text) {
            strncpy(packet.text, text, sizeof(packet.text) - 1);
            packet.text[sizeof(packet.text) - 1] = '\0';
        }
        
        eac->BroadcastDebugDraw(packet);
    }
    
    void ClearDebugDrawings()
    {
        Logger::Debug("ClearDebugDrawings called");
    }
    
    // Script management
    bool ReloadScript(const char* scriptPath)
    {
        if (!ScriptHost::s_scriptManager || !scriptPath) return false;
        return ScriptHost::s_scriptManager->ReloadScript(scriptPath);
    }
    
    bool IsScriptLoaded(const char* scriptPath)
    {
        if (!ScriptHost::s_scriptManager || !scriptPath) return false;
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
        if (ScriptHost::s_gameServer) {
            ScriptHost::s_gameServer->StartMatch();
        }
    }
    
    void EndMatch()
    {
        if (ScriptHost::s_gameServer) {
            ScriptHost::s_gameServer->EndMatch();
        }
    }
    
    void PauseMatch()
    {
        if (ScriptHost::s_gameServer) {
            ScriptHost::s_gameServer->PauseMatch();
        }
    }
    
    void ResumeMatch()
    {
        if (ScriptHost::s_gameServer) {
            ScriptHost::s_gameServer->ResumeMatch();
        }
    }
    
    bool IsMatchActive()
    {
        if (!ScriptHost::s_gameServer) return false;
        return ScriptHost::s_gameServer->IsMatchActive();
    }
    
    bool IsMatchPaused()
    {
        if (!ScriptHost::s_gameServer) return false;
        return ScriptHost::s_gameServer->IsMatchPaused();
    }
    
    int GetMatchTimeRemaining()
    {
        if (!ScriptHost::s_gameServer) return 0;
        return ScriptHost::s_gameServer->GetMatchTimeRemaining();
    }
    
    void SetMatchTimeLimit(int seconds)
    {
        if (ScriptHost::s_gameServer) {
            ScriptHost::s_gameServer->SetMatchTimeLimit(seconds);
        }
    }
    
    void ChangeMap(const char* mapName)
    {
        if (ScriptHost::s_gameServer && mapName) {
            ScriptHost::s_gameServer->ChangeMap(mapName);
        }
    }
    
    void ChangeGameMode(const char* gameMode)
    {
        if (ScriptHost::s_gameServer && gameMode) {
            ScriptHost::s_gameServer->ChangeGameMode(gameMode);
        }
    }
    
    // Network and performance
    int GetAveragePlayerPing()
    {
        auto networkMgr = ScriptHost::GetNetworkManager();
        return networkMgr ? networkMgr->GetAveragePlayerPing() : 0;
    }
    
    int GetPlayerPing(const char* playerId)
    {
        auto networkMgr = ScriptHost::GetNetworkManager();
        return (networkMgr && playerId) ? networkMgr->GetPlayerPing(playerId) : 0;
    }
    
    float GetServerCpuUsage()
    {
        if (!ScriptHost::s_gameServer) return 0.0f;
        return ScriptHost::s_gameServer->GetCpuUsagePercent();
    }
    
    long GetServerMemoryUsage()
    {
        if (!ScriptHost::s_gameServer) return 0;
        return ScriptHost::s_gameServer->GetMemoryUsageBytes();
    }
    
    int GetNetworkPacketsPerSecond()
    {
        auto networkMgr = ScriptHost::GetNetworkManager();
        return networkMgr ? networkMgr->GetPacketsPerSecond() : 0;
    }
    
    float GetServerFrameRate()
    {
        if (!ScriptHost::s_gameServer) return 0.0f;
        return ScriptHost::s_gameServer->GetFrameRate();
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
    
    // Weapon and inventory management
    void GivePlayerWeapon(const char* playerId, const char* weaponClass)
    {
        auto playerMgr = ScriptHost::GetPlayerManager();
        if (playerMgr && playerId && weaponClass) {
            playerMgr->GivePlayerWeapon(playerId, weaponClass);
        }
    }
    
    void RemovePlayerWeapon(const char* playerId, const char* weaponClass)
    {
        auto playerMgr = ScriptHost::GetPlayerManager();
        if (playerMgr && playerId && weaponClass) {
            playerMgr->RemovePlayerWeapon(playerId, weaponClass);
        }
    }
    
    bool PlayerHasWeapon(const char* playerId, const char* weaponClass)
    {
        auto playerMgr = ScriptHost::GetPlayerManager();
        return (playerMgr && playerId && weaponClass) ? 
               playerMgr->PlayerHasWeapon(playerId, weaponClass) : false;
    }
    
    const char* GetPlayerPrimaryWeapon(const char* playerId)
    {
        auto playerMgr = ScriptHost::GetPlayerManager();
        if (!playerMgr || !playerId) return ScriptHost::StoreString("");
        
        auto weapon = playerMgr->GetPlayerPrimaryWeapon(playerId);
        return ScriptHost::StoreString(weapon);
    }
    
    void SetPlayerAmmo(const char* playerId, const char* weaponClass, int ammo)
    {
        auto playerMgr = ScriptHost::GetPlayerManager();
        if (playerMgr && playerId && weaponClass) {
            playerMgr->SetPlayerAmmo(playerId, weaponClass, ammo);
        }
    }
    
    int GetPlayerAmmo(const char* playerId, const char* weaponClass)
    {
        auto playerMgr = ScriptHost::GetPlayerManager();
        return (playerMgr && playerId && weaponClass) ? 
               playerMgr->GetPlayerAmmo(playerId, weaponClass) : 0;
    }
    
    // Statistics and scoring
    int GetPlayerKills(const char* playerId)
    {
        auto playerMgr = ScriptHost::GetPlayerManager();
        return (playerMgr && playerId) ? playerMgr->GetPlayerKills(playerId) : 0;
    }
    
    int GetPlayerDeaths(const char* playerId)
    {
        auto playerMgr = ScriptHost::GetPlayerManager();
        return (playerMgr && playerId) ? playerMgr->GetPlayerDeaths(playerId) : 0;
    }
    
    void SetPlayerKills(const char* playerId, int kills)
    {
        auto playerMgr = ScriptHost::GetPlayerManager();
        if (playerMgr && playerId) {
            playerMgr->SetPlayerKills(playerId, kills);
        }
    }
    
    void SetPlayerDeaths(const char* playerId, int deaths)
    {
        auto playerMgr = ScriptHost::GetPlayerManager();
        if (playerMgr && playerId) {
            playerMgr->SetPlayerDeaths(playerId, deaths);
        }
    }
    
    void AddPlayerKill(const char* playerId)
    {
        auto playerMgr = ScriptHost::GetPlayerManager();
        if (playerMgr && playerId) {
            playerMgr->AddPlayerKill(playerId);
        }
    }
    
    void AddPlayerDeath(const char* playerId)
    {
        auto playerMgr = ScriptHost::GetPlayerManager();
        if (playerMgr && playerId) {
            playerMgr->AddPlayerDeath(playerId);
        }
    }
    
    int GetPlayerScore(const char* playerId)
    {
        auto playerMgr = ScriptHost::GetPlayerManager();
        return (playerMgr && playerId) ? playerMgr->GetPlayerScore(playerId) : 0;
    }
    
    void SetPlayerScore(const char* playerId, int score)
    {
        auto playerMgr = ScriptHost::GetPlayerManager();
        if (playerMgr && playerId) {
            playerMgr->SetPlayerScore(playerId, score);
        }
    }
    
    void AddPlayerScore(const char* playerId, int points)
    {
        auto playerMgr = ScriptHost::GetPlayerManager();
        if (playerMgr && playerId) {
            playerMgr->AddPlayerScore(playerId, points);
        }
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