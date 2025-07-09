// src/Config/INIParser.cpp - Complete implementation for RS2V Server INI file parsing
#include "Config/INIParser.h"
#include "Utils/Logger.h"
#include "Utils/StringUtils.h"
#include "Utils/FileUtils.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <regex>

INIParser::INIParser() {
    Logger::Info("INIParser initialized");
    InitializeParserSettings();
}

INIParser::~INIParser() = default;

bool INIParser::Initialize() {
    Logger::Info("Initializing INI Parser...");
    
    // Set default parser configuration
    m_parserConfig.allowEmptyValues = true;
    m_parserConfig.allowDuplicateKeys = false;
    m_parserConfig.caseSensitive = false;
    m_parserConfig.trimWhitespace = true;
    m_parserConfig.allowMultilineValues = false;
    m_parserConfig.commentChars = {'#', ';'};
    m_parserConfig.sectionBrackets = {'[', ']'};
    m_parserConfig.keyValueSeparator = '=';
    m_parserConfig.escapeChar = '\\';
    m_parserConfig.quotingChars = {'"', '\''};
    
    // Initialize validation patterns
    InitializeValidationPatterns();
    
    Logger::Info("INI Parser initialized successfully");
    return true;
}

bool INIParser::ParseFile(const std::string& filename) {
    Logger::Info("Parsing INI file: %s", filename.c_str());
    
    // Clear previous data
    Clear();
    
    // Check if file exists
    if (!std::filesystem::exists(filename)) {
        Logger::Error("INI file does not exist: %s", filename.c_str());
        return false;
    }
    
    // Store filename for error reporting
    m_currentFile = filename;
    
    // Open file
    std::ifstream file(filename);
    if (!file.is_open()) {
        Logger::Error("Failed to open INI file: %s", filename.c_str());
        return false;
    }
    
    // Parse file content
    bool parseResult = ParseStream(file);
    file.close();
    
    if (parseResult) {
        Logger::Info("Successfully parsed INI file: %s (%zu sections, %zu total keys)", 
                    filename.c_str(), m_sections.size(), GetTotalKeyCount());
        
        // Log parsing statistics
        LogParsingStatistics();
    } else {
        Logger::Error("Failed to parse INI file: %s", filename.c_str());
        LogParsingErrors();
    }
    
    return parseResult;
}

bool INIParser::ParseString(const std::string& content) {
    Logger::Debug("Parsing INI content from string (%zu characters)", content.length());
    
    // Clear previous data
    Clear();
    
    // Create string stream
    std::istringstream stream(content);
    
    // Parse stream content
    bool parseResult = ParseStream(stream);
    
    if (parseResult) {
        Logger::Debug("Successfully parsed INI content (%zu sections, %zu total keys)", 
                     m_sections.size(), GetTotalKeyCount());
    } else {
        Logger::Error("Failed to parse INI content");
        LogParsingErrors();
    }
    
    return parseResult;
}

bool INIParser::ParseStream(std::istream& stream) {
    Logger::Debug("Parsing INI stream...");
    
    std::string line;
    std::string currentSection;
    size_t lineNumber = 0;
    bool hasErrors = false;
    
    // Initialize with global section
    m_sections[""] = INISection();
    m_sections[""].name = "";
    
    while (std::getline(stream, line)) {
        lineNumber++;
        
        // Store original line for error reporting
        std::string originalLine = line;
        
        // Process line
        LineType lineType = ProcessLine(line, currentSection, lineNumber);
        
        switch (lineType) {
            case LineType::SECTION:
                // Section header was processed
                break;
                
            case LineType::KEY_VALUE:
                // Key-value pair was processed
                break;
                
            case LineType::COMMENT:
                // Comment was processed (if comment preservation is enabled)
                if (m_parserConfig.preserveComments) {
                    PreserveComment(currentSection, originalLine, lineNumber);
                }
                break;
                
            case LineType::EMPTY:
                // Empty line - nothing to do
                break;
                
            case LineType::INVALID:
                // Invalid line - log error
                ParsingError error;
                error.lineNumber = lineNumber;
                error.line = originalLine;
                error.message = "Invalid line format";
                error.severity = ErrorSeverity::WARNING;
                m_parsingErrors.push_back(error);
                
                Logger::Warn("Invalid line %zu in INI: %s", lineNumber, originalLine.c_str());
                
                if (m_parserConfig.strictMode) {
                    hasErrors = true;
                }
                break;
                
            case LineType::MULTILINE_START:
                // Handle multiline values if enabled
                if (m_parserConfig.allowMultilineValues) {
                    ProcessMultilineValue(stream, currentSection, lineNumber);
                } else {
                    ParsingError error;
                    error.lineNumber = lineNumber;
                    error.line = originalLine;
                    error.message = "Multiline values not allowed";
                    error.severity = ErrorSeverity::ERROR;
                    m_parsingErrors.push_back(error);
                    hasErrors = true;
                }
                break;
        }
    }
    
    // Post-processing validation
    if (!PostProcessValidation()) {
        hasErrors = true;
    }
    
    return !hasErrors || !m_parserConfig.strictMode;
}

