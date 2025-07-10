// src/Utils/PathUtils.cpp
#include "Utils/PathUtils.h"
#include "Utils/Logger.h"

#ifdef _WIN32
    #include <windows.h>
#elif defined(__linux__)
    #include <unistd.h>
    #include <limits.h>
#elif defined(__APPLE__)
    #include <mach-o/dyld.h>
#endif

namespace PathUtils {

std::string GetExecutablePath() {
#ifdef _WIN32
    char buffer[MAX_PATH];
    DWORD length = GetModuleFileNameA(NULL, buffer, MAX_PATH);
    if (length == 0) {
        Logger::Error("Failed to get executable path on Windows");
        return "";
    }
    return std::string(buffer, length);
    
#elif defined(__linux__)
    char buffer[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", buffer, PATH_MAX - 1);
    if (length == -1) {
        Logger::Error("Failed to get executable path on Linux");
        return "";
    }
    buffer[length] = '\0';
    return std::string(buffer);
    
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(&buffer[0], &size) != 0) {
        Logger::Error("Failed to get executable path on macOS");
        return "";
    }
    buffer.resize(size - 1); // Remove null terminator
    return buffer;
    
#else
    Logger::Error("Unsupported platform for executable path detection");
    return "";
#endif
}

std::string GetExecutableDirectory() {
    std::string execPath = GetExecutablePath();
    if (execPath.empty()) {
        return "";
    }
    
    std::filesystem::path path(execPath);
    return path.parent_path().string();
}

std::string ResolveFromExecutable(const std::string& relativePath) {
    std::string execDir = GetExecutableDirectory();
    if (execDir.empty()) {
        return relativePath; // Fallback to relative path
    }
    
    std::filesystem::path resolved = std::filesystem::path(execDir) / relativePath;
    return resolved.string();
}

} // namespace PathUtils