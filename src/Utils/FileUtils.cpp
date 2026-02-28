#include "Utils/FileUtils.h"
#include "Utils/Logger.h"
#include <fstream>
#include <sstream>
#include <filesystem>

namespace fs = std::filesystem;

namespace FileUtils {

std::optional<std::vector<uint8_t>> ReadFileBytes(const std::string& path) {
    Logger::Trace("[FileUtils::ReadFileBytes] Entry: path='%s'", path.c_str());
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        Logger::Error("[FileUtils::ReadFileBytes] Failed to open file for binary reading: '%s'", path.c_str());
        Logger::Trace("[FileUtils::ReadFileBytes] Exit: returning std::nullopt (file open failed)");
        return std::nullopt;
    }
    auto size = file.tellg();
    Logger::Debug("[FileUtils::ReadFileBytes] File opened successfully, size=%lld bytes", (long long)size);
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        Logger::Error("[FileUtils::ReadFileBytes] Failed to read %lld bytes from file: '%s'", (long long)size, path.c_str());
        Logger::Trace("[FileUtils::ReadFileBytes] Exit: returning std::nullopt (read failed)");
        return std::nullopt;
    }
    Logger::Info("[FileUtils::ReadFileBytes] Successfully read %lld bytes from '%s'", (long long)size, path.c_str());
    Logger::Trace("[FileUtils::ReadFileBytes] Exit: returning buffer of size %zu", buffer.size());
    return buffer;
}

std::optional<std::string> ReadFileText(const std::string& path, bool trimNewlines) {
    Logger::Trace("[FileUtils::ReadFileText] Entry: path='%s', trimNewlines=%s", path.c_str(), trimNewlines ? "true" : "false");
    std::ifstream file(path);
    if (!file.is_open()) {
        Logger::Error("[FileUtils::ReadFileText] Failed to open file for text reading: '%s'", path.c_str());
        Logger::Trace("[FileUtils::ReadFileText] Exit: returning std::nullopt (file open failed)");
        return std::nullopt;
    }
    Logger::Debug("[FileUtils::ReadFileText] File opened successfully for text reading: '%s'", path.c_str());
    std::ostringstream ss;
    ss << file.rdbuf();
    std::string text = ss.str();
    Logger::Debug("[FileUtils::ReadFileText] Read %zu characters from file '%s'", text.size(), path.c_str());
    if (trimNewlines) {
        size_t originalSize = text.size();
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        Logger::Debug("[FileUtils::ReadFileText] Trimmed %zu trailing newline characters", originalSize - text.size());
    } else {
        Logger::Debug("[FileUtils::ReadFileText] Skipping newline trimming (trimNewlines=false)");
    }
    Logger::Info("[FileUtils::ReadFileText] Successfully read text file '%s' (%zu characters)", path.c_str(), text.size());
    Logger::Trace("[FileUtils::ReadFileText] Exit: returning text of length %zu", text.size());
    return text;
}

bool WriteFileBytes(const std::string& path,
                    const std::vector<uint8_t>& data,
                    bool overwrite) {
    Logger::Trace("[FileUtils::WriteFileBytes] Entry: path='%s', data.size()=%zu, overwrite=%s",
                  path.c_str(), data.size(), overwrite ? "true" : "false");
    if (!overwrite && fs::exists(path)) {
        Logger::Warn("[FileUtils::WriteFileBytes] File already exists and overwrite=false, refusing to write: '%s'", path.c_str());
        Logger::Trace("[FileUtils::WriteFileBytes] Exit: returning false (file exists, no overwrite)");
        return false;
    }
    if (!overwrite) {
        Logger::Debug("[FileUtils::WriteFileBytes] File does not exist, proceeding with write: '%s'", path.c_str());
    } else {
        Logger::Debug("[FileUtils::WriteFileBytes] Overwrite enabled, proceeding with write: '%s'", path.c_str());
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        Logger::Error("[FileUtils::WriteFileBytes] Failed to open file for binary writing: '%s'", path.c_str());
        Logger::Trace("[FileUtils::WriteFileBytes] Exit: returning false (file open failed)");
        return false;
    }
    file.write(reinterpret_cast<const char*>(data.data()), data.size());
    bool success = file.good();
    if (success) {
        Logger::Info("[FileUtils::WriteFileBytes] Successfully wrote %zu bytes to '%s'", data.size(), path.c_str());
    } else {
        Logger::Error("[FileUtils::WriteFileBytes] Write operation failed for '%s' after writing %zu bytes", path.c_str(), data.size());
    }
    Logger::Trace("[FileUtils::WriteFileBytes] Exit: returning %s", success ? "true" : "false");
    return success;
}