bool INIParser::SaveToFile(const std::string& filename) {
    Logger::Info("Saving INI data to file: %s", filename.c_str());
    
    // Create directory if it doesn't exist
    std::filesystem::path filePath(filename);
    if (filePath.has_parent_path()) {
        try {
            std::filesystem::create_directories(filePath.parent_path());
        } catch (const std::exception& e) {
            Logger::Error("Failed to create directory for INI file: %s", e.what());
            return false;
        }
    }
    
    // Open file for writing
    std::ofstream file(filename);
    if (!file.is_open()) {
        Logger::Error("Failed to create INI file: %s", filename.c_str());
        return false;
    }
    
    // Write content
    bool result = WriteToStream(file);
    file.close();
    
    if (result) {
        Logger::Info("Successfully saved INI file: %s", filename.c_str());
    } else {
        Logger::Error("Failed to save INI file: %s", filename.c_str());
    }
    
    return result;
}

bool INIParser::WriteToStream(std::ostream& stream) {
    Logger::Debug("Writing INI data to stream...");
    
    // Write header comment if specified
    if (!m_parserConfig.headerComment.empty()) {
        WriteComment(stream, m_parserConfig.headerComment);
        stream << std::endl;
    }
    
    // Write global section first (if it has content)
    if (m_sections.find("") != m_sections.end() && !m_sections[""].keyValues.empty()) {
        WriteSectionContent(stream, m_sections[""]);
        stream << std::endl;
    }
    
    // Write all other sections
    for (const auto& [sectionName, section] : m_sections) {
        if (sectionName.empty()) {
            continue; // Already written global section
        }
        
        // Write preserved comments before section
        WritePreservedComments(stream, sectionName, true);
        
        // Write section header
        stream << m_parserConfig.sectionBrackets.first << sectionName << m_parserConfig.sectionBrackets.second << std::endl;
        
        // Write section content
        WriteSectionContent(stream, section);
        
        // Write preserved comments after section
        WritePreservedComments(stream, sectionName, false);
        
        stream << std::endl;
    }
    
    // Write footer comment if specified
    if (!m_parserConfig.footerComment.empty()) {
        stream << std::endl;
        WriteComment(stream, m_parserConfig.footerComment);
    }
    
    return stream.good();
}

std::string INIParser::GetValue(const std::string& section, const std::string& key, const std::string& defaultValue) const {
    auto sectionIt = m_sections.find(section);
    if (sectionIt == m_sections.end()) {
        Logger::Debug("INI section not found: %s", section.c_str());
        return defaultValue;
    }
    
    auto keyIt = sectionIt->second.keyValues.find(NormalizeKey(key));
    if (keyIt == sectionIt->second.keyValues.end()) {
        Logger::Debug("INI key not found: %s.%s", section.c_str(), key.c_str());
        return defaultValue;
    }
    
    return keyIt->second.value;
}

int INIParser::GetIntValue(const std::string& section, const std::string& key, int defaultValue) const {
    std::string value = GetValue(section, key);
    if (value.empty()) {
        return defaultValue;
    }
    
    try {
        return std::stoi(value);
    } catch (const std::exception& e) {
        Logger::Warn("Invalid integer value for %s.%s: %s", section.c_str(), key.c_str(), value.c_str());
        return defaultValue;
    }
}

float INIParser::GetFloatValue(const std::string& section, const std::string& key, float defaultValue) const {
    std::string value = GetValue(section, key);
    if (value.empty()) {
        return defaultValue;
    }
    
    try {
        return std::stof(value);
    } catch (const std::exception& e) {
        Logger::Warn("Invalid float value for %s.%s: %s", section.c_str(), key.c_str(), value.c_str());
        return defaultValue;
    }
}

bool INIParser::GetBoolValue(const std::string& section, const std::string& key, bool defaultValue) const {
    std::string value = GetValue(section, key);
    if (value.empty()) {
        return defaultValue;
    }
    
    // Convert to lowercase for comparison
    std::string lowerValue = StringUtils::ToLower(value);
    
    if (lowerValue == "true" || lowerValue == "1" || lowerValue == "yes" || lowerValue == "on" || lowerValue == "enabled") {
        return true;
    } else if (lowerValue == "false" || lowerValue == "0" || lowerValue == "no" || lowerValue == "off" || lowerValue == "disabled") {
        return false;
    }
    
    Logger::Warn("Invalid boolean value for %s.%s: %s", section.c_str(), key.c_str(), value.c_str());
    return defaultValue;
}

std::vector<std::string> INIParser::GetArrayValue(const std::string& section, const std::string& key, const std::vector<std::string>& defaultValue) const {
    std::string value = GetValue(section, key);
    if (value.empty()) {
        return defaultValue;
    }
    
    // Parse array value (comma-separated by default)
    std::vector<std::string> result;
    std::istringstream stream(value);
    std::string item;
    
    while (std::getline(stream, item, m_parserConfig.arraySeparator)) {
        item = StringUtils::Trim(item);
        if (!item.empty()) {
            result.push_back(item);
        }
    }
    
    return result;
}

