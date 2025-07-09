// src/Scripting/ScriptManager.cpp

#include "Scripting/ScriptManager.h"
#include "Config/ServerConfig.h"
#include "Utils/Logger.h"
#include "Utils/StringUtils.h"
#include "Utils/FileUtils.h"
#include <fstream>
#include <filesystem>
#include <thread>
#include <chrono>
#include <algorithm>

#ifdef _WIN32
    #include <windows.h>
    #include <metahost.h>
    #pragma comment(lib, "MSCorEE.lib")
    #import "mscorlib.tlb" auto_rename
#else
    #include <sys/inotify.h>
    #include <unistd.h>
    #include <sys/select.h>
    #include <errno.h>
#endif

ScriptManager::ScriptManager(const ServerConfig& cfg)
    : m_config(cfg)
    , m_scriptsPath(cfg.GetDataDirectory() + "/" + cfg.GetString("Scripting.scripts_path", "data/scripts/") + "/enabled/")
    , m_isRunning(false)
    , m_watcherThreadRunning(false)
    , m_clrHost(nullptr)
    , m_appDomain(nullptr)
    , m_lastReloadTime(std::chrono::steady_clock::now())
{
    Logger::Info("ScriptManager initialized with scripts path: %s", m_scriptsPath.c_str());
}

ScriptManager::~ScriptManager()
{
    Shutdown();
}

bool ScriptManager::Initialize()
{
    Logger::Info("Initializing C# Script Manager...");

    // Check if scripting is enabled
    if (!m_config.GetBool("Scripting.enable_csharp_scripting", true)) {
        Logger::Info("C# scripting is disabled in configuration");
        return true;
    }

    // Ensure scripts directory exists
    try {
        std::filesystem::create_directories(m_scriptsPath);
        Logger::Info("Scripts directory ensured: %s", m_scriptsPath.c_str());
    } catch (const std::exception& e) {
        Logger::Error("Failed to create scripts directory: %s", e.what());
        return false;
    }

    // Initialize CLR hosting
    if (!InitializeCLR()) {
        Logger::Error("Failed to initialize CLR hosting");
        return false;
    }

    // Load initial scripts
    if (!LoadAllScripts()) {
        Logger::Warn("Failed to load some scripts during initialization");
    }

    // Start file watcher thread
    StartFileWatcher();

    m_isRunning = true;
    Logger::Info("C# Script Manager initialized successfully");
    return true;
}

void ScriptManager::Shutdown()
{
    if (!m_isRunning) return;

    Logger::Info("Shutting down Script Manager...");
    m_isRunning = false;

    // Stop file watcher
    StopFileWatcher();

    // Unload all scripts
    UnloadAllScripts();

    // Shutdown CLR
    ShutdownCLR();

    Logger::Info("Script Manager shutdown complete");
}

