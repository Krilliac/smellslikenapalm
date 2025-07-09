#pragma once

#include <string>
#include <vector>
#include <optional>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace StringUtils {

    // Trim whitespace from start and end
    std::string Trim(const std::string& s);

    // Split string by delimiter (single char), include empty tokens if requested
    std::vector<std::string> Split(const std::string& s, char delim, bool keepEmpty = false);

    // Join vector of strings with delimiter
    std::string Join(const std::vector<std::string>& parts, const std::string& delim);

    // Convert to lowercase/uppercase
    std::string ToLower(const std::string& s);
    std::string ToUpper(const std::string& s);

    // Case‐insensitive compare
    bool EqualsIgnoreCase(const std::string& a, const std::string& b);

    // Check starts/ends with
    bool StartsWith(const std::string& s, const std::string& prefix);
    bool EndsWith(const std::string& s, const std::string& suffix);

    // Replace all occurrences of from→to
    std::string ReplaceAll(const std::string& s,
                           const std::string& from,
                           const std::string& to);

    // Convert numeric types to string
    template<typename T>
    std::string ToString(const T& value) {
        std::ostringstream oss;
        oss << value;
        return oss.str();
    }

    // Parse string to integer/double; returns nullopt on failure
    std::optional<int>    ToInt(const std::string& s);
    std::optional<double> ToDouble(const std::string& s);

    // Trim functions for elements in vector
    void TrimInPlace(std::vector<std::string>& parts);

    // Remove all whitespace
    std::string RemoveWhitespace(const std::string& s);
}