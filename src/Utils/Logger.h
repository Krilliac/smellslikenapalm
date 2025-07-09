#pragma once

#include <string>
#include <cstdarg>
#include <memory>

// Logging levels
enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Fatal
};

class Logger {
public:
    // Initialize the logger (e.g., open log file). Call once at startup.
    static void Initialize(const std::string& logFilePath = "");

    // Shutdown the logger (e.g., flush and close file). Call at exit.
    static void Shutdown();

    // Set minimum level to output. Messages below this level are ignored.
    static void SetLevel(LogLevel level);

    // Format and write a message at the given level
    static void Log(LogLevel level, const char* fmt, ...);

    // Level‐specific helpers
    static void Trace(const char* fmt, ...);
    static void Debug(const char* fmt, ...);
    static void Info(const char* fmt, ...);
    static void Warn(const char* fmt, ...);
    static void Error(const char* fmt, ...);
    static void Fatal(const char* fmt, ...);

private:
    struct Impl;
    static std::unique_ptr<Impl> s_impl;

    // Internal vprintf style
    static void LogV(LogLevel level, const char* fmt, va_list args);
    static const char* LevelToString(LogLevel level);
};