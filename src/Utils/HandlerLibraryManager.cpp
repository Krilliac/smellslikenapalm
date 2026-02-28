// src/Utils/HandlerLibraryManager.cpp
#include "Utils/HandlerLibraryManager.h"
#include "Utils/Logger.h"
#include "Protocol/PacketTypes.h"
#include "Protocol/ProtocolUtils.h"
#include <filesystem>
#include <ctime>

namespace GeneratedHandlers {

HandlerLibrary::~HandlerLibrary() {
    Logger::Trace("[HandlerLibrary::~HandlerLibrary] Entry: destructor called, libraryPath='%s'", libraryPath.c_str());
    Unload();
    Logger::Trace("[HandlerLibrary::~HandlerLibrary] Exit: destructor complete");
}

bool HandlerLibrary::Load() {
    Logger::Trace("[HandlerLibrary::Load] Entry: libraryPath='%s', current handle=%p", libraryPath.c_str(), (void*)handle);
    if (handle != nullptr) {
        Logger::Warn("HandlerLibrary: Library already loaded");
        Logger::Debug("[HandlerLibrary::Load] Library handle is non-null (%p), skipping load", (void*)handle);
        Logger::Trace("[HandlerLibrary::Load] Exit: returning true (already loaded)");
        return true;
    }

    if (!std::filesystem::exists(libraryPath)) {
        Logger::Error("HandlerLibrary: Library file does not exist: %s", libraryPath.c_str());
        Logger::Trace("[HandlerLibrary::Load] Exit: returning false (file does not exist)");
        return false;
    }
    Logger::Debug("[HandlerLibrary::Load] Library file exists, proceeding with platform-specific load");

#ifdef _WIN32
    Logger::Debug("[HandlerLibrary::Load] Platform: Windows, calling LoadLibraryA");
    handle = LoadLibraryA(libraryPath.c_str());
    if (!handle) {
        DWORD error = GetLastError();
        Logger::Error("HandlerLibrary: Failed to load library %s (error %lu)",
                     libraryPath.c_str(), error);
        Logger::Trace("[HandlerLibrary::Load] Exit: returning false (LoadLibraryA failed, error=%lu)", error);
        return false;
    }
    Logger::Debug("[HandlerLibrary::Load] LoadLibraryA succeeded, handle=%p", (void*)handle);
#else
    Logger::Debug("[HandlerLibrary::Load] Platform: Unix/Linux, calling dlopen with RTLD_NOW | RTLD_LOCAL");
    // Clear any existing error
    dlerror();

    handle = dlopen(libraryPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char* error = dlerror();
        Logger::Error("HandlerLibrary: Failed to load library %s: %s",
                     libraryPath.c_str(), error ? error : "Unknown error");
        Logger::Trace("[HandlerLibrary::Load] Exit: returning false (dlopen failed)");
        return false;
    }
    Logger::Debug("[HandlerLibrary::Load] dlopen succeeded, handle=%p", (void*)handle);
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

    Logger::Debug("[HandlerLibrary::Load] Attempting to resolve %zu handler symbols", handlerNames.size());
    size_t loadedCount = 0;
    size_t missingCount = 0;
    for (const auto& name : handlerNames) {
        Logger::Trace("[HandlerLibrary::Load] Resolving symbol: '%s'", name.c_str());
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
            loadedCount++;
            Logger::Debug("HandlerLibrary: Loaded handler %s", name.c_str());
        } else {
            missingCount++;
            Logger::Debug("HandlerLibrary: Handler %s not found (optional)", name.c_str());
        }
    }
    Logger::Debug("[HandlerLibrary::Load] Symbol resolution complete: %zu loaded, %zu missing", loadedCount, missingCount);

    // Update last modified time
    auto ftime = std::filesystem::last_write_time(libraryPath);
    lastModified = std::chrono::duration_cast<std::chrono::seconds>(
        ftime.time_since_epoch()).count();
    Logger::Debug("[HandlerLibrary::Load] Last modified timestamp recorded: %lld", (long long)lastModified);

    Logger::Info("HandlerLibrary: Successfully loaded %zu handlers from %s",
                handlers.size(), libraryPath.c_str());
    Logger::Trace("[HandlerLibrary::Load] Exit: returning true (load successful)");
    return true;
}

