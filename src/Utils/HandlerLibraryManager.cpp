// src/Utils/HandlerLibraryManager.cpp
#include "Utils/HandlerLibraryManager.h"
#include "Utils/Logger.h"
#include "Protocol/PacketTypes.h"
#include "Protocol/ProtocolUtils.h"
#include <filesystem>
#include <ctime>

namespace GeneratedHandlers {

HandlerLibrary::~HandlerLibrary() {
    Unload();
}

bool HandlerLibrary::Load() {
    if (handle != nullptr) {
        Logger::Warn("HandlerLibrary: Library already loaded");
        return true;
    }
    
    if (!std::filesystem::exists(libraryPath)) {
        Logger::Error("HandlerLibrary: Library file does not exist: %s", libraryPath.c_str());
        return false;
    }
    
#ifdef _WIN32
    handle = LoadLibraryA(libraryPath.c_str());
    if (!handle) {
        DWORD error = GetLastError();
        Logger::Error("HandlerLibrary: Failed to load library %s (error %lu)", 
                     libraryPath.c_str(), error);
        return false;
    }
#else
    // Clear any existing error
    dlerror();
    
    handle = dlopen(libraryPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char* error = dlerror();
        Logger::Error("HandlerLibrary: Failed to load library %s: %s", 
                     libraryPath.c_str(), error ? error : "Unknown error");
        return false;
    }
#endif

    // Load handler functions for each packet type
    const std::vector<std::string> handlerNames = {
        "Handle_HEARTBEAT", "Handle_CHAT_MESSAGE", "Handle_PLAYER_SPAWN",
        "Handle_PLAYER_MOVE", "Handle_PLAYER_ACTION", "Handle_HEALTH_UPDATE",
        "Handle_TEAM_UPDATE", "Handle_SPAWN_ENTITY", "Handle_DESPAWN_ENTITY",
        "Handle_ACTOR_REPLICATION", "Handle_OBJECTIVE_UPDATE", "Handle_SCORE_UPDATE",
        "Handle_SESSION_STATE", "Handle_CHAT_HISTORY", "Handle_ADMIN_COMMAND",
        "Handle_SERVER_NOTIFICATION", "Handle_MAP_CHANGE", "Handle_CONFIG_SYNC",
        "Handle_COMPRESSION", "Handle_RPC_CALL", "Handle_RPC_RESPONSE"
    };
    
    for (const auto& name : handlerNames) {
#ifdef _WIN32
        HandlerFunction func = reinterpret_cast<HandlerFunction>(
            GetProcAddress(handle, name.c_str()));
#else
        dlerror(); // Clear error
        HandlerFunction func = reinterpret_cast<HandlerFunction>(
            dlsym(handle, name.c_str()));
#endif
        
        if (func) {
            handlers[name] = func;
            Logger::Debug("HandlerLibrary: Loaded handler %s", name.c_str());
        } else {
            Logger::Debug("HandlerLibrary: Handler %s not found (optional)", name.c_str());
        }
    }
    
    // Update last modified time
    auto ftime = std::filesystem::last_write_time(libraryPath);
    lastModified = std::chrono::duration_cast<std::chrono::seconds>(
        ftime.time_since_epoch()).count();
    
    Logger::Info("HandlerLibrary: Successfully loaded %zu handlers from %s", 
                handlers.size(), libraryPath.c_str());
    return true;
}

void HandlerLibrary::Unload() {
    if (handle == nullptr) {
        return;
    }
    
    handlers.clear();
    
#ifdef _WIN32
    if (!FreeLibrary(handle)) {
        DWORD error = GetLastError();
        Logger::Error("HandlerLibrary: Failed to unload library (error %lu)", error);
    }
#else
    if (dlclose(handle) != 0) {
        const char* error = dlerror();
        Logger::Error("HandlerLibrary: Failed to unload library: %s", 
                     error ? error : "Unknown error");
    }
#endif
    
    handle = nullptr;
    Logger::Info("HandlerLibrary: Unloaded library %s", libraryPath.c_str());
}

bool HandlerLibrary::Reload() {
    Logger::Info("HandlerLibrary: Reloading library %s", libraryPath.c_str());
    Unload();
    return Load();
}

HandlerFunction HandlerLibrary::GetHandler(const std::string& handlerName) {
    auto it = handlers.find(handlerName);
    return (it != handlers.end()) ? it->second : nullptr;
}

// Singleton implementation
HandlerLibraryManager& HandlerLibraryManager::Instance() {
    static HandlerLibraryManager instance;
    return instance;
}

bool HandlerLibraryManager::Initialize(const std::string& libraryPath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_initialized) {
        Logger::Warn("HandlerLibraryManager: Already initialized");
        return true;
    }
    
    m_library = std::make_unique<HandlerLibrary>();
    m_library->libraryPath = libraryPath;
    
    bool success = m_library->Load();
    if (success) {
        m_initialized = true;
        Logger::Info("HandlerLibraryManager: Initialized with library %s", libraryPath.c_str());
    } else {
        m_library.reset();
        Logger::Error("HandlerLibraryManager: Failed to initialize");
    }
    
    return success;
}

void HandlerLibraryManager::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_library) {
        m_library.reset();
    }
    
    m_initialized = false;
    Logger::Info("HandlerLibraryManager: Shutdown complete");
}

bool HandlerLibraryManager::CheckAndReload() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_initialized || !m_library) {
        return false;
    }
    
    // Check if file has been modified
    if (!std::filesystem::exists(m_library->libraryPath)) {
        Logger::Warn("HandlerLibraryManager: Library file no longer exists: %s", 
                    m_library->libraryPath.c_str());
        return false;
    }
    
    auto ftime = std::filesystem::last_write_time(m_library->libraryPath);
    auto currentTime = std::chrono::duration_cast<std::chrono::seconds>(
        ftime.time_since_epoch()).count();
    
    if (currentTime > m_library->lastModified) {
        Logger::Info("HandlerLibraryManager: Library file modified, reloading...");
        return m_library->Reload();
    }
    
    return true; // No reload needed
}

HandlerFunction HandlerLibraryManager::GetHandler(const std::string& handlerName) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_initialized || !m_library) {
        return nullptr;
    }
    
    return m_library->GetHandler(handlerName);
}

bool HandlerLibraryManager::ForceReload() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_initialized || !m_library) {
        return false;
    }
    
    return m_library->Reload();
}

} // namespace GeneratedHandlers