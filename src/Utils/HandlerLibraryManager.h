// src/Utils/HandlerLibraryManager.h
#pragma once

#include <string>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include "Utils/PacketAnalysis.h"

#ifdef _WIN32
    #include <windows.h>
    using LibHandle = HMODULE;
#else
    #include <dlfcn.h>
    using LibHandle = void*;
#endif

namespace GeneratedHandlers {
    // Function pointer types for each handler
    using HandlerFunction = void(*)(const PacketAnalysisResult&);
    
    struct HandlerLibrary {
        LibHandle handle = nullptr;
        std::string libraryPath;
        std::unordered_map<std::string, HandlerFunction> handlers;
        std::time_t lastModified = 0;
        
        ~HandlerLibrary();
        bool Load();
        void Unload();
        bool Reload();
        HandlerFunction GetHandler(const std::string& handlerName);
    };
    
    class HandlerLibraryManager {
    public:
        static HandlerLibraryManager& Instance();
        
        bool Initialize(const std::string& libraryPath);
        void Shutdown();
        
        // Check if library needs reloading and do it if necessary
        bool CheckAndReload();
        
        // Get a handler function by name. When no dynamic library is loaded
        // (the default, recommended configuration) this resolves the handler
        // from the statically-compiled generated registry instead.
        HandlerFunction GetHandler(const std::string& handlerName);

        // Manually trigger a reload of the dynamic library (no-op + warning
        // when no dynamic library has been initialized).
        bool ForceReload();

        // True when the statically-generated handler registry is available
        // (i.e. the codegen output was compiled into the binary).
        bool HasStaticHandlers() const;

    private:
        HandlerLibraryManager() = default;
        ~HandlerLibraryManager() = default;
        
        std::unique_ptr<HandlerLibrary> m_library;
        std::mutex m_mutex;
        std::atomic<bool> m_initialized{false};
    };
}