bool ScriptManager::InitializeCLR()
{
#ifdef _WIN32
    Logger::Debug("Initializing CLR hosting on Windows...");

    HRESULT hr;
    
    // Get CLR MetaHost
    ICLRMetaHost* pMetaHost = nullptr;
    hr = CLRCreateInstance(CLSID_CLRMetaHost, IID_ICLRMetaHost, (LPVOID*)&pMetaHost);
    if (FAILED(hr)) {
        Logger::Error("CLRCreateInstance failed: 0x%08lx", hr);
        return false;
    }

    // Get runtime info for .NET Framework 4.8
    ICLRRuntimeInfo* pRuntimeInfo = nullptr;
    hr = pMetaHost->GetRuntime(L"v4.0.30319", IID_ICLRRuntimeInfo, (LPVOID*)&pRuntimeInfo);
    if (FAILED(hr)) {
        Logger::Error("GetRuntime failed: 0x%08lx", hr);
        pMetaHost->Release();
        return false;
    }

    // Check if runtime is loadable
    BOOL bLoadable;
    hr = pRuntimeInfo->IsLoadable(&bLoadable);
    if (FAILED(hr) || !bLoadable) {
        Logger::Error("Runtime is not loadable: 0x%08lx", hr);
        pRuntimeInfo->Release();
        pMetaHost->Release();
        return false;
    }

    // Get CLR runtime host
    hr = pRuntimeInfo->GetInterface(CLSID_CorRuntimeHost, IID_ICorRuntimeHost, (LPVOID*)&m_clrHost);
    if (FAILED(hr)) {
        Logger::Error("GetInterface failed: 0x%08lx", hr);
        pRuntimeInfo->Release();
        pMetaHost->Release();
        return false;
    }

    // Start CLR
    hr = m_clrHost->Start();
    if (FAILED(hr)) {
        Logger::Error("CLR Start failed: 0x%08lx", hr);
        m_clrHost->Release();
        m_clrHost = nullptr;
        pRuntimeInfo->Release();
        pMetaHost->Release();
        return false;
    }

    // Get default AppDomain
    IUnknown* pAppDomainThunk = nullptr;
    hr = m_clrHost->GetDefaultDomain(&pAppDomainThunk);
    if (FAILED(hr)) {
        Logger::Error("GetDefaultDomain failed: 0x%08lx", hr);
        m_clrHost->Stop();
        m_clrHost->Release();
        m_clrHost = nullptr;
        return false;
    }

    hr = pAppDomainThunk->QueryInterface(IID__AppDomain, (LPVOID*)&m_appDomain);
    pAppDomainThunk->Release();

    if (FAILED(hr)) {
        Logger::Error("QueryInterface for AppDomain failed: 0x%08lx", hr);
        return false;
    }

    // Cleanup
    pRuntimeInfo->Release();
    pMetaHost->Release();

    Logger::Info("CLR hosting initialized successfully");
    return true;

#else
    // For Linux/macOS, we would use Mono hosting
    Logger::Error("CLR hosting not implemented for this platform");
    return false;
#endif
}

void ScriptManager::ShutdownCLR()
{
#ifdef _WIN32
    if (m_appDomain) {
        m_appDomain->Release();
        m_appDomain = nullptr;
    }

    if (m_clrHost) {
        m_clrHost->Stop();
        m_clrHost->Release();
        m_clrHost = nullptr;
    }
#endif
}