void INIParser::SetValue(const std::string& section, const std::string& key, const std::string& value) {
    // Ensure section exists
    if (m_sections.find(section) == m_sections.end()) {
        m_sections[section] = INISection();
        m_sections[section].name = section;
    }
    
    // Set key-value pair
    INIKeyValue keyValue;
    keyValue.key = key;
    keyValue.value = value;
    keyValue.lineNumber = 0; // New value
    
    std::string normalizedKey = NormalizeKey(key);
    
    // Check for duplicate keys if not allowed
    if (!m_parserConfig.allowDuplicateKeys && m_sections[section].keyValues.find(normalizedKey) != m_sections[section].keyValues.end()) {
        Logger::Debug("Overwriting existing INI value: %s.%s", section.c_str(), key.c_str());
    }
    
    m_sections[section].keyValues[normalizedKey] = keyValue;
    
    Logger::Debug("Set INI value: %s.%s = %s", section.c_str(), key.c_str(), value.c_str());
}

void INIParser::SetIntValue(const std::string& section, const std::string& key, int value) {
    SetValue(section, key, std::to_string(value));
}

void INIParser::SetFloatValue(const std::string& section, const std::string& key, float value) {
    SetValue(section, key, std::to_string(value));
}

void INIParser::SetBoolValue(const std::string& section, const std::string& key, bool value) {
    SetValue(section, key, value ? "true" : "false");
}

void INIParser::SetArrayValue(const std::string& section, const std::string& key, const std::vector<std::string>& value) {
    std::string arrayString;
    for (size_t i = 0; i < value.size(); ++i) {
        if (i > 0) {
            arrayString += m_parserConfig.arraySeparator;
            arrayString += " "; // Add space after separator for readability
        }
        arrayString += value[i];
    }
    SetValue(section, key, arrayString);
}

bool INIParser::HasSection(const std::string& section) const {
    return m_sections.find(section) != m_sections.end();
}

bool INIParser::HasKey(const std::string& section, const std::string& key) const {
    auto sectionIt = m_sections.find(section);
    if (sectionIt == m_sections.end()) {
        return false;
    }
    
    return sectionIt->second.keyValues.find(NormalizeKey(key)) != sectionIt->second.keyValues.end();
}

void INIParser::RemoveSection(const std::string& section) {
    auto it = m_sections.find(section);
    if (it != m_sections.end()) {
        m_sections.erase(it);
        Logger::Debug("Removed INI section: %s", section.c_str());
    }
}

void INIParser::RemoveKey(const std::string& section, const std::string& key) {
    auto sectionIt = m_sections.find(section);
    if (sectionIt != m_sections.end()) {
        auto keyIt = sectionIt->second.keyValues.find(NormalizeKey(key));
        if (keyIt != sectionIt->second.keyValues.end()) {
            sectionIt->second.keyValues.erase(keyIt);
            Logger::Debug("Removed INI key: %s.%s", section.c_str(), key.c_str());
        }
    }
}

std::vector<std::string> INIParser::GetSectionNames() const {
    std::vector<std::string> sectionNames;
    for (const auto& [name, section] : m_sections) {
        if (!name.empty()) { // Skip global section
            sectionNames.push_back(name);
        }
    }
    return sectionNames;
}

std::vector<std::string> INIParser::GetKeyNames(const std::string& section) const {
    std::vector<std::string> keyNames;
    
    auto sectionIt = m_sections.find(section);
    if (sectionIt != m_sections.end()) {
        for (const auto& [key, value] : sectionIt->second.keyValues) {
            keyNames.push_back(value.key); // Use original key name, not normalized
        }
    }
    
    return keyNames;
}

void INIParser::Clear() {
    m_sections.clear();
    m_parsingErrors.clear();
    m_preservedComments.clear();
    m_currentFile.clear();
    
    Logger::Debug("INI parser data cleared");
}

bool INIParser::IsEmpty() const {
    return m_sections.empty() || (m_sections.size() == 1 && m_sections.begin()->second.keyValues.empty());
}

size_t INIParser::GetSectionCount() const {
    // Don't count empty global section
    size_t count = m_sections.size();
    if (m_sections.find("") != m_sections.end() && m_sections.at("").keyValues.empty()) {
        count--;
    }
    return count;
}

size_t INIParser::GetKeyCount(const std::string& section) const {
    auto sectionIt = m_sections.find(section);
    return sectionIt != m_sections.end() ? sectionIt->second.keyValues.size() : 0;
}

size_t INIParser::GetTotalKeyCount() const {
    size_t totalKeys = 0;
    for (const auto& [name, section] : m_sections) {
        totalKeys += section.keyValues.size();
    }
    return totalKeys;
}

