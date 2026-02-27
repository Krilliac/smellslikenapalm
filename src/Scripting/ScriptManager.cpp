// src/Scripting/ScriptManager.cpp

#include "Scripting/ScriptManager.h"
#include "Config/ServerConfig.h"
#include "Utils/Logger.h"
#include "Utils/StringUtils.h"
#include "Utils/FileUtils.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <filesystem>
#include <chrono>
#include <thread>

#ifdef _WIN32
    #include <windows.h>
    #include <metahost.h>
    #pragma comment(lib, "MSCorEE.lib")
    #import "mscorlib.tlb" auto_rename raw_interfaces_only \
             high_property_prefixes("_get","_put","_putref") \
             rename("ReportEvent", "InteropServices_ReportEvent")
#else
    #include <sys/inotify.h>
    #include <unistd.h>
    #include <sys/select.h>
    #include <errno.h>
#endif

ScriptManager::ScriptManager(const ServerConfig& cfg)
    : m_config(cfg)
    , m_scriptsPath(cfg.GetDataDirectory() + "/data/scripts/")
    , m_isRunning(false)
    , m_watcherRunning(false)
#ifdef _WIN32
    , m_clrHost(nullptr)
    , m_appDomain(nullptr)
#endif
    , m_lastReloadTime(std::chrono::steady_clock::now())
    , m_reloadCount(0)
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

    // Ensure scripts directory exists
    std::filesystem::create_directories(m_scriptsPath + "/enabled");
    std::filesystem::create_directories(m_scriptsPath + "/disabled");
    Logger::Info("Scripts directory ensured: %s", m_scriptsPath.c_str());

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

    // Notify scripts server start
    NotifyServerStart();

    m_isRunning = true;
    Logger::Info("C# Script Manager initialized successfully");
    return true;
}

void ScriptManager::Shutdown()
{
    if (!m_isRunning) return;

    NotifyServerShutdown();

    Logger::Info("Shutting down Script Manager...");
    m_isRunning = false;

    StopFileWatcher();
    UnloadAllScripts();
    ShutdownCLR();

    Logger::Info("Script Manager shutdown complete");
}

#ifdef _WIN32
bool ScriptManager::InitializeCLR()
{
    Logger::Debug("Initializing CLR hosting on Windows...");

    ICLRMetaHost* pMetaHost = nullptr;
    if (FAILED(CLRCreateInstance(CLSID_CLRMetaHost, IID_ICLRMetaHost, (LPVOID*)&pMetaHost))) {
        Logger::Error("CLRCreateInstance failed");
        return false;
    }

    ICLRRuntimeInfo* pRuntimeInfo = nullptr;
    if (FAILED(pMetaHost->GetRuntime(L"v4.0.30319", IID_ICLRRuntimeInfo, (LPVOID*)&pRuntimeInfo))) {
        Logger::Error("GetRuntime failed");
        pMetaHost->Release();
        return false;
    }

    BOOL bLoadable = FALSE;
    if (FAILED(pRuntimeInfo->IsLoadable(&bLoadable)) || !bLoadable) {
        Logger::Error("Runtime not loadable");
        pRuntimeInfo->Release();
        pMetaHost->Release();
        return false;
    }

    if (FAILED(pRuntimeInfo->GetInterface(CLSID_CorRuntimeHost, IID_ICorRuntimeHost, (LPVOID*)&m_clrHost))) {
        Logger::Error("GetInterface failed");
        pRuntimeInfo->Release();
        pMetaHost->Release();
        return false;
    }

    if (FAILED(m_clrHost->Start())) {
        Logger::Error("CLR Start failed");
        m_clrHost->Release();
        m_clrHost = nullptr;
        pRuntimeInfo->Release();
        pMetaHost->Release();
        return false;
    }

    IUnknown* pAppDomainThunk = nullptr;
    m_clrHost->GetDefaultDomain(&pAppDomainThunk);
    pAppDomainThunk->QueryInterface(IID__AppDomain, (LPVOID*)&m_appDomain);
    pAppDomainThunk->Release();

    pRuntimeInfo->Release();
    pMetaHost->Release();
    Logger::Info("CLR hosting initialized successfully");
    return true;
}

void ScriptManager::ShutdownCLR()
{
    Logger::Debug("Shutting down CLR...");

    if (m_appDomain) {
        m_appDomain->Release();
        m_appDomain = nullptr;
    }
    if (m_clrHost) {
        m_clrHost->Stop();
        m_clrHost->Release();
        m_clrHost = nullptr;
    }
}
#else
bool ScriptManager::InitializeCLR() { Logger::Error("CLR not implemented"); return false; }
void ScriptManager::ShutdownCLR() {}
#endif

