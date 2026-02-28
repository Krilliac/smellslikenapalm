#include "Utils/Logger.h"
#include <iostream>
#include <fstream>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <ctime>
#include <sstream>
#include <vector>
#include <string>
#include <cstdio>
#include <algorithm>

struct Logger::Impl {
    std::mutex      mutex;
    std::ofstream   file;
    LogLevel        level = LogLevel::Trace;
    bool            hasFile = false;
    std::string     logFilePath;

    // Structured logging
    bool            structuredLogging = false;

    // Log rotation
    size_t          maxFileSize  = 0;       // 0 means no limit
    size_t          maxLogFiles  = 5;       // keep at most this many rotated files
    size_t          currentFileSize = 0;

    // Remote log shipping
    std::string              remoteEndpoint;
    bool                     remoteShippingEnabled = false;
    std::vector<std::string> remoteBuffer;  // queued messages awaiting shipment
};

std::unique_ptr<Logger::Impl> Logger::s_impl = nullptr;

void Logger::Initialize(const std::string& logFilePath) {
    s_impl = std::make_unique<Impl>();
    if (!logFilePath.empty()) {
        s_impl->logFilePath = logFilePath;
        s_impl->file.open(logFilePath, std::ios::app);
        s_impl->hasFile = s_impl->file.is_open();
        // Track existing file size for rotation
        if (s_impl->hasFile) {
            s_impl->file.seekp(0, std::ios::end);
            s_impl->currentFileSize = static_cast<size_t>(s_impl->file.tellp());
        }
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

// ---------------------------------------------------------------------------
// Structured logging
// ---------------------------------------------------------------------------

void Logger::SetStructuredLogging(bool enabled) {
    if (s_impl) {
        s_impl->structuredLogging = enabled;
    }
}

bool Logger::IsStructuredLogging() {
    if (s_impl) {
        return s_impl->structuredLogging;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Log rotation
// ---------------------------------------------------------------------------

void Logger::SetMaxFileSize(size_t maxBytes) {
    if (s_impl) {
        s_impl->maxFileSize = maxBytes;
    }
}

void Logger::SetMaxLogFiles(size_t maxFiles) {
    if (s_impl) {
        s_impl->maxLogFiles = maxFiles;
    }
}

void Logger::RotateIfNeeded() {
    if (!s_impl) return;
    if (!s_impl->hasFile) return;
    if (s_impl->maxFileSize == 0) return;
    if (s_impl->currentFileSize < s_impl->maxFileSize) return;

    // Close the current file
    s_impl->file.close();

    // Rename existing rotated files, shifting indices up by one.
    // e.g.  app.log.2 -> app.log.3,  app.log.1 -> app.log.2,  app.log -> app.log.1
    const std::string& base = s_impl->logFilePath;
    for (size_t i = s_impl->maxLogFiles; i >= 1; --i) {
        std::string src = (i == 1) ? base : (base + "." + std::to_string(i - 1));
        std::string dst = base + "." + std::to_string(i);

        // Delete the oldest file if we are at the max
        if (i == s_impl->maxLogFiles) {
            std::remove(dst.c_str());
        }
        std::rename(src.c_str(), dst.c_str());
    }

    // Open a fresh log file
    s_impl->file.open(base, std::ios::trunc);
    s_impl->hasFile = s_impl->file.is_open();
    s_impl->currentFileSize = 0;
}

// ---------------------------------------------------------------------------
// Remote log shipping
// ---------------------------------------------------------------------------

void Logger::SetRemoteEndpoint(const std::string& endpoint) {
    if (s_impl) {
        s_impl->remoteEndpoint = endpoint;
    }
}

void Logger::EnableRemoteShipping(bool enabled) {
    if (s_impl) {
        s_impl->remoteShippingEnabled = enabled;
    }
}

// ---------------------------------------------------------------------------
// Core logging
// ---------------------------------------------------------------------------

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

    // Format the timestamp once - reused by both plain and JSON modes
    std::ostringstream tsStream;
    tsStream << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
             << '.' << std::setfill('0') << std::setw(3) << ms.count();
    std::string timestamp = tsStream.str();

    // Format the user message
    constexpr size_t BUFFER_SIZE = 4096;
    char buffer[BUFFER_SIZE];
    vsnprintf(buffer, BUFFER_SIZE, fmt, args);

    // Build the final output line depending on structured logging mode
    std::string outputLine;
    if (s_impl->structuredLogging) {
        // Produce JSON: {"timestamp":"...","level":"...","message":"..."}
        // We need to escape special JSON characters in the message.
        std::string msg(buffer);
        std::string escaped;
        escaped.reserve(msg.size() + 16);
        for (char c : msg) {
            switch (c) {
                case '"':  escaped += "\\\""; break;
                case '\\': escaped += "\\\\"; break;
                case '\n': escaped += "\\n";  break;
                case '\r': escaped += "\\r";  break;
                case '\t': escaped += "\\t";  break;
                default:   escaped += c;      break;
            }
        }

        // Trim trailing spaces from level string for clean JSON
        std::string lvl(LevelToString(level));
        lvl.erase(lvl.find_last_not_of(' ') + 1);

        std::ostringstream json;
        json << "{\"timestamp\":\"" << timestamp
             << "\",\"level\":\"" << lvl
             << "\",\"message\":\"" << escaped
             << "\"}";
        outputLine = json.str();
    } else {
        std::ostringstream plain;
        plain << timestamp << " [" << LevelToString(level) << "] " << buffer;
        outputLine = plain.str();
    }

    std::lock_guard<std::mutex> lock(s_impl->mutex);

    // Check log rotation before writing
    RotateIfNeeded();

    // Always write to console
    std::cout << outputLine << std::endl;

    // Also write to file if available
    if (s_impl->hasFile && s_impl->file.is_open()) {
        s_impl->file << outputLine << std::endl;
        // Track written bytes (line + newline)
        s_impl->currentFileSize += outputLine.size() + 1;
    }

    // Queue for remote shipping if enabled
    if (s_impl->remoteShippingEnabled && !s_impl->remoteEndpoint.empty()) {
        s_impl->remoteBuffer.push_back(outputLine);
        // NOTE: Actual HTTP transmission is not implemented here because no
        // HTTP client library is available. A real implementation would
        // asynchronously POST the contents of remoteBuffer to
        // s_impl->remoteEndpoint and clear the buffer on success.
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