const std::vector<ParsingError>& INIParser::GetParsingErrors() const {
    return m_parsingErrors;
}

bool INIParser::HasParsingErrors() const {
    return !m_parsingErrors.empty();
}

void INIParser::SetParserConfig(const ParserConfig& config) {
    m_parserConfig = config;
    Logger::Debug("INI parser configuration updated");
}

const ParserConfig& INIParser::GetParserConfig() const {
    return m_parserConfig;
}

INIParser::LineType INIParser::ProcessLine(std::string& line, std::string& currentSection, size_t lineNumber) {
    // Store original line for error reporting
    std::string originalLine = line;
    
    // Trim whitespace if enabled
    if (m_parserConfig.trimWhitespace) {
        line = StringUtils::Trim(line);
    }
    
    // Empty line
    if (line.empty()) {
        return LineType::EMPTY;
    }
    
    // Comment line
    if (IsCommentLine(line)) {
        return LineType::COMMENT;
    }
    
    // Section header
    if (IsSectionLine(line)) {
        return ProcessSectionLine(line, currentSection, lineNumber);
    }
    
    // Multiline value start
    if (IsMultilineStart(line)) {
        return LineType::MULTILINE_START;
    }
    
    // Key-value pair
    if (IsKeyValueLine(line)) {
        return ProcessKeyValueLine(line, currentSection, lineNumber);
    }
    
    // Invalid line
    return LineType::INVALID;
}

INIParser::LineType INIParser::ProcessSectionLine(const std::string& line, std::string& currentSection, size_t lineNumber) {
    // Extract section name from brackets
    size_t start = line.find(m_parserConfig.sectionBrackets.first);
    size_t end = line.find(m_parserConfig.sectionBrackets.second, start + 1);
    
    if (start == std::string::npos || end == std::string::npos || end <= start + 1) {
        ParsingError error;
        error.lineNumber = lineNumber;
        error.line = line;
        error.message = "Invalid section header format";
        error.severity = ErrorSeverity::ERROR;
        m_parsingErrors.push_back(error);
        return LineType::INVALID;
    }
    
    std::string sectionName = line.substr(start + 1, end - start - 1);
    
    if (m_parserConfig.trimWhitespace) {
        sectionName = StringUtils::Trim(sectionName);
    }
    
    // Validate section name
    if (!IsValidSectionName(sectionName)) {
        ParsingError error;
        error.lineNumber = lineNumber;
        error.line = line;
        error.message = "Invalid section name: " + sectionName;
        error.severity = ErrorSeverity::ERROR;
        m_parsingErrors.push_back(error);
        return LineType::INVALID;
    }
    
    // Normalize section name if case-insensitive
    if (!m_parserConfig.caseSensitive) {
        sectionName = StringUtils::ToLower(sectionName);
    }
    
    // Create section if it doesn't exist
    if (m_sections.find(sectionName) == m_sections.end()) {
        m_sections[sectionName] = INISection();
        m_sections[sectionName].name = sectionName;
        m_sections[sectionName].lineNumber = lineNumber;
    }
    
    currentSection = sectionName;
    
    Logger::Debug("Found INI section: %s (line %zu)", sectionName.c_str(), lineNumber);
    return LineType::SECTION;
}

INIParser::LineType INIParser::ProcessKeyValueLine(const std::string& line, const std::string& currentSection, size_t lineNumber) {
    // Find key-value separator
    size_t separatorPos = line.find(m_parserConfig.keyValueSeparator);
    if (separatorPos == std::string::npos) {
        ParsingError error;
        error.lineNumber = lineNumber;
        error.line = line;
        error.message = "Missing key-value separator";
        error.severity = ErrorSeverity::ERROR;
        m_parsingErrors.push_back(error);
        return LineType::INVALID;
    }
    
    // Extract key and value
    std::string key = line.substr(0, separatorPos);
    std::string value = line.substr(separatorPos + 1);
    
    if (m_parserConfig.trimWhitespace) {
        key = StringUtils::Trim(key);
        value = StringUtils::Trim(value);
    }
    
    // Validate key
    if (key.empty()) {
        ParsingError error;
        error.lineNumber = lineNumber;
        error.line = line;
        error.message = "Empty key name";
        error.severity = ErrorSeverity::ERROR;
        m_parsingErrors.push_back(error);
        return LineType::INVALID;
    }
    
    if (!IsValidKeyName(key)) {
        ParsingError error;
        error.lineNumber = lineNumber;
        error.line = line;
        error.message = "Invalid key name: " + key;
        error.severity = ErrorSeverity::WARNING;
        m_parsingErrors.push_back(error);
        
        if (m_parserConfig.strictMode) {
            return LineType::INVALID;
        }
    }
    
    // Validate value
    if (value.empty() && !m_parserConfig.allowEmptyValues) {
        ParsingError error;
        error.lineNumber = lineNumber;
        error.line = line;
        error.message = "Empty value not allowed for key: " + key;
        error.severity = ErrorSeverity::WARNING;
        m_parsingErrors.push_back(error);
        
        if (m_parserConfig.strictMode) {
            return LineType::INVALID;
        }
    }
    
    // Process quoted values
    value = ProcessQuotedValue(value);
    
    // Process escape sequences
    value = ProcessEscapeSequences(value);
    
    // Create key-value entry
    INIKeyValue keyValue;
    keyValue.key = key;
    keyValue.value = value;
    keyValue.lineNumber = lineNumber;
    
    // Ensure section exists
    std::string targetSection = currentSection;
    if (m_sections.find(targetSection) == m_sections.end()) {
        m_sections[targetSection] = INISection();
        m_sections[targetSection].name = targetSection;
    }
    
    // Normalize key if case-insensitive
    std::string normalizedKey = NormalizeKey(key);
    
    // Check for duplicate keys
    if (!m_parserConfig.allowDuplicateKeys && m_sections[targetSection].keyValues.find(normalizedKey) != m_sections[targetSection].keyValues.end()) {
        ParsingError error;
        error.lineNumber = lineNumber;
        error.line = line;
        error.message = "Duplicate key: " + key;
        error.severity = ErrorSeverity::WARNING;
        m_parsingErrors.push_back(error);
        
        if (m_parserConfig.strictMode) {
            return LineType::INVALID;
        }
    }
    
    // Store key-value pair
    m_sections[targetSection].keyValues[normalizedKey] = keyValue;
    
    Logger::Debug("Found INI key-value: %s.%s = %s (line %zu)", 
                 targetSection.c_str(), key.c_str(), value.c_str(), lineNumber);
    
    return LineType::KEY_VALUE;
}

