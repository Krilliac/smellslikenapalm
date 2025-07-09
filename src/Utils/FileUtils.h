#pragma once

#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <cstdint>

namespace FileUtils {

    // Read entire file into a byte buffer. Returns nullopt on failure.
    std::optional<std::vector<uint8_t>> ReadFileBytes(const std::string& path);

    // Read entire text file into a string. Returns nullopt on failure.
    std::optional<std::string> ReadFileText(const std::string& path, bool trimNewlines = true);

    // Write a byte buffer to file. Overwrites by default.
    bool WriteFileBytes(const std::string& path,
                        const std::vector<uint8_t>& data,
                        bool overwrite = true);

    // Write text to file. Overwrites by default.
    bool WriteFileText(const std::string& path,
                       const std::string& text,
                       bool overwrite = true);

    // Ensure the directory for the given path exists (creates recursively)
    bool EnsureDirectory(const std::string& path);

    // List files in a directory (non‐recursive), returns list of filenames.
    std::optional<std::vector<std::string>> ListFiles(const std::string& directory);

    // Delete a file. Returns true on success.
    bool DeleteFile(const std::string& path);

    // Get file size in bytes. Returns nullopt if file does not exist.
    std::optional<uintmax_t> GetFileSize(const std::string& path);
}