bool WriteFileText(const std::string& path,
                   const std::string& text,
                   bool overwrite) {
    Logger::Trace("[FileUtils::WriteFileText] Entry: path='%s', text.size()=%zu, overwrite=%s",
                  path.c_str(), text.size(), overwrite ? "true" : "false");
    if (!overwrite && fs::exists(path)) {
        Logger::Warn("[FileUtils::WriteFileText] File already exists and overwrite=false, refusing to write: '%s'", path.c_str());
        Logger::Trace("[FileUtils::WriteFileText] Exit: returning false (file exists, no overwrite)");
        return false;
    }
    if (!overwrite) {
        Logger::Debug("[FileUtils::WriteFileText] File does not exist, proceeding with write: '%s'", path.c_str());
    } else {
        Logger::Debug("[FileUtils::WriteFileText] Overwrite enabled, proceeding with write: '%s'", path.c_str());
    }
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) {
        Logger::Error("[FileUtils::WriteFileText] Failed to open file for text writing: '%s'", path.c_str());
        Logger::Trace("[FileUtils::WriteFileText] Exit: returning false (file open failed)");
        return false;
    }
    file << text;
    bool success = file.good();
    if (success) {
        Logger::Info("[FileUtils::WriteFileText] Successfully wrote %zu characters to '%s'", text.size(), path.c_str());
    } else {
        Logger::Error("[FileUtils::WriteFileText] Write operation failed for '%s' after writing %zu characters", path.c_str(), text.size());
    }
    Logger::Trace("[FileUtils::WriteFileText] Exit: returning %s", success ? "true" : "false");
    return success;
}

bool EnsureDirectory(const std::string& path) {
    Logger::Trace("[FileUtils::EnsureDirectory] Entry: path='%s'", path.c_str());
    try {
        fs::path dir = fs::u8path(path);
        if (fs::exists(dir)) {
            bool isDir = fs::is_directory(dir);
            if (isDir) {
                Logger::Debug("[FileUtils::EnsureDirectory] Directory already exists: '%s'", path.c_str());
            } else {
                Logger::Warn("[FileUtils::EnsureDirectory] Path exists but is not a directory: '%s'", path.c_str());
            }
            Logger::Trace("[FileUtils::EnsureDirectory] Exit: returning %s (path exists)", isDir ? "true" : "false");
            return isDir;
        }
        Logger::Debug("[FileUtils::EnsureDirectory] Directory does not exist, creating: '%s'", path.c_str());
        bool created = fs::create_directories(dir);
        if (created) {
            Logger::Info("[FileUtils::EnsureDirectory] Successfully created directory: '%s'", path.c_str());
        } else {
            Logger::Warn("[FileUtils::EnsureDirectory] create_directories returned false for: '%s'", path.c_str());
        }
        Logger::Trace("[FileUtils::EnsureDirectory] Exit: returning %s", created ? "true" : "false");
        return created;
    } catch (const std::exception& ex) {
        Logger::Error("[FileUtils::EnsureDirectory] Exception while ensuring directory '%s': %s", path.c_str(), ex.what());
        Logger::Trace("[FileUtils::EnsureDirectory] Exit: returning false (exception caught)");
        return false;
    } catch (...) {
        Logger::Error("[FileUtils::EnsureDirectory] Unknown exception while ensuring directory '%s'", path.c_str());
        Logger::Trace("[FileUtils::EnsureDirectory] Exit: returning false (unknown exception caught)");
        return false;
    }
}

