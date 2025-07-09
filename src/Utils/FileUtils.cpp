#include "Utils/FileUtils.h"
#include <fstream>
#include <sstream>
#include <filesystem>

namespace fs = std::filesystem;

namespace FileUtils {

std::optional<std::vector<uint8_t>> ReadFileBytes(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return std::nullopt;
    auto size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        return std::nullopt;
    }
    return buffer;
}

std::optional<std::string> ReadFileText(const std::string& path, bool trimNewlines) {
    std::ifstream file(path);
    if (!file.is_open()) return std::nullopt;
    std::ostringstream ss;
    ss << file.rdbuf();
    std::string text = ss.str();
    if (trimNewlines) {
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
    }
    return text;
}

bool WriteFileBytes(const std::string& path,
                    const std::vector<uint8_t>& data,
                    bool overwrite) {
    if (!overwrite && fs::exists(path)) return false;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) return false;
    file.write(reinterpret_cast<const char*>(data.data()), data.size());
    return file.good();
}

bool WriteFileText(const std::string& path,
                   const std::string& text,
                   bool overwrite) {
    if (!overwrite && fs::exists(path)) return false;
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) return false;
    file << text;
    return file.good();
}

bool EnsureDirectory(const std::string& path) {
    try {
        fs::path dir = fs::u8path(path);
        if (fs::exists(dir)) {
            return fs::is_directory(dir);
        }
        return fs::create_directories(dir);
    } catch (...) {
        return false;
    }
}

std::optional<std::vector<std::string>> ListFiles(const std::string& directory) {
    try {
        fs::path dir = fs::u8path(directory);
        if (!fs::exists(dir) || !fs::is_directory(dir)) return std::nullopt;
        std::vector<std::string> files;
        for (auto& entry : fs::directory_iterator(dir)) {
            if (entry.is_regular_file()) {
                files.push_back(entry.path().filename().string());
            }
        }
        return files;
    } catch (...) {
        return std::nullopt;
    }
}

bool DeleteFile(const std::string& path) {
    try {
        return fs::remove(fs::u8path(path));
    } catch (...) {
        return false;
    }
}

std::optional<uintmax_t> GetFileSize(const std::string& path) {
    try {
        fs::path p = fs::u8path(path);
        if (!fs::exists(p) || !fs::is_regular_file(p)) return std::nullopt;
        return fs::file_size(p);
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace FileUtils