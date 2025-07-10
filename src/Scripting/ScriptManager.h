#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <chrono>
#include <filesystem>
#include <cstdint>
#include "Config/ServerConfig.h"

struct ScriptInfo {
    std::string filePath;
    std::string content;
    std::filesystem::file_time_type lastModified;
    bool isLoaded = false;
    void* compiledAssembly = nullptr;
};

class ScriptManager {
public:
    explicit ScriptManager(const ServerConfig& cfg);
    ~ScriptManager();

    bool Initialize();
    void Shutdown();

    // Core events
    void NotifyServerStart();
    void NotifyServerShutdown();
    void NotifyPlayerJoin(const std::string& playerId);
    void NotifyPlayerLeave(const std::string& playerId);
    void NotifyChatMessage(const std::string& playerId, const std::string& message);
    void NotifyMatchStart();
    void NotifyMatchEnd();
    void NotifyPlayerMove(const std::string& playerId, float x, float y, float z);
    void NotifyPlayerAction(const std::string& playerId, const std::string& action);
    void NotifyPlayerKill(const std::string& killerId, const std::string& victimId);
    void NotifyPlayerFrozen(const std::string& playerId);
    void NotifyPlayerUnfrozen(const std::string& playerId);
    void NotifyPlayerStunned(const std::string& playerId);
    void NotifyPlayerUnstunned(const std::string& playerId);

    // Script interaction
    std::vector<std::string> GetLoadedScripts() const;
    bool IsScriptLoaded(const std::string& path) const;
    bool ExecuteScriptMethod(const std::string& path, const std::string& method,
                             const std::vector<std::string>& args = {});

    // EAC memory callbacks
    void OnRemoteMemoryRead(uint32_t clientId, uint64_t address, const uint8_t* data, uint32_t len);
    void OnRemoteMemoryWriteAck(uint32_t clientId, bool success);
    void OnRemoteMemoryAlloc(uint32_t clientId, uint64_t baseAddr);
    void OnBroadcastMemoryRead(uint32_t clientId, const uint8_t* data, uint32_t len);

    int GetReloadCount() const { return m_reloadCount; }

private:
    bool InitializeCLR();
    void ShutdownCLR();

    bool LoadAllScripts();
    bool LoadScript(const std::string& path);
    bool CompileScript(ScriptInfo& info);
    void UnloadAllScripts();
    void UnloadScript(const std::string& path);
    bool ReloadScript(const std::string& path);

    void StartFileWatcher();
    void StopFileWatcher();
    void FileWatcherThread();
    void OnFileChanged(const std::string& path);

    std::string WrapScriptInClass(const std::string& script);

    const ServerConfig& m_config;
    std::string m_scriptsPath;
    bool m_isRunning = false;
    bool m_watcherRunning = false;
    std::thread m_watcherThread;
    std::chrono::steady_clock::time_point m_lastReloadTime;
    int m_reloadCount = 0;

#ifdef _WIN32
    ICorRuntimeHost* m_clrHost = nullptr;
    _AppDomain* m_appDomain = nullptr;
#endif

    mutable std::mutex m_mutex;
    std::map<std::string, ScriptInfo> m_scripts;
};