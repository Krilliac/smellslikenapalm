// src/Scripting/ScriptHost.h

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <map>
#include <mutex>
#include <chrono>

class ScriptManager;
class ServerConfig;
class ConfigManager;
class GameServer;
class NetworkManager;
class PlayerManager;
class EntityManager;
class AdminManager;
class EACServerEmulator;

/// <summary>
/// Static facade class that provides native C exports for C# DllImport calls
/// Acts as a bridge between C# scripts and the C++ server engine
/// </summary>
class ScriptHost
{
public:
    // Initialize the ScriptHost with references to server systems
    static bool Initialize(ScriptManager* scriptManager, 
                          std::shared_ptr<ConfigManager> configManager,
                          GameServer* gameServer,
                          EACServerEmulator* eacServer = nullptr);
    
    // Shutdown and cleanup
    static void Shutdown();

    // Get singleton instance for internal use
    static ScriptHost* Instance();

private:
    // Core system references
    static ScriptManager* s_scriptManager;
    static std::shared_ptr<ConfigManager> s_configManager;
    static GameServer* s_gameServer;
    static EACServerEmulator* s_eacServer;
    static std::mutex s_mutex;
    static bool s_initialized;

    // String storage for safe return to C#
    static std::map<std::string, std::string> s_stringStorage;
    static std::mutex s_stringMutex;

    // Event handler registry
    static std::map<std::string, std::vector<std::string>> s_eventHandlers;
    static std::mutex s_eventMutex;

    // Scheduled callbacks
    struct ScheduledCallback {
        float delaySeconds;
        std::string methodName;
        std::chrono::steady_clock::time_point scheduledTime;
        bool executed;
    };
    static std::vector<ScheduledCallback> s_scheduledCallbacks;
    static std::mutex s_callbackMutex;

    // Helper methods
    static const char* StoreString(const std::string& str);
    static PlayerManager* GetPlayerManager();
    static EntityManager* GetEntityManager();
    static AdminManager* GetAdminManager();
    static NetworkManager* GetNetworkManager();
    static EACServerEmulator* GetEACServer();

    // Disable construction
    ScriptHost() = delete;
    ~ScriptHost() = delete;
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;
};

