#include "Utils/Logger.h"
#include <iostream>
#include <fstream>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <ctime>

struct Logger::Impl {
    std::mutex      mutex;
    std::ofstream   file;
    LogLevel        level = LogLevel::Trace;
    bool            hasFile = false;
};

std::unique_ptr<Logger::Impl> Logger::s_impl = nullptr;

void Logger::Initialize(const std::string& logFilePath) {
    s_impl = std::make_unique<Impl>();
    if (!logFilePath.empty()) {
        s_impl->file.open(logFilePath, std::ios::app);
        s_impl->hasFile = s_impl->file.is_open();
    }
    // Always output to console as well
}

void Logger::Shutdown() {
    if (s_impl && s_impl->file.is_open()) {
        s_impl->file.flush();
        s_impl->file.close();
    }
    s_impl.reset();
}

void Logger::SetLevel(LogLevel level) {
    if (s_impl) {
        s_impl->level = level;
    }
}

LogLevel Logger::GetLevel() {
    if (s_impl) {
        return s_impl->level;
    }
    return LogLevel::Info;
}

void Logger::Log(LogLevel level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogV(level, fmt, args);
    va_end(args);
}

void Logger::Trace(const char* fmt, ...) {
    if (s_impl && s_impl->level <= LogLevel::Trace) {
        va_list args; va_start(args, fmt);
        LogV(LogLevel::Trace, fmt, args);
        va_end(args);
    }
}

void Logger::Debug(const char* fmt, ...) {
    if (s_impl && s_impl->level <= LogLevel::Debug) {
        va_list args; va_start(args, fmt);
        LogV(LogLevel::Debug, fmt, args);
        va_end(args);
    }
}

void Logger::Info(const char* fmt, ...) {
    if (s_impl && s_impl->level <= LogLevel::Info) {
        va_list args; va_start(args, fmt);
        LogV(LogLevel::Info, fmt, args);
        va_end(args);
    }
}

void Logger::Warn(const char* fmt, ...) {
    if (s_impl && s_impl->level <= LogLevel::Warn) {
        va_list args; va_start(args, fmt);
        LogV(LogLevel::Warn, fmt, args);
        va_end(args);
    }
}

void Logger::Error(const char* fmt, ...) {
    if (s_impl && s_impl->level <= LogLevel::Error) {
        va_list args; va_start(args, fmt);
        LogV(LogLevel::Error, fmt, args);
        va_end(args);
    }
}

void Logger::Fatal(const char* fmt, ...) {
    if (s_impl) {
        va_list args; va_start(args, fmt);
        LogV(LogLevel::Fatal, fmt, args);
        va_end(args);
        std::abort();
    }
}

void Logger::LogV(LogLevel level, const char* fmt, va_list args) {
    if (!s_impl) return;
    if (level < s_impl->level) return;

    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif

    std::ostringstream header;
    header << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
           << '.' << std::setfill('0') << std::setw(3) << ms.count()
           << " [" << LevelToString(level) << "] ";

    // Format message with a generous buffer for verbose logging
    constexpr size_t BUFFER_SIZE = 4096;
    char buffer[BUFFER_SIZE];
    vsnprintf(buffer, BUFFER_SIZE, fmt, args);

    std::lock_guard<std::mutex> lock(s_impl->mutex);

    // Always write to console
    std::cout << header.str() << buffer << std::endl;

    // Also write to file if available
    if (s_impl->hasFile && s_impl->file.is_open()) {
        s_impl->file << header.str() << buffer << std::endl;
    }
}

const char* Logger::LevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
        default:              return "?????";
    }
}