bool ScriptManager::LoadAllScripts()
{
    Logger::Debug("Loading all scripts from: %s", m_scriptsPath.c_str());
    std::lock_guard<std::mutex> lock(m_mutex);
    bool allSuccess = true;

    auto enabledDir = std::filesystem::path(m_scriptsPath) / "enabled";
    if (!std::filesystem::exists(enabledDir)) return true;
    for (auto& entry : std::filesystem::recursive_directory_iterator(enabledDir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".cs") {
            if (!LoadScript(entry.path().string()))
                allSuccess = false;
        }
    }

    Logger::Info("Loaded %zu scripts", m_scripts.size());
    return allSuccess;
}

bool ScriptManager::LoadScript(const std::string& filePath)
{
    Logger::Debug("Loading script: %s", filePath.c_str());
    std::ifstream file(filePath);
    if (!file.is_open()) {
        Logger::Error("Cannot open script file: %s", filePath.c_str());
        return false;
    }

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    if (content.empty()) {
        Logger::Warn("Script empty: %s", filePath.c_str());
        return false;
    }

    ScriptInfo scriptInfo;
    scriptInfo.filePath = filePath;
    scriptInfo.content = content;
    scriptInfo.lastModified = std::filesystem::last_write_time(filePath);
    scriptInfo.isLoaded = false;

    if (!CompileScript(scriptInfo)) {
        Logger::Error("Failed to compile: %s", filePath.c_str());
        return false;
    }

    scriptInfo.isLoaded = true;
    m_scripts[filePath] = scriptInfo;
    ExecuteScriptMethod(filePath, "Initialize");
    return true;
}

bool ScriptManager::CompileScript(ScriptInfo& /*scriptInfo*/)
{
#ifdef _WIN32
    // Windows CLR compilation omitted in Linux builds
    Logger::Error("CompileScript: CLR hosting not available in this build");
    return false;
#else
    Logger::Error("CompileScript: CLR hosting not implemented on this platform");
    return false;
#endif
}

void ScriptManager::UnloadAllScripts()
{
    Logger::Debug("Unloading all scripts");
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& kv : m_scripts) {
        if (kv.second.isLoaded)
            ExecuteScriptMethod(kv.first, "Cleanup");
    }
    m_scripts.clear();
}

bool ScriptManager::ReloadScript(const std::string& filePath)
{
    Logger::Info("Reloading script: %s", filePath.c_str());
    UnloadScript(filePath);
    m_scripts.erase(filePath);
    m_reloadCount++;
    return LoadScript(filePath);
}

void ScriptManager::UnloadScript(const std::string& filePath)
{
    auto it = m_scripts.find(filePath);
    if (it != m_scripts.end() && it->second.isLoaded) {
        ExecuteScriptMethod(filePath, "Cleanup");
        it->second.isLoaded = false;
    }
}

void ScriptManager::StartFileWatcher()
{
    if (m_watcherRunning) return;
    m_watcherRunning = true;
    m_watcherThread = std::thread(&ScriptManager::FileWatcherThread, this);
    Logger::Debug("File watcher thread started");
}

void ScriptManager::StopFileWatcher()
{
    if (!m_watcherRunning) return;
    m_watcherRunning = false;
    if (m_watcherThread.joinable())
        m_watcherThread.join();
    Logger::Debug("File watcher thread stopped");
}