void HandlerLibrary::Unload() {
    Logger::Trace("[HandlerLibrary::Unload] Entry: libraryPath='%s', handle=%p, handlers.size()=%zu",
                  libraryPath.c_str(), (void*)handle, handlers.size());
    if (handle == nullptr) {
        Logger::Debug("[HandlerLibrary::Unload] Handle is null, nothing to unload");
        Logger::Trace("[HandlerLibrary::Unload] Exit: no-op (handle was null)");
        return;
    }

    size_t handlerCount = handlers.size();
    handlers.clear();
    Logger::Debug("[HandlerLibrary::Unload] Cleared %zu handler entries", handlerCount);

#ifdef _WIN32
    Logger::Debug("[HandlerLibrary::Unload] Platform: Windows, calling FreeLibrary");
    if (!FreeLibrary(handle)) {
        DWORD error = GetLastError();
        Logger::Error("HandlerLibrary: Failed to unload library (error %lu)", error);
    } else {
        Logger::Debug("[HandlerLibrary::Unload] FreeLibrary succeeded");
    }
#else
    Logger::Debug("[HandlerLibrary::Unload] Platform: Unix/Linux, calling dlclose");
    if (dlclose(handle) != 0) {
        const char* error = dlerror();
        Logger::Error("HandlerLibrary: Failed to unload library: %s",
                     error ? error : "Unknown error");
    } else {
        Logger::Debug("[HandlerLibrary::Unload] dlclose succeeded");
    }
#endif

    handle = nullptr;
    Logger::Info("HandlerLibrary: Unloaded library %s", libraryPath.c_str());
    Logger::Trace("[HandlerLibrary::Unload] Exit: library unloaded, handle set to nullptr");
}

bool HandlerLibrary::Reload() {
    Logger::Trace("[HandlerLibrary::Reload] Entry: libraryPath='%s'", libraryPath.c_str());
    Logger::Info("HandlerLibrary: Reloading library %s", libraryPath.c_str());
    Logger::Debug("[HandlerLibrary::Reload] Step 1: Unloading current library");
    Unload();
    Logger::Debug("[HandlerLibrary::Reload] Step 2: Loading library fresh");
    bool result = Load();
    Logger::Debug("[HandlerLibrary::Reload] Reload result: %s", result ? "success" : "failure");
    Logger::Trace("[HandlerLibrary::Reload] Exit: returning %s", result ? "true" : "false");
    return result;
}

HandlerFunction HandlerLibrary::GetHandler(const std::string& handlerName) {
    Logger::Trace("[HandlerLibrary::GetHandler] Entry: handlerName='%s'", handlerName.c_str());
    auto it = handlers.find(handlerName);
    if (it != handlers.end()) {
        Logger::Debug("[HandlerLibrary::GetHandler] Handler '%s' found in map", handlerName.c_str());
        Logger::Trace("[HandlerLibrary::GetHandler] Exit: returning valid function pointer");
        return it->second;
    } else {
        Logger::Debug("[HandlerLibrary::GetHandler] Handler '%s' not found in map (map size=%zu)", handlerName.c_str(), handlers.size());
        Logger::Trace("[HandlerLibrary::GetHandler] Exit: returning nullptr");
        return nullptr;
    }
}

// Singleton implementation
HandlerLibraryManager& HandlerLibraryManager::Instance() {
    Logger::Trace("[HandlerLibraryManager::Instance] Entry: accessing singleton instance");
    static HandlerLibraryManager instance;
    Logger::Trace("[HandlerLibraryManager::Instance] Exit: returning singleton reference");
    return instance;
}

bool HandlerLibraryManager::Initialize(const std::string& libraryPath) {
    Logger::Trace("[HandlerLibraryManager::Initialize] Entry: libraryPath='%s'", libraryPath.c_str());
    std::lock_guard<std::mutex> lock(m_mutex);
    Logger::Debug("[HandlerLibraryManager::Initialize] Mutex acquired");

    if (m_initialized) {
        Logger::Warn("HandlerLibraryManager: Already initialized");
        Logger::Trace("[HandlerLibraryManager::Initialize] Exit: returning true (already initialized)");
        return true;
    }

    Logger::Debug("[HandlerLibraryManager::Initialize] Creating new HandlerLibrary instance");
    m_library = std::make_unique<HandlerLibrary>();
    m_library->libraryPath = libraryPath;
    Logger::Debug("[HandlerLibraryManager::Initialize] HandlerLibrary created, libraryPath set to '%s'", libraryPath.c_str());

    bool success = m_library->Load();
    if (success) {
        m_initialized = true;
        Logger::Info("HandlerLibraryManager: Initialized with library %s", libraryPath.c_str());
    } else {
        m_library.reset();
        Logger::Error("HandlerLibraryManager: Failed to initialize");
        Logger::Debug("[HandlerLibraryManager::Initialize] Library load failed, m_library reset to nullptr");
    }

    Logger::Trace("[HandlerLibraryManager::Initialize] Exit: returning %s", success ? "true" : "false");
    return success;
}

void HandlerLibraryManager::Shutdown() {
    Logger::Trace("[HandlerLibraryManager::Shutdown] Entry");
    std::lock_guard<std::mutex> lock(m_mutex);
    Logger::Debug("[HandlerLibraryManager::Shutdown] Mutex acquired, m_initialized=%s, m_library=%p",
                  m_initialized ? "true" : "false", (void*)m_library.get());

    if (m_library) {
        Logger::Debug("[HandlerLibraryManager::Shutdown] Resetting library (releasing unique_ptr)");
        m_library.reset();
    } else {
        Logger::Debug("[HandlerLibraryManager::Shutdown] No library to reset (m_library is null)");
    }

    m_initialized = false;
    Logger::Info("HandlerLibraryManager: Shutdown complete");
    Logger::Trace("[HandlerLibraryManager::Shutdown] Exit: shutdown complete, m_initialized=false");
}

