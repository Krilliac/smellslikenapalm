// src/Scripting/ScriptManager.h

#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <filesystem>
#include "Config/ServerConfig.h"

class ScriptManager {
public:
    explicit ScriptManager(const ServerConfig& cfg);
    ~ScriptManager();

    // Initialize the scripting subsystem (loads CLR, initial scripts, starts watcher)
    bool Initialize();

    // Shutdown scripting (unload scripts, stop watcher, shutdown CLR)
    void Shutdown();

    // Notify scripts of game events
    void NotifyPlayerJoin(const std::string& playerName);
    void NotifyPlayerLeave(const std::string& playerName);
    void NotifyChatMessage(const std::string& playerName, const std::string& message);
    void NotifyMatchStart();
    void NotifyMatchEnd();

    // Query loaded scripts
    std::vector<std::string> GetLoadedScripts() const;
    bool                      IsScriptLoaded(const std::string& scriptPath) const;

private:
    // CLR hosting
    bool  InitializeCLR();
    void  ShutdownCLR();

    // Script loading/unloading
    bool  LoadAllScripts();
    bool  LoadScript(const std::string& filePath);
    bool  CompileScript(struct ScriptInfo& scriptInfo);
    void  UnloadAllScripts();
    void  UnloadScript(const std::string& filePath);
    bool  ReloadScript(const std::string& filePath);

    // File watcher for hot‐reload
    void  StartFileWatcher();
    void  StopFileWatcher();
    void  FileWatcherThread();
    void  OnFileChanged(const std::string& filePath);

    // Script execution
    bool  ExecuteScriptMethod(const std::string& scriptPath,
                              const std::string& methodName,
                              const std::vector<std::string>& args = {});

    // Utility
    std::string WrapScriptInClass(const std::string& script);
    std::string CreateScriptAssembly(const std::string& wrappedScript);

    const ServerConfig&                            m_config;
    std::string                                    m_scriptsPath;
    std::string                                    m_configPath;
    std::string                                    m_mapsDir;

    bool                                           m_isRunning;
    bool                                           m_watcherThreadRunning;
    std::thread                                    m_watcherThread;
    std::chrono::steady_clock::time_point          m_lastReloadTime;

#ifdef _WIN32
    ICorRuntimeHost*                               m_clrHost;
    _AppDomain*                                    m_appDomain;
#else
    // Mono or other hosting handles could go here
#endif

    mutable std::mutex                             m_scriptsMutex;
    std::map<std::string, struct ScriptInfo>       m_loadedScripts;
};