void ScriptManager::FileWatcherThread()
{
#ifdef _WIN32
    std::string watchPath = m_scriptsPath + "/enabled";
    HANDLE hDir = CreateFileA(watchPath.c_str(), FILE_LIST_DIRECTORY,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);

    char buffer[4096];
    DWORD bytesReturned;
    while (m_watcherRunning) {
        if (ReadDirectoryChangesA(hDir, buffer, sizeof(buffer), TRUE,
            FILE_NOTIFY_CHANGE_LAST_WRITE|FILE_NOTIFY_CHANGE_FILE_NAME,
            &bytesReturned, nullptr, nullptr)) {
            FILE_NOTIFY_INFORMATION* info = (FILE_NOTIFY_INFORMATION*)buffer;
            std::string name(info->FileName, info->FileNameLength/sizeof(WCHAR));
            if (name.rfind(".cs") != std::string::npos)
                OnFileChanged(watchPath + "/" + name);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CloseHandle(hDir);
#else
    std::string watchPath = m_scriptsPath + "/enabled";
    int fd = inotify_init();
    int wd = inotify_add_watch(fd, watchPath.c_str(),
        IN_MODIFY|IN_CREATE|IN_DELETE|IN_MOVED_TO);
    char buf[4096];
    while (m_watcherRunning) {
        int len = read(fd, buf, sizeof(buf));
        int i = 0;
        while (i < len) {
            auto* ev = (struct inotify_event*)&buf[i];
            if (ev->len && std::string(ev->name).rfind(".cs") != std::string::npos)
                OnFileChanged(watchPath + "/" + ev->name);
            i += sizeof(struct inotify_event) + ev->len;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    inotify_rm_watch(fd, wd);
    close(fd);
#endif
}

void ScriptManager::OnFileChanged(const std::string& filePath)
{
    auto now = std::chrono::steady_clock::now();
    if (now - m_lastReloadTime < std::chrono::milliseconds(500)) return;
    m_lastReloadTime = now;

    Logger::Debug("File changed: %s", filePath.c_str());

    if (!std::filesystem::exists(filePath)) {
        UnloadScript(filePath);
        m_scripts.erase(filePath);
        Logger::Info("Script removed: %s", filePath.c_str());
    } else {
        ReloadScript(filePath);
    }
}

bool ScriptManager::ExecuteScriptMethod(
    const std::string& scriptPath,
    const std::string& methodName,
    const std::vector<std::string>& /*args*/)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_scripts.find(scriptPath);
    if (it == m_scripts.end() || !it->second.isLoaded) return false;
    Logger::Debug("Executing %s.%s()", scriptPath.c_str(), methodName.c_str());
    return true;
}

// FIRE macro for event dispatch
#define FIRE(evt, ...) \
    do { if(!m_isRunning) break; std::lock_guard<std::mutex> lk(m_mutex); \
         for(auto& kv : m_scripts) if(kv.second.isLoaded) \
             ExecuteScriptMethod(kv.first, evt, {__VA_ARGS__}); } while(0)

// Event notifications
void ScriptManager::NotifyServerStart()    { FIRE("OnServerStart"); }
void ScriptManager::NotifyServerShutdown() { FIRE("OnServerShutdown"); }
void ScriptManager::NotifyPlayerJoin(const std::string& id)  { FIRE("OnPlayerJoin", id); }
void ScriptManager::NotifyPlayerLeave(const std::string& id) { FIRE("OnPlayerLeave", id); }
void ScriptManager::NotifyChatMessage(const std::string& p, const std::string& m)
{ FIRE("OnChatMessage", p, m); }
void ScriptManager::NotifyMatchStart()     { FIRE("OnMatchStart"); }
void ScriptManager::NotifyMatchEnd()       { FIRE("OnMatchEnd"); }
void ScriptManager::NotifyPlayerMove(const std::string& p,float x,float y,float z)
{ FIRE("OnPlayerMove", p, std::to_string(x), std::to_string(y), std::to_string(z)); }
void ScriptManager::NotifyPlayerAction(const std::string& p,const std::string& a)
{ FIRE("OnPlayerAction", p, a); }
void ScriptManager::NotifyPlayerKill(const std::string& k,const std::string& v)
{ FIRE("OnPlayerKill", k, v); }
void ScriptManager::NotifyPlayerFrozen(const std::string& p)
{ FIRE("OnPlayerFrozen", p); }
void ScriptManager::NotifyPlayerUnfrozen(const std::string& p)
{ FIRE("OnPlayerUnfrozen", p); }
void ScriptManager::NotifyPlayerStunned(const std::string& p)
{ FIRE("OnPlayerStunned", p); }
void ScriptManager::NotifyPlayerUnstunned(const std::string& p)
{ FIRE("OnPlayerUnstunned", p); }

// EAC callbacks
void ScriptManager::OnRemoteMemoryRead(uint32_t cid, uint64_t addr, const uint8_t* data, uint32_t len)
{
    std::string hex; hex.reserve(len*2);
    char b[3]={};
    for(uint32_t i=0;i<len;i++){ sprintf(b,"%02X",data[i]); hex+=b; }
    FIRE("OnRemoteMemoryRead", std::to_string(cid), std::to_string(addr), std::to_string(len), hex);
}
void ScriptManager::OnRemoteMemoryWriteAck(uint32_t cid, bool ok)
{ FIRE("OnRemoteMemoryWriteAck", std::to_string(cid), ok?"true":"false"); }
void ScriptManager::OnRemoteMemoryAlloc(uint32_t cid, uint64_t baseAddr)
{ FIRE("OnRemoteMemoryAlloc", std::to_string(cid), std::to_string(baseAddr)); }
void ScriptManager::OnBroadcastMemoryRead(uint32_t cid, const uint8_t* data, uint32_t len)
{
    std::string hex; hex.reserve(len*2);
    char b[3]={};
    for(uint32_t i=0;i<len;i++){ sprintf(b,"%02X",data[i]); hex+=b; }
    FIRE("OnBroadcastMemoryRead", std::to_string(cid), std::to_string(len), hex);
}

#undef FIRE