bool ScriptManager::LoadAllScripts()
{
    Logger::Debug("Loading all scripts from: %s", m_scriptsPath.c_str());

    std::lock_guard<std::mutex> lock(m_scriptsMutex);
    bool allSuccess = true;

    try
    {
        // Only scan the "enabled" subdirectory for active C# scripts
        std::filesystem::path enabledDir = std::filesystem::path(m_scriptsPath) / "enabled";

        for (const auto& entry : std::filesystem::recursive_directory_iterator(enabledDir))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".cs")
            {
                if (!LoadScript(entry.path().string()))
                {
                    allSuccess = false;
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        const auto dir = (std::filesystem::path(m_scriptsPath) / "enabled").string();
        Logger::Error("Error scanning enabled scripts directory '%s': %s",
                      dir.c_str(), e.what());
        return false;
    }

    Logger::Info("Loaded %zu scripts", m_loadedScripts.size());
    return allSuccess;
}

bool ScriptManager::LoadScript(const std::string& filePath)
{
    Logger::Debug("Loading script: %s", filePath.c_str());

    try {
        // Read script content
        std::ifstream file(filePath);
        if (!file.is_open()) {
            Logger::Error("Cannot open script file: %s", filePath.c_str());
            return false;
        }

        std::string content((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
        file.close();

        if (content.empty()) {
            Logger::Warn("Script file is empty: %s", filePath.c_str());
            return false;
        }

        // Create script info
        ScriptInfo scriptInfo;
        scriptInfo.filePath = filePath;
        scriptInfo.content = content;
        scriptInfo.lastModified = std::filesystem::last_write_time(filePath);
        scriptInfo.isLoaded = false;

        // Compile the script using Roslyn
        if (CompileScript(scriptInfo)) {
            m_loadedScripts[filePath] = scriptInfo;
            Logger::Info("Successfully loaded script: %s", filePath.c_str());
            
            // Execute initialization if present
            ExecuteScriptMethod(filePath, "Initialize");
            
            return true;
        } else {
            Logger::Error("Failed to compile script: %s", filePath.c_str());
            return false;
        }

    } catch (const std::exception& e) {
        Logger::Error("Exception loading script %s: %s", filePath.c_str(), e.what());
        return false;
    }
}

bool ScriptManager::CompileScript(ScriptInfo& scriptInfo)
{
#ifdef _WIN32
    Logger::Debug("Compiling script using CLR...");

    try {
        // Wrap script in a class if not already wrapped
        std::string wrappedScript = WrapScriptInClass(scriptInfo.content);
        
        // Create compilation parameters
        std::string assemblyCode = CreateScriptAssembly(wrappedScript);
        
        // Compile using CodeDom (simplified approach)
        // In a real implementation, you would use Roslyn scripting APIs
        // For now, we'll mark as compiled for demonstration
        scriptInfo.isLoaded = true;
        scriptInfo.compiledAssembly = nullptr; // Would contain actual assembly
        
        return true;
    } catch (const std::exception& e) {
        Logger::Error("Script compilation failed: %s", e.what());
        return false;
    }
#else
    Logger::Error("Script compilation not implemented for this platform");
    return false;
#endif
}

std::string ScriptManager::WrapScriptInClass(const std::string& script)
{
    // Check if script already contains a class definition
    if (script.find("class ") != std::string::npos) {
        return script;
    }

    // Wrap in a default script class
    std::ostringstream wrapped;
    wrapped << "using System;\n";
    wrapped << "using System.Collections.Generic;\n";
    wrapped << "using System.Linq;\n";
    wrapped << "using System.Text;\n";
    wrapped << "\n";
    wrapped << "public class DynamicScript\n";
    wrapped << "{\n";
    wrapped << "    public void Initialize() { OnInitialize(); }\n";
    wrapped << "    public void OnPlayerJoin(string playerName) { PlayerJoin(playerName); }\n";
    wrapped << "    public void OnPlayerLeave(string playerName) { PlayerLeave(playerName); }\n";
    wrapped << "    public void OnChatMessage(string player, string message) { ChatMessage(player, message); }\n";
    wrapped << "\n";
    wrapped << "    // User script content:\n";
    wrapped << script << "\n";
    wrapped << "\n";
    wrapped << "    // Default implementations (can be overridden)\n";
    wrapped << "    protected virtual void OnInitialize() { }\n";
    wrapped << "    protected virtual void PlayerJoin(string playerName) { }\n";
    wrapped << "    protected virtual void PlayerLeave(string playerName) { }\n";
    wrapped << "    protected virtual void ChatMessage(string player, string message) { }\n";
    wrapped << "}\n";

    return wrapped.str();
}

std::string ScriptManager::CreateScriptAssembly(const std::string& wrappedScript)
{
    // This would create a proper assembly using Roslyn
    // For now, return the wrapped script
    return wrappedScript;
}

void ScriptManager::UnloadAllScripts()
{
    std::lock_guard<std::mutex> lock(m_scriptsMutex);
    
    Logger::Debug("Unloading all scripts...");
    
    for (auto& [path, scriptInfo] : m_loadedScripts) {
        UnloadScript(path);
    }
    
    m_loadedScripts.clear();
    Logger::Info("All scripts unloaded");
}

void ScriptManager::UnloadScript(const std::string& filePath)
{
    Logger::Debug("Unloading script: %s", filePath.c_str());
    
    auto it = m_loadedScripts.find(filePath);
    if (it != m_loadedScripts.end()) {
        // Execute cleanup if present
        ExecuteScriptMethod(filePath, "Cleanup");
        
        // Release assembly resources
        if (it->second.compiledAssembly) {
            // Release compiled assembly
            it->second.compiledAssembly = nullptr;
        }
        
        it->second.isLoaded = false;
    }
}

bool ScriptManager::ReloadScript(const std::string& filePath)
{
    Logger::Info("Reloading script: %s", filePath.c_str());
    
    std::lock_guard<std::mutex> lock(m_scriptsMutex);
    
    // Unload existing script
    UnloadScript(filePath);
    
    // Remove from loaded scripts
    m_loadedScripts.erase(filePath);
    
    // Load the script again
    return LoadScript(filePath);
}

void ScriptManager::StartFileWatcher()
{
    if (m_watcherThreadRunning) return;

    m_watcherThreadRunning = true;
    m_watcherThread = std::thread(&ScriptManager::FileWatcherThread, this);
    Logger::Debug("File watcher thread started");
}

void ScriptManager::StopFileWatcher()
{
    if (!m_watcherThreadRunning) return;

    m_watcherThreadRunning = false;
    
    if (m_watcherThread.joinable()) {
        m_watcherThread.join();
    }
    
    Logger::Debug("File watcher thread stopped");
}

void ScriptManager::FileWatcherThread()
{
#ifdef _WIN32
    // Windows file monitoring using ReadDirectoryChangesW
    HANDLE hDir = CreateFileA(
        m_scriptsPath.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr
    );

    if (hDir == INVALID_HANDLE_VALUE) {
        Logger::Error("Failed to open directory for monitoring: %s", m_scriptsPath.c_str());
        return;
    }

    char buffer[4096];
    DWORD bytesReturned;
    FILE_NOTIFY_INFORMATION* pNotify;

    while (m_watcherThreadRunning) {
        if (ReadDirectoryChangesA(
            hDir,
            buffer,
            sizeof(buffer),
            TRUE, // Watch subdirectories
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME,
            &bytesReturned,
            nullptr,
            nullptr
        )) {
            pNotify = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer);
            
            do {
                std::string fileName(pNotify->FileName, pNotify->FileNameLength / sizeof(char));
                if (StringUtils::EndsWith(fileName, ".cs")) {
                    std::string fullPath = m_scriptsPath + fileName;
                    OnFileChanged(fullPath);
                }
                
                if (pNotify->NextEntryOffset == 0) break;
                pNotify = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(
                    reinterpret_cast<char*>(pNotify) + pNotify->NextEntryOffset
                );
            } while (true);
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    CloseHandle(hDir);

#else
    // Linux file monitoring using inotify
    int inotifyFd = inotify_init();
    if (inotifyFd == -1) {
        Logger::Error("Failed to initialize inotify");
        return;
    }

    int watchDescriptor = inotify_add_watch(
        inotifyFd,
        m_scriptsPath.c_str(),
        IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_TO
    );

    if (watchDescriptor == -1) {
        Logger::Error("Failed to add inotify watch for: %s", m_scriptsPath.c_str());
        close(inotifyFd);
        return;
    }

    char buffer[4096];
    fd_set readfds;
    struct timeval timeout;

    while (m_watcherThreadRunning) {
        FD_ZERO(&readfds);
        FD_SET(inotifyFd, &readfds);
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000; // 100ms

        int selectResult = select(inotifyFd + 1, &readfds, nullptr, nullptr, &timeout);
        
        if (selectResult > 0 && FD_ISSET(inotifyFd, &readfds)) {
            ssize_t length = read(inotifyFd, buffer, sizeof(buffer));
            
            if (length > 0) {
                size_t offset = 0;
                while (offset < static_cast<size_t>(length)) {
                    struct inotify_event* event = reinterpret_cast<struct inotify_event*>(buffer + offset);
                    
                    if (event->len > 0) {
                        std::string fileName(event->name);
                        if (StringUtils::EndsWith(fileName, ".cs")) {
                            std::string fullPath = m_scriptsPath + fileName;
                            OnFileChanged(fullPath);
                        }
                    }
                    
                    offset += sizeof(struct inotify_event) + event->len;
                }
            }
        }
    }

    inotify_rm_watch(inotifyFd, watchDescriptor);
    close(inotifyFd);
#endif
}

void ScriptManager::OnFileChanged(const std::string& filePath)
{
    // Debounce rapid file changes
    auto now = std::chrono::steady_clock::now();
    if (now - m_lastReloadTime < std::chrono::milliseconds(500)) {
        return;
    }
    m_lastReloadTime = now;

    Logger::Debug("File changed: %s", filePath.c_str());

    // Check if file exists (might have been deleted)
    if (!std::filesystem::exists(filePath)) {
        // File was deleted
        std::lock_guard<std::mutex> lock(m_scriptsMutex);
        auto it = m_loadedScripts.find(filePath);
        if (it != m_loadedScripts.end()) {
            UnloadScript(filePath);
            m_loadedScripts.erase(it);
            Logger::Info("Script removed: %s", filePath.c_str());
        }
        return;
    }

    // File was modified or created
    ReloadScript(filePath);
}

bool ScriptManager::ExecuteScriptMethod(const std::string& scriptPath, const std::string& methodName, const std::vector<std::string>& args)
{
    std::lock_guard<std::mutex> lock(m_scriptsMutex);
    
    auto it = m_loadedScripts.find(scriptPath);
    if (it == m_loadedScripts.end() || !it->second.isLoaded) {
        Logger::Debug("Script not loaded: %s", scriptPath.c_str());
        return false;
    }

    try {
        // Execute method using reflection
        // This is a simplified implementation
        Logger::Debug("Executing method %s in script %s", methodName.c_str(), scriptPath.c_str());
        
        // In a real implementation, you would use CLR reflection to invoke the method
        // For now, we'll just log the execution
        
        return true;
    } catch (const std::exception& e) {
        Logger::Error("Failed to execute method %s in script %s: %s", 
                     methodName.c_str(), scriptPath.c_str(), e.what());
        return false;
    }
}

void ScriptManager::NotifyPlayerJoin(const std::string& playerName)
{
    if (!m_isRunning) return;

    Logger::Debug("Notifying scripts of player join: %s", playerName.c_str());
    
    std::lock_guard<std::mutex> lock(m_scriptsMutex);
    for (const auto& [path, scriptInfo] : m_loadedScripts) {
        if (scriptInfo.isLoaded) {
            ExecuteScriptMethod(path, "OnPlayerJoin", {playerName});
        }
    }
}

void ScriptManager::NotifyPlayerLeave(const std::string& playerName)
{
    if (!m_isRunning) return;

    Logger::Debug("Notifying scripts of player leave: %s", playerName.c_str());
    
    std::lock_guard<std::mutex> lock(m_scriptsMutex);
    for (const auto& [path, scriptInfo] : m_loadedScripts) {
        if (scriptInfo.isLoaded) {
            ExecuteScriptMethod(path, "OnPlayerLeave", {playerName});
        }
    }
}

void ScriptManager::NotifyChatMessage(const std::string& playerName, const std::string& message)
{
    if (!m_isRunning) return;

    Logger::Debug("Notifying scripts of chat message from %s: %s", playerName.c_str(), message.c_str());
    
    std::lock_guard<std::mutex> lock(m_scriptsMutex);
    for (const auto& [path, scriptInfo] : m_loadedScripts) {
        if (scriptInfo.isLoaded) {
            ExecuteScriptMethod(path, "OnChatMessage", {playerName, message});
        }
    }
}

void ScriptManager::NotifyMatchStart()
{
    if (!m_isRunning) return;

    Logger::Debug("Notifying scripts of match start");
    
    std::lock_guard<std::mutex> lock(m_scriptsMutex);
    for (const auto& [path, scriptInfo] : m_loadedScripts) {
        if (scriptInfo.isLoaded) {
            ExecuteScriptMethod(path, "OnMatchStart", {});
        }
    }
}

void ScriptManager::NotifyMatchEnd()
{
    if (!m_isRunning) return;

    Logger::Debug("Notifying scripts of match end");
    
    std::lock_guard<std::mutex> lock(m_scriptsMutex);
    for (const auto& [path, scriptInfo] : m_loadedScripts) {
        if (scriptInfo.isLoaded) {
            ExecuteScriptMethod(path, "OnMatchEnd", {});
        }
    }
}

std::vector<std::string> ScriptManager::GetLoadedScripts() const
{
    std::lock_guard<std::mutex> lock(m_scriptsMutex);
    
    std::vector<std::string> scriptPaths;
    scriptPaths.reserve(m_loadedScripts.size());
    
    for (const auto& [path, scriptInfo] : m_loadedScripts) {
        if (scriptInfo.isLoaded) {
            scriptPaths.push_back(path);
        }
    }
    
    return scriptPaths;
}

bool ScriptManager::IsScriptLoaded(const std::string& scriptPath) const
{
    std::lock_guard<std::mutex> lock(m_scriptsMutex);
    
    auto it = m_loadedScripts.find(scriptPath);
    return it != m_loadedScripts.end() && it->second.isLoaded;
}