void INIParser::ProcessMultilineValue(std::istream& stream, const std::string& currentSection, size_t& lineNumber) {
    // TODO: Implement multiline value processing
    Logger::Debug("Processing multiline value (not implemented)");
}

bool INIParser::IsCommentLine(const std::string& line) const {
    if (line.empty()) {
        return false;
    }
    
    for (char commentChar : m_parserConfig.commentChars) {
        if (line[0] == commentChar) {
            return true;
        }
    }
    
    return false;
}

bool INIParser::IsSectionLine(const std::string& line) const {
    return !line.empty() && 
           line.front() == m_parserConfig.sectionBrackets.first && 
           line.find(m_parserConfig.sectionBrackets.second) != std::string::npos;
}

bool INIParser::IsKeyValueLine(const std::string& line) const {
    return line.find(m_parserConfig.keyValueSeparator) != std::string::npos;
}

bool INIParser::IsMultilineStart(const std::string& line) const {
    // Simple multiline detection - ends with backslash
    return !line.empty() && line.back() == '\\';
}

bool INIParser::IsValidSectionName(const std::string& name) const {
    if (name.empty()) {
        return false;
    }
    
    // Check against validation pattern if specified
    if (!m_parserConfig.sectionNamePattern.empty()) {
        try {
            std::regex pattern(m_parserConfig.sectionNamePattern);
            return std::regex_match(name, pattern);
        } catch (const std::exception& e) {
            Logger::Warn("Invalid section name regex pattern: %s", e.what());
        }
    }
    
    // Default validation - alphanumeric, underscore, dash, dot
    for (char c : name) {
        if (!std::isalnum(c) && c != '_' && c != '-' && c != '.') {
            return false;
        }
    }
    
    return true;
}

bool INIParser::IsValidKeyName(const std::string& name) const {
    if (name.empty()) {
        return false;
    }
    
    // Check against validation pattern if specified
    if (!m_parserConfig.keyNamePattern.empty()) {
        try {
            std::regex pattern(m_parserConfig.keyNamePattern);
            return std::regex_match(name, pattern);
        } catch (const std::exception& e) {
            Logger::Warn("Invalid key name regex pattern: %s", e.what());
        }
    }
    
    // Default validation - alphanumeric, underscore, dash, dot
    for (char c : name) {
        if (!std::isalnum(c) && c != '_' && c != '-' && c != '.') {
            return false;
        }
    }
    
    return true;
}

std::string INIParser::NormalizeKey(const std::string& key) const {
    return m_parserConfig.caseSensitive ? key : StringUtils::ToLower(key);
}

std::string INIParser::ProcessQuotedValue(const std::string& value) const {
    if (value.length() < 2) {
        return value;
    }
    
    // Check if value is quoted
    for (char quoteChar : m_parserConfig.quotingChars) {
        if (value.front() == quoteChar && value.back() == quoteChar) {
            // Remove quotes
            return value.substr(1, value.length() - 2);
        }
    }
    
    return value;
}

