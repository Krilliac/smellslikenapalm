#pragma once

#include <string>
#include <cstdarg>
#include <memory>
#include <vector>

// printf-style format-string checking under GCC/Clang. These are static member
// functions (no implicit 'this'), so the format string is argument 1 and the
// variadic arguments begin at argument 2 — i.e. RS2V_PRINTF_FORMAT(1, 2).
// Use the gnu_printf archetype: on MinGW the plain "printf" archetype maps to
// ms_printf, which rejects C99 length modifiers (%zu, %llu, %lld) and produces
// hundreds of false positives. gnu_printf validates against the C99/glibc specs
// the code actually uses, on both Linux GCC and MinGW, while still catching real
// format/argument mismatches.
#if defined(__GNUC__)
  #define RS2V_PRINTF_FORMAT(fmtIdx, argIdx) __attribute__((format(gnu_printf, fmtIdx, argIdx)))
#else
  #define RS2V_PRINTF_FORMAT(fmtIdx, argIdx)
#endif

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
    // If logFilePath is non-empty, logs to both console AND file.
    static void Initialize(const std::string& logFilePath = "");

    // Shutdown the logger (e.g., flush and close file). Call at exit.
    static void Shutdown();

    // Set minimum level to output. Messages below this level are ignored.
    static void SetLevel(LogLevel level);

    // Get current minimum log level
    static LogLevel GetLevel();

    // Format and write a message at the given level
    static void Log(LogLevel level, const char* fmt, ...);

    // Level-specific helpers
    static void Trace(const char* fmt, ...) RS2V_PRINTF_FORMAT(1, 2);
    static void Debug(const char* fmt, ...) RS2V_PRINTF_FORMAT(1, 2);
    static void Info(const char* fmt, ...)  RS2V_PRINTF_FORMAT(1, 2);
    static void Warn(const char* fmt, ...)  RS2V_PRINTF_FORMAT(1, 2);
    static void Error(const char* fmt, ...) RS2V_PRINTF_FORMAT(1, 2);
    static void Fatal(const char* fmt, ...) RS2V_PRINTF_FORMAT(1, 2);

    // Structured logging - outputs in JSON format
    static void SetStructuredLogging(bool enabled);
    static bool IsStructuredLogging();

    // Log rotation
    static void SetMaxFileSize(size_t maxBytes);
    static void SetMaxLogFiles(size_t maxFiles);
    static void RotateIfNeeded();

    // Remote log shipping
    static void SetRemoteEndpoint(const std::string& endpoint);
    static void EnableRemoteShipping(bool enabled);

private:
    struct Impl;
    static std::unique_ptr<Impl> s_impl;

    // Internal vprintf style
    static void LogV(LogLevel level, const char* fmt, va_list args);
    static const char* LevelToString(LogLevel level);
};
