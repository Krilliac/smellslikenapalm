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
    bool            toConsole = true;
};

std::unique_ptr<Logger::Impl> Logger::s_impl = nullptr;

void Logger::Initialize(const std::string& logFilePath) {
    s_impl = std::make_unique<Impl>();
    if (!logFilePath.empty()) {
        s_impl->file.open(logFilePath, std::ios::app);
        s_impl->toConsole = false;
    }
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
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif

    std::ostringstream header;
    header << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
           << " [" << LevelToString(level) << "] ";

    std::lock_guard<std::mutex> lock(s_impl->mutex);
    // Write header
    if (s_impl->toConsole) {
        std::cout << header.str();
    } else if (s_impl->file.is_open()) {
        s_impl->file << header.str();
    }
    // Write message
    constexpr size_t BUFFER_SIZE = 1024;
    char buffer[BUFFER_SIZE];
    vsnprintf(buffer, BUFFER_SIZE, fmt, args);
    if (s_impl->toConsole) {
        std::cout << buffer << std::endl;
    } else if (s_impl->file.is_open()) {
        s_impl->file << buffer << std::endl;
    }
}

const char* Logger::LevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
        default:              return "UNKNOWN";
    }
}