std::string INIParser::ProcessEscapeSequences(const std::string& value) const {
    if (value.find(m_parserConfig.escapeChar) == std::string::npos) {
        return value; // No escape sequences
    }
    
    std::string result;
    result.reserve(value.length());
    
    for (size_t i = 0; i < value.length(); ++i) {
        if (value[i] == m_parserConfig.escapeChar && i + 1 < value.length()) {
            char nextChar = value[i + 1];
            switch (nextChar) {
                case 'n':
                    result += '\n';
                    break;
                case 't':
                    result += '\t';
                    break;
                case 'r':
                    result += '\r';
                    break;
                case '\\':
                    result += '\\';
                    break;
                case '"':
                    result += '"';
                    break;
                case '\'':
                    result += '\'';
                    break;
                default:
                    // Unknown escape sequence - keep as is
                    result += value[i];
                    result += nextChar;
                    break;
            }
            i++; // Skip next character
        } else {
            result += value[i];
        }
    }
    
    return result;
}

bool INIParser::PostProcessValidation() {
    Logger::Debug("Performing post-processing validation...");
    
    bool isValid = true;
    
    // Validate RS2V specific configuration sections
    if (!ValidateRS2VConfiguration()) {
        isValid = false;
    }
    
    // Validate cross-section references
    if (!ValidateCrossSectionReferences()) {
        isValid = false;
    }
    
    // Validate required sections
    if (!ValidateRequiredSections()) {
        isValid = false;
    }
    
    // Validate value formats
    if (!ValidateValueFormats()) {
        isValid = false;
    }
    
    return isValid;
}

bool INIParser::ValidateRS2VConfiguration() {
    Logger::Debug("Validating RS2V specific configuration...");
    
    // Check for required RS2V sections
    std::vector<std::string> requiredSections = {"Server", "Game", "Network"};
    
    for (const auto& sectionName : requiredSections) {
        if (!HasSection(sectionName)) {
            ParsingError error;
            error.lineNumber = 0;
            error.line = "";
            error.message = "Missing required section: " + sectionName;
            error.severity = ErrorSeverity::ERROR;
            m_parsingErrors.push_back(error);
            
            Logger::Error("Missing required RS2V section: %s", sectionName.c_str());
        }
    }
    
    // Validate specific RS2V keys
    ValidateRS2VKeys();
    
    return true;
}

void INIParser::ValidateRS2VKeys() {
    // Validate Server section keys
    if (HasSection("Server")) {
        std::map<std::string, std::string> requiredServerKeys = {
            {"ServerName", "string"},
            {"MaxPlayers", "integer"},
            {"GamePort", "port"},
            {"QueryPort", "port"}
        };
        
        for (const auto& [key, type] : requiredServerKeys) {
            if (!HasKey("Server", key)) {
                ParsingError error;
                error.lineNumber = 0;
                error.line = "";
                error.message = "Missing required Server key: " + key;
                error.severity = ErrorSeverity::WARNING;
                m_parsingErrors.push_back(error);
            }
        }
    }
    
    // Validate Game section keys
    if (HasSection("Game")) {
        std::map<std::string, std::string> requiredGameKeys = {
            {"MapName", "string"},
            {"GameMode", "string"}
        };
        
        for (const auto& [key, type] : requiredGameKeys) {
            if (!HasKey("Game", key)) {
                ParsingError error;
                error.lineNumber = 0;
                error.line = "";
                error.message = "Missing required Game key: " + key;
                error.severity = ErrorSeverity::WARNING;
                m_parsingErrors.push_back(error);
            }
        }
    }
}

bool INIParser::ValidateCrossSectionReferences() {
    Logger::Debug("Validating cross-section references...");
    
    // Example: Validate that EAC.ProxyPort doesn't conflict with Server.GamePort
    if (HasKey("Server", "GamePort") && HasKey("EAC", "ProxyPort")) {
        int gamePort = GetIntValue("Server", "GamePort", 0);
        int eacPort = GetIntValue("EAC", "ProxyPort", 0);
        
        if (gamePort > 0 && eacPort > 0 && gamePort == eacPort) {
            ParsingError error;
            error.lineNumber = 0;
            error.line = "";
            error.message = "Port conflict: Server.GamePort and EAC.ProxyPort cannot be the same";
            error.severity = ErrorSeverity::ERROR;
            m_parsingErrors.push_back(error);
            return false;
        }
    }
    
    return true;
}

bool INIParser::ValidateRequiredSections() {
    // Implement required section validation logic
    return true;
}

bool INIParser::ValidateValueFormats() {
    Logger::Debug("Validating value formats...");
    
    // Validate port numbers
    ValidatePortValues();
    
    // Validate IP addresses
    ValidateIPAddressValues();
    
    // Validate boolean values
    ValidateBooleanValues();
    
    // Validate numeric ranges
    ValidateNumericRanges();
    
    return true;
}

void INIParser::ValidatePortValues() {
    // Check all port-related keys
    std::vector<std::pair<std::string, std::string>> portKeys = {
        {"Server", "GamePort"},
        {"Server", "QueryPort"},
        {"EAC", "ProxyPort"},
        {"Network", "ListenPort"}
    };
    
    for (const auto& [section, key] : portKeys) {
        if (HasKey(section, key)) {
            int port = GetIntValue(section, key, 0);
            if (port < 1024 || port > 65535) {
                ParsingError error;
                error.lineNumber = 0;
                error.line = "";
                error.message = section + "." + key + " must be between 1024 and 65535";
                error.severity = ErrorSeverity::ERROR;
                m_parsingErrors.push_back(error);
            }
        }
    }
}