std::optional<std::vector<std::string>> ListFiles(const std::string& directory) {
    Logger::Trace("[FileUtils::ListFiles] Entry: directory='%s'", directory.c_str());
    try {
        fs::path dir = fs::u8path(directory);
        if (!fs::exists(dir) || !fs::is_directory(dir)) {
            Logger::Warn("[FileUtils::ListFiles] Path does not exist or is not a directory: '%s'", directory.c_str());
            Logger::Trace("[FileUtils::ListFiles] Exit: returning std::nullopt (not a valid directory)");
            return std::nullopt;
        }
        Logger::Debug("[FileUtils::ListFiles] Directory validated, iterating entries in '%s'", directory.c_str());
        std::vector<std::string> files;
        for (auto& entry : fs::directory_iterator(dir)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                files.push_back(filename);
                Logger::Trace("[FileUtils::ListFiles] Found regular file: '%s'", filename.c_str());
            } else {
                Logger::Trace("[FileUtils::ListFiles] Skipping non-regular entry: '%s'", entry.path().filename().string().c_str());
            }
        }
        Logger::Info("[FileUtils::ListFiles] Listed %zu files in directory '%s'", files.size(), directory.c_str());
        Logger::Trace("[FileUtils::ListFiles] Exit: returning vector of %zu filenames", files.size());
        return files;
    } catch (const std::exception& ex) {
        Logger::Error("[FileUtils::ListFiles] Exception while listing files in '%s': %s", directory.c_str(), ex.what());
        Logger::Trace("[FileUtils::ListFiles] Exit: returning std::nullopt (exception caught)");
        return std::nullopt;
    } catch (...) {
        Logger::Error("[FileUtils::ListFiles] Unknown exception while listing files in '%s'", directory.c_str());
        Logger::Trace("[FileUtils::ListFiles] Exit: returning std::nullopt (unknown exception caught)");
        return std::nullopt;
    }
}

bool DeleteFile(const std::string& path) {
    Logger::Trace("[FileUtils::DeleteFile] Entry: path='%s'", path.c_str());
    try {
        bool removed = fs::remove(fs::u8path(path));
        if (removed) {
            Logger::Info("[FileUtils::DeleteFile] Successfully deleted file: '%s'", path.c_str());
        } else {
            Logger::Warn("[FileUtils::DeleteFile] File did not exist or could not be removed: '%s'", path.c_str());
        }
        Logger::Trace("[FileUtils::DeleteFile] Exit: returning %s", removed ? "true" : "false");
        return removed;
    } catch (const std::exception& ex) {
        Logger::Error("[FileUtils::DeleteFile] Exception while deleting file '%s': %s", path.c_str(), ex.what());
        Logger::Trace("[FileUtils::DeleteFile] Exit: returning false (exception caught)");
        return false;
    } catch (...) {
        Logger::Error("[FileUtils::DeleteFile] Unknown exception while deleting file '%s'", path.c_str());
        Logger::Trace("[FileUtils::DeleteFile] Exit: returning false (unknown exception caught)");
        return false;
    }
}

std::optional<uintmax_t> GetFileSize(const std::string& path) {
    Logger::Trace("[FileUtils::GetFileSize] Entry: path='%s'", path.c_str());
    try {
        fs::path p = fs::u8path(path);
        if (!fs::exists(p) || !fs::is_regular_file(p)) {
            Logger::Warn("[FileUtils::GetFileSize] Path does not exist or is not a regular file: '%s'", path.c_str());
            Logger::Trace("[FileUtils::GetFileSize] Exit: returning std::nullopt (not a regular file)");
            return std::nullopt;
        }
        uintmax_t size = fs::file_size(p);
        Logger::Info("[FileUtils::GetFileSize] File size for '%s': %ju bytes", path.c_str(), size);
        Logger::Trace("[FileUtils::GetFileSize] Exit: returning size=%ju", size);
        return size;
    } catch (const std::exception& ex) {
        Logger::Error("[FileUtils::GetFileSize] Exception while getting file size for '%s': %s", path.c_str(), ex.what());
        Logger::Trace("[FileUtils::GetFileSize] Exit: returning std::nullopt (exception caught)");
        return std::nullopt;
    } catch (...) {
        Logger::Error("[FileUtils::GetFileSize] Unknown exception while getting file size for '%s'", path.c_str());
        Logger::Trace("[FileUtils::GetFileSize] Exit: returning std::nullopt (unknown exception caught)");
        return std::nullopt;
    }
}

}  // namespace FileUtils
