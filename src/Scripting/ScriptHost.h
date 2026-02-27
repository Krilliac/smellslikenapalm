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

    // Core system references (accessible from extern "C" exports)
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

    // Construction (private - use Instance() for singleton)
    ScriptHost() = default;
    ~ScriptHost() = default;
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;
};

// Cross-platform export macro
#ifdef _WIN32
  #define RS2V_EXPORT __declspec(dllexport)
#else
  #define RS2V_EXPORT __attribute__((visibility("default")))
#endif

// Native C exports for C# DllImport
extern "C" {

    // Event registration and execution
    RS2V_EXPORT void RegisterEventHandler(const char* eventName, const char* methodName);
    RS2V_EXPORT void UnregisterEventHandler(const char* eventName, const char* methodName);
    RS2V_EXPORT bool ExecuteScriptMethod(const char* scriptPath, const char* methodName);
    RS2V_EXPORT bool ExecuteScriptMethodWithArgs(const char* scriptPath, const char* methodName,
                                                          const char** args, int argCount);
    
    // Logging functions
    RS2V_EXPORT void LogInfo(const char* message);
    RS2V_EXPORT void LogWarning(const char* message);
    RS2V_EXPORT void LogError(const char* message);
    RS2V_EXPORT void LogDebug(const char* message);
    
    // Chat and communication
    RS2V_EXPORT void SendChatToPlayer(const char* playerId, const char* message);
    RS2V_EXPORT void BroadcastChat(const char* message);
    RS2V_EXPORT void SendPrivateMessage(const char* fromPlayerId, const char* toPlayerId, const char* message);
    
    // Player management
    RS2V_EXPORT void KickPlayer(const char* playerId, const char* reason);
    RS2V_EXPORT void BanPlayer(const char* playerId, const char* reason, int durationHours);
    RS2V_EXPORT void UnbanPlayer(const char* playerId);
    RS2V_EXPORT int GetPlayerAdminLevel(const char* playerId);
    RS2V_EXPORT bool IsPlayerOnline(const char* playerId);
    RS2V_EXPORT const char* GetPlayerName(const char* playerId);
    RS2V_EXPORT void GetPlayerPosition(const char* playerId, float* x, float* y, float* z);
    RS2V_EXPORT void SetPlayerPosition(const char* playerId, float x, float y, float z);
    RS2V_EXPORT int GetPlayerTeam(const char* playerId);
    RS2V_EXPORT void SetPlayerTeam(const char* playerId, int teamId);
    RS2V_EXPORT int GetPlayerHealth(const char* playerId);
    RS2V_EXPORT void SetPlayerHealth(const char* playerId, int health);
    
    // Server information
    RS2V_EXPORT const char* GetDataDirectory();
    RS2V_EXPORT const char* GetServerName();
    RS2V_EXPORT int GetPlayerCount();
    RS2V_EXPORT int GetMaxPlayers();
    RS2V_EXPORT int GetCurrentTickRate();
    RS2V_EXPORT int GetScriptReloadCount();
    RS2V_EXPORT const char* GetCurrentMap();
    RS2V_EXPORT const char* GetCurrentGameMode();
    RS2V_EXPORT long GetServerUptime();
    RS2V_EXPORT const char* GetServerVersion();
    
    // Player lists (returns count, use GetConnectedPlayerAt to iterate)
    RS2V_EXPORT int GetConnectedPlayerCount();
    RS2V_EXPORT const char* GetConnectedPlayerAt(int index);
    RS2V_EXPORT int GetPlayerCountPerTeam(int teamId);
    
    // Team management
    RS2V_EXPORT int GetTeamCount();
    RS2V_EXPORT const char* GetTeamName(int teamId);
    RS2V_EXPORT void SetTeamSize(int teamId, int maxSize);
    RS2V_EXPORT int GetTeamSize(int teamId);
    RS2V_EXPORT int GetTeamScore(int teamId);
    RS2V_EXPORT void SetTeamScore(int teamId, int score);
    
    // Entity spawning and management
    RS2V_EXPORT bool SpawnEntity(const char* className, float x, float y, float z);
    RS2V_EXPORT bool SpawnEntityWithId(const char* className, float x, float y, float z, int* outEntityId);
    RS2V_EXPORT void RemoveEntity(int entityId);
    RS2V_EXPORT bool IsEntityValid(int entityId);
    RS2V_EXPORT void MoveEntityTo(int entityId, float x, float y, float z);
    RS2V_EXPORT void GetEntityPosition(int entityId, float* x, float* y, float* z);
    RS2V_EXPORT float GetEntityHealth(int entityId);
    RS2V_EXPORT void SetEntityHealth(int entityId, float health);
    RS2V_EXPORT const char* GetEntityClass(int entityId);
    RS2V_EXPORT int GetEntityCount();
    RS2V_EXPORT int GetEntityCountByClass(const char* className);
    
    // Configuration management
    RS2V_EXPORT void SetConfigInt(const char* key, int value);
    RS2V_EXPORT int GetConfigInt(const char* key, int defaultValue);
    RS2V_EXPORT void SetConfigFloat(const char* key, float value);
    RS2V_EXPORT float GetConfigFloat(const char* key, float defaultValue);
    RS2V_EXPORT void SetConfigBool(const char* key, bool value);
    RS2V_EXPORT bool GetConfigBool(const char* key, bool defaultValue);
    RS2V_EXPORT void SetConfigString(const char* key, const char* value);
    RS2V_EXPORT const char* GetConfigString(const char* key, const char* defaultValue);
    RS2V_EXPORT void ReloadConfig();
    RS2V_EXPORT bool SaveConfig();
    
    // Scheduling and timing
    RS2V_EXPORT void ScheduleCallback(float delaySeconds, const char* methodName);
    RS2V_EXPORT void CancelScheduledCallbacks(const char* methodName);
    RS2V_EXPORT void ProcessScheduledCallbacks();
    RS2V_EXPORT long GetCurrentTimeMillis();
    RS2V_EXPORT const char* GetCurrentTimeString();
    
    // Debug drawing and visualization
    RS2V_EXPORT void DebugDrawLine(float x1, float y1, float z1, float x2, float y2, float z2, 
                                           float duration, float thickness, float r, float g, float b);
    RS2V_EXPORT void DebugDrawSphere(float x, float y, float z, float radius, float duration, 
                                             float r, float g, float b);
    RS2V_EXPORT void DebugDrawBox(float x, float y, float z, float sizeX, float sizeY, float sizeZ, 
                                          float duration, float r, float g, float b);
    RS2V_EXPORT void DebugDrawArrow(float x1, float y1, float z1, float x2, float y2, float z2, 
                                            float duration, float thickness, float r, float g, float b);
    RS2V_EXPORT void DebugDrawText(float x, float y, float z, const char* text, float duration, 
                                           float r, float g, float b);
    RS2V_EXPORT void ClearDebugDrawings();
    
    // Script management
    RS2V_EXPORT bool ReloadScript(const char* scriptPath);
    RS2V_EXPORT bool IsScriptLoaded(const char* scriptPath);
    RS2V_EXPORT int GetLoadedScriptCount();
    RS2V_EXPORT const char* GetLoadedScriptAt(int index);
    RS2V_EXPORT bool EnableScript(const char* scriptName);
    RS2V_EXPORT bool DisableScript(const char* scriptName);
    
    // Game state management
    RS2V_EXPORT void StartMatch();
    RS2V_EXPORT void EndMatch();
    RS2V_EXPORT void PauseMatch();
    RS2V_EXPORT void ResumeMatch();
    RS2V_EXPORT bool IsMatchActive();
    RS2V_EXPORT bool IsMatchPaused();
    RS2V_EXPORT int GetMatchTimeRemaining();
    RS2V_EXPORT void SetMatchTimeLimit(int seconds);
    RS2V_EXPORT void ChangeMap(const char* mapName);
    RS2V_EXPORT void ChangeGameMode(const char* gameMode);
    
    // Network and performance
    RS2V_EXPORT int GetAveragePlayerPing();
    RS2V_EXPORT int GetPlayerPing(const char* playerId);
    RS2V_EXPORT float GetServerCpuUsage();
    RS2V_EXPORT long GetServerMemoryUsage();
    RS2V_EXPORT int GetNetworkPacketsPerSecond();
    RS2V_EXPORT float GetServerFrameRate();
    
    // File and data management
    RS2V_EXPORT bool FileExists(const char* path);
    RS2V_EXPORT bool WriteFile(const char* path, const char* content);
    RS2V_EXPORT const char* ReadFile(const char* path);
    RS2V_EXPORT bool DeleteFile(const char* path);
    RS2V_EXPORT bool CreateDirectory(const char* path);
    RS2V_EXPORT long GetFileSize(const char* path);
    RS2V_EXPORT long GetFileModificationTime(const char* path);
    
    // Weapon and inventory management
    RS2V_EXPORT void GivePlayerWeapon(const char* playerId, const char* weaponClass);
    RS2V_EXPORT void RemovePlayerWeapon(const char* playerId, const char* weaponClass);
    RS2V_EXPORT bool PlayerHasWeapon(const char* playerId, const char* weaponClass);
    RS2V_EXPORT const char* GetPlayerPrimaryWeapon(const char* playerId);
    RS2V_EXPORT void SetPlayerAmmo(const char* playerId, const char* weaponClass, int ammo);
    RS2V_EXPORT int GetPlayerAmmo(const char* playerId, const char* weaponClass);
    
    // Statistics and scoring
    RS2V_EXPORT int GetPlayerKills(const char* playerId);
    RS2V_EXPORT int GetPlayerDeaths(const char* playerId);
    RS2V_EXPORT void SetPlayerKills(const char* playerId, int kills);
    RS2V_EXPORT void SetPlayerDeaths(const char* playerId, int deaths);
    RS2V_EXPORT void AddPlayerKill(const char* playerId);
    RS2V_EXPORT void AddPlayerDeath(const char* playerId);
    RS2V_EXPORT int GetPlayerScore(const char* playerId);
    RS2V_EXPORT void SetPlayerScore(const char* playerId, int score);
    RS2V_EXPORT void AddPlayerScore(const char* playerId, int points);
    
    // EAC Memory Operations
    RS2V_EXPORT bool RemoteRead(uint32_t clientId, uint64_t address, uint32_t length);
    RS2V_EXPORT bool RemoteWrite(uint32_t clientId, uint64_t address, const uint8_t* buf, uint32_t length);
    RS2V_EXPORT bool RemoteAlloc(uint32_t clientId, uint32_t length, uint64_t protect);
    RS2V_EXPORT void BroadcastRemoteRead(uint64_t address, uint32_t length);
    RS2V_EXPORT uint32_t GetConnectedClientId(int index);
    RS2V_EXPORT int GetConnectedClientCount();
    
    // Memory management for strings
    RS2V_EXPORT void FreeString(const char* str);
}