void INIParser::ValidateIPAddressValues() {
    // Check IP address format for relevant keys
    std::vector<std::pair<std::string, std::string>> ipKeys = {
        {"Network", "BindAddress"},
        {"EAC", "TargetIP"}
    };
    
    for (const auto& [section, key] : ipKeys) {
        if (HasKey(section, key)) {
            std::string ip = GetValue(section, key);
            if (!IsValidIPAddress(ip)) {
                ParsingError error;
                error.lineNumber = 0;
                error.line = "";
                error.message = section + "." + key + " is not a valid IP address: " + ip;
                error.severity = ErrorSeverity::ERROR;
                m_parsingErrors.push_back(error);
            }
        }
    }
}

void INIParser::ValidateBooleanValues() {
    // Check boolean format for relevant keys
    std::vector<std::pair<std::string, std::string>> boolKeys = {
        {"Security", "SecureMode"},
        {"Game", "FriendlyFire"},
        {"EAC", "EnableLogging"}
    };
    
    for (const auto& [section, key] : boolKeys) {
        if (HasKey(section, key)) {
            std::string value = GetValue(section, key);
            if (!IsValidBooleanValue(value)) {
                ParsingError error;
                error.lineNumber = 0;
                error.line = "";
                error.message = section + "." + key + " must be a boolean value (true/false)";
                error.severity = ErrorSeverity::WARNING;
                m_parsingErrors.push_back(error);
            }
        }
    }
}

void INIParser::ValidateNumericRanges() {
    // Validate numeric ranges for specific keys
    std::map<std::pair<std::string, std::string>, std::pair<int, int>> numericRanges = {
        {{"Server", "MaxPlayers"}, {1, 128}},
        {{"Server", "TickRate"}, {10, 128}},
        {{"Game", "RoundTimeLimit"}, {60, 7200}}
    };
    
    for (const auto& [keyPair, range] : numericRanges) {
        const auto& [section, key] = keyPair;
        const auto& [minVal, maxVal] = range;
        
        if (HasKey(section, key)) {
            int value = GetIntValue(section, key, 0);
            if (value < minVal || value > maxVal) {
                ParsingError error;
                error.lineNumber = 0;
                error.line = "";
                error.message = section + "." + key + " must be between " + 
                               std::to_string(minVal) + " and " + std::to_string(maxVal);
                error.severity = ErrorSeverity::ERROR;
                m_parsingErrors.push_back(error);
            }
        }
    }
}

bool INIParser::IsValidIPAddress(const std::string& ip) const {
    if (ip == "0.0.0.0" || ip == "localhost") {
        return true;
    }
    
    std::regex ipRegex(R"(^((25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.){3}(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)$)");
    return std::regex_match(ip, ipRegex);
}

bool INIParser::IsValidBooleanValue(const std::string& value) const {
    std::string lower = StringUtils::ToLower(value);
    return lower == "true" || lower == "false" || lower == "1" || lower == "0" || 
           lower == "yes" || lower == "no" || lower == "on" || lower == "off" ||
           lower == "enabled" || lower == "disabled";
}

void INIParser::PreserveComment(const std::string& section, const std::string& comment, size_t lineNumber) {
    if (!m_parserConfig.preserveComments) {
        return;
    }
    
    PreservedComment preservedComment;
    preservedComment.section = section;
    preservedComment.comment = comment;
    preservedComment.lineNumber = lineNumber;
    preservedComment.beforeSection = true; // Default to before section
    
    m_preservedComments.push_back(preservedComment);
}

void INIParser::WriteSectionContent(std::ostream& stream, const INISection& section) {
    for (const auto& [normalizedKey, keyValue] : section.keyValues) {
        // Write preserved comments before this key
        WritePreservedCommentsForKey(stream, section.name, keyValue.key, true);
        
        // Write key-value pair
        stream << keyValue.key << m_parserConfig.keyValueSeparator;
        
        // Add space after separator for readability
        if (m_parserConfig.addSpaceAroundSeparator) {
            stream << " ";
        }
        
        // Quote value if necessary
        std::string outputValue = keyValue.value;
        if (ShouldQuoteValue(outputValue)) {
            outputValue = QuoteValue(outputValue);
        }
        
        stream << outputValue << std::endl;
        
        // Write preserved comments after this key
        WritePreservedCommentsForKey(stream, section.name, keyValue.key, false);
    }
}

void INIParser::WriteComment(std::ostream& stream, const std::string& comment) {
    // Use first comment character
    char commentChar = m_parserConfig.commentChars.empty() ? '#' : m_parserConfig.commentChars[0];
    
    // Split multi-line comments
    std::istringstream commentStream(comment);
    std::string line;
    
    while (std::getline(commentStream, line)) {
        stream << commentChar << " " << line << std::endl;
    }
}