bool HandlerLibraryManager::CheckAndReload() {
    Logger::Trace("[HandlerLibraryManager::CheckAndReload] Entry");
    std::lock_guard<std::mutex> lock(m_mutex);
    Logger::Debug("[HandlerLibraryManager::CheckAndReload] Mutex acquired, m_initialized=%s", m_initialized ? "true" : "false");

    if (!m_initialized || !m_library) {
        Logger::Debug("[HandlerLibraryManager::CheckAndReload] Not initialized or library is null, cannot check for reload");
        Logger::Trace("[HandlerLibraryManager::CheckAndReload] Exit: returning false (not initialized)");
        return false;
    }

    // Check if file has been modified
    if (!std::filesystem::exists(m_library->libraryPath)) {
        Logger::Warn("HandlerLibraryManager: Library file no longer exists: %s",
                    m_library->libraryPath.c_str());
        Logger::Trace("[HandlerLibraryManager::CheckAndReload] Exit: returning false (file missing)");
        return false;
    }

    auto ftime = std::filesystem::last_write_time(m_library->libraryPath);
    auto currentTime = std::chrono::duration_cast<std::chrono::seconds>(
        ftime.time_since_epoch()).count();
    Logger::Debug("[HandlerLibraryManager::CheckAndReload] File last modified time: %lld, recorded last modified: %lld",
                  (long long)currentTime, (long long)m_library->lastModified);

    if (currentTime > m_library->lastModified) {
        Logger::Info("HandlerLibraryManager: Library file modified, reloading...");
        Logger::Debug("[HandlerLibraryManager::CheckAndReload] Modification detected (current=%lld > recorded=%lld), triggering reload",
                      (long long)currentTime, (long long)m_library->lastModified);
        bool result = m_library->Reload();
        Logger::Trace("[HandlerLibraryManager::CheckAndReload] Exit: returning %s (after reload)", result ? "true" : "false");
        return result;
    }

    Logger::Debug("[HandlerLibraryManager::CheckAndReload] No modification detected, no reload needed");
    Logger::Trace("[HandlerLibraryManager::CheckAndReload] Exit: returning true (no reload needed)");
    return true; // No reload needed
}

HandlerFunction HandlerLibraryManager::GetHandler(const std::string& handlerName) {
    Logger::Trace("[HandlerLibraryManager::GetHandler] Entry: handlerName='%s'", handlerName.c_str());
    std::lock_guard<std::mutex> lock(m_mutex);
    Logger::Debug("[HandlerLibraryManager::GetHandler] Mutex acquired, m_initialized=%s", m_initialized ? "true" : "false");

    if (!m_initialized || !m_library) {
        Logger::Warn("[HandlerLibraryManager::GetHandler] Not initialized or library is null, cannot get handler '%s'", handlerName.c_str());
        Logger::Trace("[HandlerLibraryManager::GetHandler] Exit: returning nullptr (not initialized)");
        return nullptr;
    }

    HandlerFunction fn = m_library->GetHandler(handlerName);
    if (fn) {
        Logger::Debug("[HandlerLibraryManager::GetHandler] Handler '%s' resolved successfully", handlerName.c_str());
    } else {
        Logger::Debug("[HandlerLibraryManager::GetHandler] Handler '%s' not found in library", handlerName.c_str());
    }
    Logger::Trace("[HandlerLibraryManager::GetHandler] Exit: returning %s", fn ? "valid function pointer" : "nullptr");
    return fn;
}

bool HandlerLibraryManager::ForceReload() {
    Logger::Trace("[HandlerLibraryManager::ForceReload] Entry");
    std::lock_guard<std::mutex> lock(m_mutex);
    Logger::Debug("[HandlerLibraryManager::ForceReload] Mutex acquired, m_initialized=%s", m_initialized ? "true" : "false");

    if (!m_initialized || !m_library) {
        Logger::Warn("[HandlerLibraryManager::ForceReload] Not initialized or library is null, cannot force reload");
        Logger::Trace("[HandlerLibraryManager::ForceReload] Exit: returning false (not initialized)");
        return false;
    }

    Logger::Info("[HandlerLibraryManager::ForceReload] Force reloading library '%s'", m_library->libraryPath.c_str());
    bool result = m_library->Reload();
    if (result) {
        Logger::Info("[HandlerLibraryManager::ForceReload] Force reload succeeded");
    } else {
        Logger::Error("[HandlerLibraryManager::ForceReload] Force reload failed");
    }
    Logger::Trace("[HandlerLibraryManager::ForceReload] Exit: returning %s", result ? "true" : "false");
    return result;
}

} // namespace GeneratedHandlers
