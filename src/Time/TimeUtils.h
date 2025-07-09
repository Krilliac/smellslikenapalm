#pragma once

#include <chrono>
#include <string>
#include <sstream>
#include <iomanip>
#include <ctime>

namespace TimeUtils {

    using SteadyClock = std::chrono::steady_clock;
    using SystemClock = std::chrono::system_clock;
    using Millis       = std::chrono::milliseconds;
    using TimePoint    = SteadyClock::time_point;

    // Format a system_clock::time_point as "YYYY-MM-DD HH:MM:SS"
    inline std::string ToString(const SystemClock::time_point& tp) {
        std::time_t t = SystemClock::to_time_t(tp);
        std::tm tm;
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        std::ostringstream oss;
        oss << std::setfill('0')
            << std::setw(4) << (tm.tm_year + 1900) << '-'
            << std::setw(2) << (tm.tm_mon + 1)     << '-'
            << std::setw(2) << tm.tm_mday          << ' '
            << std::setw(2) << tm.tm_hour          << ':'
            << std::setw(2) << tm.tm_min           << ':'
            << std::setw(2) << tm.tm_sec;
        return oss.str();
    }

    // Parse "YYYY-MM-DD HH:MM:SS" into system_clock::time_point; returns nullopt on failure
    inline std::optional<SystemClock::time_point> Parse(const std::string& s) {
        std::tm tm = {};
        char sep1, sep2, sep3, sep4, sep5;
        std::istringstream iss(s);
        iss >> std::setw(4) >> tm.tm_year >> sep1
            >> std::setw(2) >> tm.tm_mon  >> sep2
            >> std::setw(2) >> tm.tm_mday >> sep3
            >> std::setw(2) >> tm.tm_hour >> sep4
            >> std::setw(2) >> tm.tm_min  >> sep5
            >> std::setw(2) >> tm.tm_sec;
        if (iss.fail() || sep1!='-'||sep2!='-'||sep3!=' '||sep4!=':'||sep5!=':') {
            return std::nullopt;
        }
        tm.tm_year -= 1900;
        tm.tm_mon  -= 1;
        auto tt = std::mktime(&tm);
        if (tt == -1) return std::nullopt;
        return SystemClock::from_time_t(tt);
    }

    // Convert steady_clock duration to milliseconds count
    inline int64_t ToMillis(const SteadyClock::duration& d) {
        return std::chrono::duration_cast<Millis>(d).count();
    }

    // Sleep for given milliseconds
    inline void SleepMillis(int64_t ms) {
        std::this_thread::sleep_for(Millis(ms));
    }

    // Get formatted uptime since given start time
    inline std::string FormatUptime(const TimePoint& start) {
        auto now = SteadyClock::now();
        auto diff = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
        int days    = static_cast<int>(diff / 86400);
        diff %= 86400;
        int hours   = static_cast<int>(diff / 3600);
        diff %= 3600;
        int minutes = static_cast<int>(diff / 60);
        int seconds = static_cast<int>(diff % 60);
        std::ostringstream oss;
        if (days)    oss << days << "d ";
        if (hours)   oss << hours << "h ";
        if (minutes) oss << minutes << "m ";
        oss << seconds << "s";
        return oss.str();
    }

}