void INIParser::WritePreservedComments(std::ostream& stream, const std::string& section, bool beforeSection) {
    for (const auto& comment : m_preservedComments) {
        if (comment.section == section && comment.beforeSection == beforeSection) {
            stream << comment.comment << std::endl;
        }
    }
}

void INIParser::WritePreservedCommentsForKey(std::ostream& stream, const std::string& section, const std::string& key, bool beforeKey) {
    // Implementation for key-specific comment preservation
    // This would require more sophisticated comment tracking
}

bool INIParser::ShouldQuoteValue(const std::string& value) const {
    if (value.empty()) {
        return false;
    }
    
    // Quote if value contains special characters
    for (char specialChar : {' ', '\t', '\n', '\r'}) {
        if (value.find(specialChar) != std::string::npos) {
            return true;
        }
    }
    
    // Quote if value contains comment characters
    for (char commentChar : m_parserConfig.commentChars) {
        if (value.find(commentChar) != std::string::npos) {
            return true;
        }
    }
    
    // Quote if value contains separator
    if (value.find(m_parserConfig.keyValueSeparator) != std::string::npos) {
        return true;
    }
    
    return false;
}

std::string INIParser::QuoteValue(const std::string& value) const {
    if (m_parserConfig.quotingChars.empty()) {
        return value;
    }
    
    char quoteChar = m_parserConfig.quotingChars[0];
    return std::string(1, quoteChar) + value + std::string(1, quoteChar);
}

void INIParser::InitializeParserSettings() {
    Logger::Debug("Initializing INI parser settings...");
    
    // Default settings are set in Initialize()
    // This method can be extended for custom initialization
    
    Logger::Debug("INI parser settings initialized");
}

void INIParser::InitializeValidationPatterns() {
    Logger::Debug("Initializing validation patterns...");
    
    // Set up regex patterns for validation
    m_parserConfig.sectionNamePattern = R"([a-zA-Z0-9_\-\.]+)";
    m_parserConfig.keyNamePattern = R"([a-zA-Z0-9_\-\.]+)";
    
    Logger::Debug("Validation patterns initialized");
}

void INIParser::LogParsingStatistics() {
    size_t sectionCount = GetSectionCount();
    size_t totalKeys = GetTotalKeyCount();
    size_t errorCount = m_parsingErrors.size();
    
    Logger::Info("INI Parsing Statistics:");
    Logger::Info("  Sections: %zu", sectionCount);
    Logger::Info("  Total Keys: %zu", totalKeys);
    Logger::Info("  Parsing Errors: %zu", errorCount);
    
    if (errorCount > 0) {
        size_t warnings = 0, errors = 0;
        for (const auto& error : m_parsingErrors) {
            if (error.severity == ErrorSeverity::WARNING) {
                warnings++;
            } else {
                errors++;
            }
        }
        Logger::Info("  Warnings: %zu, Errors: %zu", warnings, errors);
    }
}

void INIParser::LogParsingErrors() {
    if (m_parsingErrors.empty()) {
        return;
    }
    
    Logger::Warn("INI Parsing Errors (%zu total):", m_parsingErrors.size());
    
    for (const auto& error : m_parsingErrors) {
        const char* severityStr = (error.severity == ErrorSeverity::ERROR) ? "ERROR" : "WARNING";
        
        if (error.lineNumber > 0) {
            Logger::Log(error.severity == ErrorSeverity::ERROR ? LogLevel::ERROR : LogLevel::WARN,
                       "  Line %zu [%s]: %s", error.lineNumber, severityStr, error.message.c_str());
            
            if (!error.line.empty()) {
                Logger::Log(error.severity == ErrorSeverity::ERROR ? LogLevel::ERROR : LogLevel::WARN,
                           "    > %s", error.line.c_str());
            }
        } else {
            Logger::Log(error.severity == ErrorSeverity::ERROR ? LogLevel::ERROR : LogLevel::WARN,
                       "  [%s]: %s", severityStr, error.message.c_str());
        }
    }
}

std::map<std::string, std::map<std::string, std::string>> INIParser::GetAllData() const {
    std::map<std::string, std::map<std::string, std::string>> result;
    
    for (const auto& [sectionName, section] : m_sections) {
        std::map<std::string, std::string> sectionData;
        
        for (const auto& [normalizedKey, keyValue] : section.keyValues) {
            sectionData[keyValue.key] = keyValue.value;
        }
        
        result[sectionName] = sectionData;
    }
    
    return result;
}

void INIParser::SetAllData(const std::map<std::string, std::map<std::string, std::string>>& data) {
    Clear();
    
    for (const auto& [sectionName, sectionData] : data) {
        for (const auto& [key, value] : sectionData) {
            SetValue(sectionName, key, value);
        }
    }
    
    Logger::Debug("Set all INI data: %zu sections", data.size());
}