// Native C exports for C# DllImport
extern "C" {
    
    // Event registration and execution
    __declspec(dllexport) void RegisterEventHandler(const char* eventName, const char* methodName);
    __declspec(dllexport) void UnregisterEventHandler(const char* eventName, const char* methodName);
    __declspec(dllexport) bool ExecuteScriptMethod(const char* scriptPath, const char* methodName);
    __declspec(dllexport) bool ExecuteScriptMethodWithArgs(const char* scriptPath, const char* methodName, 
                                                          const char** args, int argCount);
    
    // Logging functions
    __declspec(dllexport) void LogInfo(const char* message);
    __declspec(dllexport) void LogWarning(const char* message);
    __declspec(dllexport) void LogError(const char* message);
    __declspec(dllexport) void LogDebug(const char* message);
    
    // Chat and communication
    __declspec(dllexport) void SendChatToPlayer(const char* playerId, const char* message);
    __declspec(dllexport) void BroadcastChat(const char* message);
    __declspec(dllexport) void SendPrivateMessage(const char* fromPlayerId, const char* toPlayerId, const char* message);
    
    // Player management
    __declspec(dllexport) void KickPlayer(const char* playerId, const char* reason);
    __declspec(dllexport) void BanPlayer(const char* playerId, const char* reason, int durationHours);
    __declspec(dllexport) void UnbanPlayer(const char* playerId);
    __declspec(dllexport) int GetPlayerAdminLevel(const char* playerId);
    __declspec(dllexport) bool IsPlayerOnline(const char* playerId);
    __declspec(dllexport) const char* GetPlayerName(const char* playerId);
    __declspec(dllexport) void GetPlayerPosition(const char* playerId, float* x, float* y, float* z);
    __declspec(dllexport) void SetPlayerPosition(const char* playerId, float x, float y, float z);
    __declspec(dllexport) int GetPlayerTeam(const char* playerId);
    __declspec(dllexport) void SetPlayerTeam(const char* playerId, int teamId);
    __declspec(dllexport) int GetPlayerHealth(const char* playerId);
    __declspec(dllexport) void SetPlayerHealth(const char* playerId, int health);
    
    // Server information
    __declspec(dllexport) const char* GetDataDirectory();
    __declspec(dllexport) const char* GetServerName();
    __declspec(dllexport) int GetPlayerCount();
    __declspec(dllexport) int GetMaxPlayers();
    __declspec(dllexport) int GetCurrentTickRate();
    __declspec(dllexport) int GetScriptReloadCount();
    __declspec(dllexport) const char* GetCurrentMap();
    __declspec(dllexport) const char* GetCurrentGameMode();
    __declspec(dllexport) long GetServerUptime();
    __declspec(dllexport) const char* GetServerVersion();
    
    // Player lists (returns count, use GetConnectedPlayerAt to iterate)
    __declspec(dllexport) int GetConnectedPlayerCount();
    __declspec(dllexport) const char* GetConnectedPlayerAt(int index);
    __declspec(dllexport) int GetPlayerCountPerTeam(int teamId);
    
    // Team management
    __declspec(dllexport) int GetTeamCount();
    __declspec(dllexport) const char* GetTeamName(int teamId);
    __declspec(dllexport) void SetTeamSize(int teamId, int maxSize);
    __declspec(dllexport) int GetTeamSize(int teamId);
    __declspec(dllexport) int GetTeamScore(int teamId);
    __declspec(dllexport) void SetTeamScore(int teamId, int score);
    
    // Entity spawning and management
    __declspec(dllexport) bool SpawnEntity(const char* className, float x, float y, float z);
    __declspec(dllexport) bool SpawnEntityWithId(const char* className, float x, float y, float z, int* outEntityId);
    __declspec(dllexport) void RemoveEntity(int entityId);
    __declspec(dllexport) bool IsEntityValid(int entityId);
    __declspec(dllexport) void MoveEntityTo(int entityId, float x, float y, float z);
    __declspec(dllexport) void GetEntityPosition(int entityId, float* x, float* y, float* z);
    __declspec(dllexport) float GetEntityHealth(int entityId);
    __declspec(dllexport) void SetEntityHealth(int entityId, float health);
    __declspec(dllexport) const char* GetEntityClass(int entityId);
    __declspec(dllexport) int GetEntityCount();
    __declspec(dllexport) int GetEntityCountByClass(const char* className);
    
    // Configuration management
    __declspec(dllexport) void SetConfigInt(const char* key, int value);
    __declspec(dllexport) int GetConfigInt(const char* key, int defaultValue);
    __declspec(dllexport) void SetConfigFloat(const char* key, float value);
    __declspec(dllexport) float GetConfigFloat(const char* key, float defaultValue);
    __declspec(dllexport) void SetConfigBool(const char* key, bool value);
    __declspec(dllexport) bool GetConfigBool(const char* key, bool defaultValue);
    __declspec(dllexport) void SetConfigString(const char* key, const char* value);
    __declspec(dllexport) const char* GetConfigString(const char* key, const char* defaultValue);
    __declspec(dllexport) void ReloadConfig();
    __declspec(dllexport) bool SaveConfig();
    
    // Scheduling and timing
    __declspec(dllexport) void ScheduleCallback(float delaySeconds, const char* methodName);
    __declspec(dllexport) void CancelScheduledCallbacks(const char* methodName);
    __declspec(dllexport) void ProcessScheduledCallbacks();
    __declspec(dllexport) long GetCurrentTimeMillis();
    __declspec(dllexport) const char* GetCurrentTimeString();
    
    // Debug drawing and visualization
    __declspec(dllexport) void DebugDrawLine(float x1, float y1, float z1, float x2, float y2, float z2, 
                                           float duration, float thickness, float r, float g, float b);
    __declspec(dllexport) void DebugDrawSphere(float x, float y, float z, float radius, float duration, 
                                             float r, float g, float b);
    __declspec(dllexport) void DebugDrawBox(float x, float y, float z, float sizeX, float sizeY, float sizeZ, 
                                          float duration, float r, float g, float b);
    __declspec(dllexport) void DebugDrawArrow(float x1, float y1, float z1, float x2, float y2, float z2, 
                                            float duration, float thickness, float r, float g, float b);
    __declspec(dllexport) void DebugDrawText(float x, float y, float z, const char* text, float duration, 
                                           float r, float g, float b);
    __declspec(dllexport) void ClearDebugDrawings();
    
    // Script management
    __declspec(dllexport) bool ReloadScript(const char* scriptPath);
    __declspec(dllexport) bool IsScriptLoaded(const char* scriptPath);
    __declspec(dllexport) int GetLoadedScriptCount();
    __declspec(dllexport) const char* GetLoadedScriptAt(int index);
    __declspec(dllexport) bool EnableScript(const char* scriptName);
    __declspec(dllexport) bool DisableScript(const char* scriptName);
    
    // Game state management
    __declspec(dllexport) void StartMatch();
    __declspec(dllexport) void EndMatch();
    __declspec(dllexport) void PauseMatch();
    __declspec(dllexport) void ResumeMatch();
    __declspec(dllexport) bool IsMatchActive();
    __declspec(dllexport) bool IsMatchPaused();
    __declspec(dllexport) int GetMatchTimeRemaining();
    __declspec(dllexport) void SetMatchTimeLimit(int seconds);
    __declspec(dllexport) void ChangeMap(const char* mapName);
    __declspec(dllexport) void ChangeGameMode(const char* gameMode);
    
    // Network and performance
    __declspec(dllexport) int GetAveragePlayerPing();
    __declspec(dllexport) int GetPlayerPing(const char* playerId);
    __declspec(dllexport) float GetServerCpuUsage();
    __declspec(dllexport) long GetServerMemoryUsage();
    __declspec(dllexport) int GetNetworkPacketsPerSecond();
    __declspec(dllexport) float GetServerFrameRate();
    
    // File and data management
    __declspec(dllexport) bool FileExists(const char* path);
    __declspec(dllexport) bool WriteFile(const char* path, const char* content);
    __declspec(dllexport) const char* ReadFile(const char* path);
    __declspec(dllexport) bool DeleteFile(const char* path);
    __declspec(dllexport) bool CreateDirectory(const char* path);
    __declspec(dllexport) long GetFileSize(const char* path);
    __declspec(dllexport) long GetFileModificationTime(const char* path);
    
    // Weapon and inventory management
    __declspec(dllexport) void GivePlayerWeapon(const char* playerId, const char* weaponClass);
    __declspec(dllexport) void RemovePlayerWeapon(const char* playerId, const char* weaponClass);
    __declspec(dllexport) bool PlayerHasWeapon(const char* playerId, const char* weaponClass);
    __declspec(dllexport) const char* GetPlayerPrimaryWeapon(const char* playerId);
    __declspec(dllexport) void SetPlayerAmmo(const char* playerId, const char* weaponClass, int ammo);
    __declspec(dllexport) int GetPlayerAmmo(const char* playerId, const char* weaponClass);
    
    // Statistics and scoring
    __declspec(dllexport) int GetPlayerKills(const char* playerId);
    __declspec(dllexport) int GetPlayerDeaths(const char* playerId);
    __declspec(dllexport) void SetPlayerKills(const char* playerId, int kills);
    __declspec(dllexport) void SetPlayerDeaths(const char* playerId, int deaths);
    __declspec(dllexport) void AddPlayerKill(const char* playerId);
    __declspec(dllexport) void AddPlayerDeath(const char* playerId);
    __declspec(dllexport) int GetPlayerScore(const char* playerId);
    __declspec(dllexport) void SetPlayerScore(const char* playerId, int score);
    __declspec(dllexport) void AddPlayerScore(const char* playerId, int points);
    
    // EAC Memory Operations
    __declspec(dllexport) bool RemoteRead(uint32_t clientId, uint64_t address, uint32_t length);
    __declspec(dllexport) bool RemoteWrite(uint32_t clientId, uint64_t address, const uint8_t* buf, uint32_t length);
    __declspec(dllexport) bool RemoteAlloc(uint32_t clientId, uint32_t length, uint64_t protect);
    __declspec(dllexport) void BroadcastRemoteRead(uint64_t address, uint32_t length);
    __declspec(dllexport) uint32_t GetConnectedClientId(int index);
    __declspec(dllexport) int GetConnectedClientCount();
    
    // Memory management for strings
    __declspec(dllexport) void FreeString(const char* str);
}