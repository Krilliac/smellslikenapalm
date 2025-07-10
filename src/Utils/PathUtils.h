// src/Utils/PathUtils.h
#pragma once

#include <string>
#include <filesystem>

namespace PathUtils {
    // Get the directory containing the current executable
    std::string GetExecutableDirectory();
    
    // Get the full path to the current executable
    std::string GetExecutablePath();
    
    // Resolve relative path to absolute path from executable directory
    std::string ResolveFromExecutable(const std::string& relativePath);
}