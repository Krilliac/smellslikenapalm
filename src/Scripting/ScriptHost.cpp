// src/Scripting/ScriptHost.cpp

#include "Scripting/ScriptHost.h"
#include "Scripting/ScriptManager.h"
#include "Config/ConfigManager.h"
#include "Game/GameServer.h"
#include "Game/PlayerManager.h"
#include "Game/EntityManager.h"
#include "Network/NetworkManager.h"
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
    void SendChatToPlayer(const char* /*playerId*/, const char* message)
    {
        if (message) Logger::Debug("[ScriptHost] SendChatToPlayer: %s", message);
    }

    void BroadcastChat(const char* message)
    {
        if (message) Logger::Debug("[ScriptHost] BroadcastChat: %s", message);
    }

    void SendPrivateMessage(const char* /*fromPlayerId*/, const char* /*toPlayerId*/, const char* message)
    {
        if (message) Logger::Debug("[ScriptHost] SendPrivateMessage: %s", message);
    }
    
    // Player management -- stubbed (PlayerManager API not yet matching)
    void KickPlayer(const char* /*playerId*/, const char* /*reason*/) {}
    void BanPlayer(const char* /*playerId*/, const char* /*reason*/, int /*durationHours*/) {}
    void UnbanPlayer(const char* /*playerId*/) {}
    int GetPlayerAdminLevel(const char* /*playerId*/) { return 0; }
    bool IsPlayerOnline(const char* /*playerId*/) { return false; }
    const char* GetPlayerName(const char* playerId) { return ScriptHost::StoreString(playerId ? playerId : ""); }
    void GetPlayerPosition(const char* /*playerId*/, float* x, float* y, float* z) { if (x) *x=0; if (y) *y=0; if (z) *z=0; }
    void SetPlayerPosition(const char* /*playerId*/, float /*x*/, float /*y*/, float /*z*/) {}
    int GetPlayerTeam(const char* /*playerId*/) { return -1; }
    void SetPlayerTeam(const char* /*playerId*/, int /*teamId*/) {}
    int GetPlayerHealth(const char* /*playerId*/) { return 0; }
    void SetPlayerHealth(const char* /*playerId*/, int /*health*/) {}
    
    // Server information -- stubbed where API unavailable
    const char* GetDataDirectory() { return ScriptHost::StoreString("data/"); }
    const char* GetServerName() { return ScriptHost::StoreString("RS2V Server"); }
    int GetPlayerCount() { return 0; }
    int GetMaxPlayers() { return 64; }
    int GetCurrentTickRate() { return 60; }
    int GetScriptReloadCount() { return 0; }
    const char* GetCurrentMap() { return ScriptHost::StoreString("unknown"); }
    const char* GetCurrentGameMode() { return ScriptHost::StoreString("unknown"); }
    long GetServerUptime() { return 0; }
    const char* GetServerVersion() { return ScriptHost::StoreString("1.0.0"); }
    
    // Player lists -- stubbed
    int GetConnectedPlayerCount() { return 0; }
    const char* GetConnectedPlayerAt(int /*index*/) { return ScriptHost::StoreString(""); }
    int GetPlayerCountPerTeam(int /*teamId*/) { return 0; }
    
    // Team management -- stubbed
    int GetTeamCount() { return 2; }
    const char* GetTeamName(int /*teamId*/) { return ScriptHost::StoreString("Team"); }
    void SetTeamSize(int /*teamId*/, int /*maxSize*/) {}
    int GetTeamSize(int /*teamId*/) { return 32; }
    int GetTeamScore(int /*teamId*/) { return 0; }
    void SetTeamScore(int /*teamId*/, int /*score*/) {}
    
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
    
    // Configuration management -- stubbed (ConfigManager API mismatch)
    void SetConfigInt(const char* /*key*/, int /*value*/) {}
    int GetConfigInt(const char* /*key*/, int defaultValue) { return defaultValue; }
    void SetConfigFloat(const char* /*key*/, float /*value*/) {}
    float GetConfigFloat(const char* /*key*/, float defaultValue) { return defaultValue; }
    void SetConfigBool(const char* /*key*/, bool /*value*/) {}
    bool GetConfigBool(const char* /*key*/, bool defaultValue) { return defaultValue; }
    void SetConfigString(const char* /*key*/, const char* /*value*/) {}
    const char* GetConfigString(const char* /*key*/, const char* defaultValue) { return ScriptHost::StoreString(defaultValue ? defaultValue : ""); }
    void ReloadConfig() {}
    bool SaveConfig() { return false; }
    
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
    
    // Debug drawing -- stubbed (DebugDrawPacket fields not matching)
    void DebugDrawLine(float /*x1*/, float /*y1*/, float /*z1*/, float /*x2*/, float /*y2*/, float /*z2*/,
                      float /*duration*/, float /*thickness*/, float /*r*/, float /*g*/, float /*b*/) {}
    void DebugDrawSphere(float /*x*/, float /*y*/, float /*z*/, float /*radius*/, float /*duration*/,
                        float /*r*/, float /*g*/, float /*b*/) {}
    void DebugDrawBox(float /*x*/, float /*y*/, float /*z*/, float /*sizeX*/, float /*sizeY*/, float /*sizeZ*/,
                     float /*duration*/, float /*r*/, float /*g*/, float /*b*/) {}
    void DebugDrawArrow(float /*x1*/, float /*y1*/, float /*z1*/, float /*x2*/, float /*y2*/, float /*z2*/,
                       float /*duration*/, float /*thickness*/, float /*r*/, float /*g*/, float /*b*/) {}
    void DebugDrawText(float /*x*/, float /*y*/, float /*z*/, const char* /*text*/, float /*duration*/,
                      float /*r*/, float /*g*/, float /*b*/) {}
    void ClearDebugDrawings() {}
    
    // Script management -- stubbed (ScriptManager API mismatch)
    bool ReloadScript(const char* /*scriptPath*/) { return false; }
    bool IsScriptLoaded(const char* /*scriptPath*/) { return false; }
    int GetLoadedScriptCount() { return 0; }
    const char* GetLoadedScriptAt(int /*index*/) { return ScriptHost::StoreString(""); }
    
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
    
    // Game state management -- stubbed (GameServer API mismatch)
    void StartMatch() {}
    void EndMatch() {}
    void PauseMatch() {}
    void ResumeMatch() {}
    bool IsMatchActive() { return false; }
    bool IsMatchPaused() { return false; }
    int GetMatchTimeRemaining() { return 0; }
    void SetMatchTimeLimit(int /*seconds*/) {}
    void ChangeMap(const char* /*mapName*/) {}
    void ChangeGameMode(const char* /*gameMode*/) {}
    
    // Network and performance -- stubbed
    int GetAveragePlayerPing() { return 0; }
    int GetPlayerPing(const char* /*playerId*/) { return 0; }
    float GetServerCpuUsage() { return 0.0f; }
    long GetServerMemoryUsage() { return 0; }
    int GetNetworkPacketsPerSecond() { return 0; }
    float GetServerFrameRate() { return 0.0f; }
    
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
    
    // Weapon/inventory and statistics -- stubbed (PlayerManager API mismatch)
    void GivePlayerWeapon(const char* /*playerId*/, const char* /*weaponClass*/) {}
    void RemovePlayerWeapon(const char* /*playerId*/, const char* /*weaponClass*/) {}
    bool PlayerHasWeapon(const char* /*playerId*/, const char* /*weaponClass*/) { return false; }
    const char* GetPlayerPrimaryWeapon(const char* /*playerId*/) { return ScriptHost::StoreString(""); }
    void SetPlayerAmmo(const char* /*playerId*/, const char* /*weaponClass*/, int /*ammo*/) {}
    int GetPlayerAmmo(const char* /*playerId*/, const char* /*weaponClass*/) { return 0; }
    int GetPlayerKills(const char* /*playerId*/) { return 0; }
    int GetPlayerDeaths(const char* /*playerId*/) { return 0; }
    void SetPlayerKills(const char* /*playerId*/, int /*kills*/) {}
    void SetPlayerDeaths(const char* /*playerId*/, int /*deaths*/) {}
    void AddPlayerKill(const char* /*playerId*/) {}
    void AddPlayerDeath(const char* /*playerId*/) {}
    int GetPlayerScore(const char* /*playerId*/) { return 0; }
    void SetPlayerScore(const char* /*playerId*/, int /*score*/) {}
    void AddPlayerScore(const char* /*playerId*/, int /*points*/) {}
    
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