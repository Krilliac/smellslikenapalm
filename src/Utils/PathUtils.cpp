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
    Logger::Trace("[PathUtils::GetExecutablePath] Entry");
#ifdef _WIN32
    Logger::Debug("[PathUtils::GetExecutablePath] Platform: Windows, using GetModuleFileNameA");
    char buffer[MAX_PATH];
    DWORD length = GetModuleFileNameA(NULL, buffer, MAX_PATH);
    if (length == 0) {
        DWORD err = GetLastError();
        Logger::Error("[PathUtils::GetExecutablePath] Failed to get executable path on Windows, GetLastError=%lu", err);
        Logger::Trace("[PathUtils::GetExecutablePath] Exit: returning empty string");
        return "";
    }
    std::string result(buffer, length);
    Logger::Info("[PathUtils::GetExecutablePath] Executable path resolved: '%s' (length=%lu)", result.c_str(), length);
    Logger::Trace("[PathUtils::GetExecutablePath] Exit: returning path of length %lu", length);
    return result;

#elif defined(__linux__)
    Logger::Debug("[PathUtils::GetExecutablePath] Platform: Linux, reading /proc/self/exe via readlink");
    char buffer[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", buffer, PATH_MAX - 1);
    if (length == -1) {
        Logger::Error("[PathUtils::GetExecutablePath] Failed to get executable path on Linux, readlink returned -1, errno=%d", errno);
        Logger::Trace("[PathUtils::GetExecutablePath] Exit: returning empty string");
        return "";
    }
    buffer[length] = '\0';
    std::string result(buffer);
    Logger::Info("[PathUtils::GetExecutablePath] Executable path resolved: '%s' (length=%zd)", result.c_str(), length);
    Logger::Trace("[PathUtils::GetExecutablePath] Exit: returning '%s'", result.c_str());
    return result;

#elif defined(__APPLE__)
    Logger::Debug("[PathUtils::GetExecutablePath] Platform: macOS, using _NSGetExecutablePath");
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    Logger::Debug("[PathUtils::GetExecutablePath] Required buffer size from _NSGetExecutablePath: %u bytes", size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(&buffer[0], &size) != 0) {
        Logger::Error("[PathUtils::GetExecutablePath] Failed to get executable path on macOS, _NSGetExecutablePath returned non-zero");
        Logger::Trace("[PathUtils::GetExecutablePath] Exit: returning empty string");
        return "";
    }
    buffer.resize(size - 1); // Remove null terminator
    Logger::Info("[PathUtils::GetExecutablePath] Executable path resolved: '%s' (size=%u)", buffer.c_str(), size);
    Logger::Trace("[PathUtils::GetExecutablePath] Exit: returning '%s'", buffer.c_str());
    return buffer;

#else
    Logger::Error("[PathUtils::GetExecutablePath] Unsupported platform for executable path detection");
    Logger::Trace("[PathUtils::GetExecutablePath] Exit: returning empty string (unsupported platform)");
    return "";
#endif
}

std::string GetExecutableDirectory() {
    Logger::Trace("[PathUtils::GetExecutableDirectory] Entry");
    std::string execPath = GetExecutablePath();
    if (execPath.empty()) {
        Logger::Warn("[PathUtils::GetExecutableDirectory] GetExecutablePath returned empty string, cannot determine directory");
        Logger::Trace("[PathUtils::GetExecutableDirectory] Exit: returning empty string");
        return "";
    }

    std::filesystem::path path(execPath);
    std::string result = path.parent_path().string();
    Logger::Debug("[PathUtils::GetExecutableDirectory] Extracted parent directory from executable path: '%s' -> '%s'", execPath.c_str(), result.c_str());
    Logger::Info("[PathUtils::GetExecutableDirectory] Executable directory: '%s'", result.c_str());
    Logger::Trace("[PathUtils::GetExecutableDirectory] Exit: returning '%s'", result.c_str());
    return result;
}

std::string ResolveFromExecutable(const std::string& relativePath) {
    Logger::Trace("[PathUtils::ResolveFromExecutable] Entry: relativePath='%s'", relativePath.c_str());
    std::string execDir = GetExecutableDirectory();
    if (execDir.empty()) {
        Logger::Warn("[PathUtils::ResolveFromExecutable] Executable directory is empty, falling back to relative path: '%s'", relativePath.c_str());
        Logger::Trace("[PathUtils::ResolveFromExecutable] Exit: returning '%s' (fallback to relative path)", relativePath.c_str());
        return relativePath; // Fallback to relative path
    }

    std::filesystem::path resolved = std::filesystem::path(execDir) / relativePath;
    std::string result = resolved.string();
    Logger::Debug("[PathUtils::ResolveFromExecutable] Resolved: execDir='%s' + relativePath='%s' -> '%s'", execDir.c_str(), relativePath.c_str(), result.c_str());
    Logger::Info("[PathUtils::ResolveFromExecutable] Path resolved successfully: '%s'", result.c_str());
    Logger::Trace("[PathUtils::ResolveFromExecutable] Exit: returning '%s'", result.c_str());
    return result;
}

} // namespace PathUtils
