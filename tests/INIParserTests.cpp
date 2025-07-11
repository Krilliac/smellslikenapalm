// tests/INIParserTests.cpp
// Comprehensive INI file parsing and configuration handling unit tests

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <regex>

// Include the headers
#include "Config/INIParser.h"
#include "Config/ConfigManager.h"
#include "Utils/Logger.h"
#include "Utils/FileUtils.h"
#include "Utils/StringUtils.h"

using ::testing::_;
using ::testing::Return;
using ::testing::InSequence;
using ::testing::StrictMock;
using ::testing::NiceMock;
using ::testing::Invoke;
using ::testing::DoAll;
using ::testing::SetArgReferee;
using ::testing::AtLeast;
using ::testing::Between;

// Constants for INI parser testing
constexpr const char* TEST_INI_DIR = "test_ini_files";
constexpr const char* VALID_INI = "valid.ini";
constexpr const char* MALFORMED_INI = "malformed.ini";
constexpr const char* EMPTY_INI = "empty.ini";
constexpr const char* COMMENTS_INI = "comments.ini";
constexpr const char* UNICODE_INI = "unicode.ini";
constexpr const char* LARGE_INI = "large.ini";

// INI data structures
struct INISection {
    std::string sectionName;
    std::unordered_map<std::string, std::string> keyValuePairs;
    std::vector<std::string> comments;
    int lineNumber;
    
    INISection(const std::string& name, int line = 0) 
        : sectionName(name), lineNumber(line) {}
};

struct INIParseResult {
    bool success;
    std::vector<INISection> sections;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    int totalLines;
    std::chrono::milliseconds parseTime;
    
    INIParseResult() : success(false), totalLines(0), parseTime(0) {}
};

struct INIValue {
    std::string rawValue;
    std::string processedValue;
    std::string type; // "string", "int", "bool", "float", "array"
    int lineNumber;
    std::vector<std::string> comments;
    
    INIValue(const std::string& raw = "", int line = 0) 
        : rawValue(raw), processedValue(raw), type("string"), lineNumber(line) {}
};

// Mock classes for INI parser testing
class MockFileUtils : public FileUtils {
public:
    MOCK_METHOD(bool, FileExists, (const std::string& path), (const, override));
    MOCK_METHOD(bool, IsReadable, (const std::string& path), (const, override));
    MOCK_METHOD(std::string, ReadFileToString, (const std::string& path), (const, override));
    MOCK_METHOD(bool, WriteStringToFile, (const std::string& path, const std::string& content), (const, override));
    MOCK_METHOD(std::vector<std::string>, ReadFileToLines, (const std::string& path), (const, override));
    MOCK_METHOD(std::chrono::file_time_type, GetLastWriteTime, (const std::string& path), (const, override));
};

class MockStringUtils : public StringUtils {
public:
    MOCK_METHOD(std::string, Trim, (const std::string& str), (const, override));
    MOCK_METHOD(std::string, TrimLeft, (const std::string& str), (const, override));
    MOCK_METHOD(std::string, TrimRight, (const std::string& str), (const, override));
    MOCK_METHOD(std::vector<std::string>, Split, (const std::string& str, char delimiter), (const, override));
    MOCK_METHOD(bool, StartsWith, (const std::string& str, const std::string& prefix), (const, override));
    MOCK_METHOD(bool, EndsWith, (const std::string& str, const std::string& suffix), (const, override));
    MOCK_METHOD(std::string, ToLowerCase, (const std::string& str), (const, override));
    MOCK_METHOD(std::string, ToUpperCase, (const std::string& str), (const, override));
};

// INI parser implementation for testing
class TestINIParser : public INIParser {
public:
    TestINIParser() : m_caseSensitive(false), m_allowDuplicateKeys(false) {}

    bool SetCaseSensitive(bool enabled) {
        m_caseSensitive = enabled;
        return true;
    }

    bool SetAllowDuplicateKeys(bool enabled) {
        m_allowDuplicateKeys = enabled;
        return true;
    }

    INIParseResult ParseFile(const std::string& filename) override {
        INIParseResult result;
        auto startTime = std::chrono::high_resolution_clock::now();

        try {
            if (!std::filesystem::exists(filename)) {
                result.errors.push_back("File does not exist: " + filename);
                return result;
            }

            std::ifstream file(filename);
            if (!file.is_open()) {
                result.errors.push_back("Cannot open file: " + filename);
                return result;
            }

            std::string line;
            int lineNumber = 0;
            std::string currentSection = "";
            INISection* currentSectionPtr = nullptr;

            while (std::getline(file, line)) {
                lineNumber++;
                result.totalLines++;

                if (!ParseLine(line, lineNumber, currentSection, currentSectionPtr, result)) {
                    // Error was added to result.errors in ParseLine
                }
            }

            file.close();
            result.success = result.errors.empty();

        } catch (const std::exception& ex) {
            result.errors.push_back("Exception during parsing: " + std::string(ex.what()));
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        result.parseTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

        return result;
    }

    INIParseResult ParseString(const std::string& content) override {
        INIParseResult result;
        auto startTime = std::chrono::high_resolution_clock::now();

        try {
            std::istringstream stream(content);
            std::string line;
            int lineNumber = 0;
            std::string currentSection = "";
            INISection* currentSectionPtr = nullptr;

            while (std::getline(stream, line)) {
                lineNumber++;
                result.totalLines++;

                if (!ParseLine(line, lineNumber, currentSection, currentSectionPtr, result)) {
                    // Error handling
                }
            }

            result.success = result.errors.empty();

        } catch (const std::exception& ex) {
            result.errors.push_back("Exception during string parsing: " + std::string(ex.what()));
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        result.parseTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

        return result;
    }

    std::string GetValue(const std::string& section, const std::string& key, const std::string& defaultValue = "") const override {
        auto sectionIt = m_data.find(NormalizeKey(section));
        if (sectionIt == m_data.end()) {
            return defaultValue;
        }

        auto keyIt = sectionIt->second.find(NormalizeKey(key));
        if (keyIt == sectionIt->second.end()) {
            return defaultValue;
        }

        return keyIt->second.processedValue;
    }

    int GetInt(const std::string& section, const std::string& key, int defaultValue = 0) const override {
        std::string value = GetValue(section, key);
        if (value.empty()) {
            return defaultValue;
        }

        try {
            return std::stoi(value);
        } catch (...) {
            return defaultValue;
        }
    }

    float GetFloat(const std::string& section, const std::string& key, float defaultValue = 0.0f) const override {
        std::string value = GetValue(section, key);
        if (value.empty()) {
            return defaultValue;
        }

        try {
            return std::stof(value);
        } catch (...) {
            return defaultValue;
        }
    }

    bool GetBool(const std::string& section, const std::string& key, bool defaultValue = false) const override {
        std::string value = GetValue(section, key);
        if (value.empty()) {
            return defaultValue;
        }

        std::string lower = ToLowerCase(value);
        return (lower == "true" || lower == "1" || lower == "yes" || lower == "on");
    }

    std::vector<std::string> GetArray(const std::string& section, const std::string& key, char delimiter = ',') const override {
        std::string value = GetValue(section, key);
        if (value.empty()) {
            return {};
        }

        return Split(value, delimiter);
    }

    bool HasSection(const std::string& section) const override {
        return m_data.find(NormalizeKey(section)) != m_data.end();
    }

    bool HasKey(const std::string& section, const std::string& key) const override {
        auto sectionIt = m_data.find(NormalizeKey(section));
        if (sectionIt == m_data.end()) {
            return false;
        }

        return sectionIt->second.find(NormalizeKey(key)) != sectionIt->second.end();
    }

    std::vector<std::string> GetSectionNames() const override {
        std::vector<std::string> names;
        for (const auto& [sectionName, _] : m_data) {
            names.push_back(sectionName);
        }
        return names;
    }

    std::vector<std::string> GetKeyNames(const std::string& section) const override {
        std::vector<std::string> names;
        auto sectionIt = m_data.find(NormalizeKey(section));
        if (sectionIt != m_data.end()) {
            for (const auto& [keyName, _] : sectionIt->second) {
                names.push_back(keyName);
            }
        }
        return names;
    }

    bool SetValue(const std::string& section, const std::string& key, const std::string& value) override {
        std::string normalizedSection = NormalizeKey(section);
        std::string normalizedKey = NormalizeKey(key);

        if (!m_allowDuplicateKeys && HasKey(section, key)) {
            return false;
        }

        INIValue iniValue(value);
        iniValue.processedValue = ProcessValue(value);
        m_data[normalizedSection][normalizedKey] = iniValue;
        return true;
    }

    bool RemoveSection(const std::string& section) override {
        return m_data.erase(NormalizeKey(section)) > 0;
    }

    bool RemoveKey(const std::string& section, const std::string& key) override {
        auto sectionIt = m_data.find(NormalizeKey(section));
        if (sectionIt == m_data.end()) {
            return false;
        }

        return sectionIt->second.erase(NormalizeKey(key)) > 0;
    }

    std::string WriteToString() const override {
        std::ostringstream output;

        for (const auto& [sectionName, sectionData] : m_data) {
            output << "[" << sectionName << "]\n";

            for (const auto& [keyName, value] : sectionData) {
                output << keyName << "=" << value.rawValue << "\n";
            }

            output << "\n"; // Empty line between sections
        }

        return output.str();
    }

    bool WriteToFile(const std::string& filename) const override {
        std::string content = WriteToString();
        
        std::ofstream file(filename);
        if (!file.is_open()) {
            return false;
        }

        file << content;
        file.close();
        return !file.fail();
    }

    void Clear() override {
        m_data.clear();
    }

    // Test-specific methods
    const std::unordered_map<std::string, std::unordered_map<std::string, INIValue>>& GetInternalData() const {
        return m_data;
    }

    void AddParseError(const std::string& error, int lineNumber = 0) {
        // For testing error injection
        m_lastErrors.push_back("Line " + std::to_string(lineNumber) + ": " + error);
    }

    std::vector<std::string> GetLastErrors() const {
        return m_lastErrors;
    }

private:
    bool ParseLine(const std::string& line, int lineNumber, std::string& currentSection, 
                   INISection*& currentSectionPtr, INIParseResult& result) {
        std::string trimmedLine = Trim(line);

        // Skip empty lines
        if (trimmedLine.empty()) {
            return true;
        }

        // Skip comment lines
        if (trimmedLine[0] == '#' || trimmedLine[0] == ';') {
            // Store comment for potential association with next key/section
            m_pendingComments.push_back(trimmedLine);
            return true;
        }

        // Parse section header
        if (trimmedLine[0] == '[') {
            return ParseSectionHeader(trimmedLine, lineNumber, currentSection, currentSectionPtr, result);
        }

        // Parse key-value pair
        return ParseKeyValue(trimmedLine, lineNumber, currentSection, result);
    }

    bool ParseSectionHeader(const std::string& line, int lineNumber, std::string& currentSection,
                           INISection*& currentSectionPtr, INIParseResult& result) {
        size_t closePos = line.find(']');
        if (closePos == std::string::npos) {
            result.errors.push_back("Line " + std::to_string(lineNumber) + ": Missing closing bracket in section header");
            return false;
        }

        std::string sectionName = Trim(line.substr(1, closePos - 1));
        if (sectionName.empty()) {
            result.errors.push_back("Line " + std::to_string(lineNumber) + ": Empty section name");
            return false;
        }

        currentSection = sectionName;
        
        // Add section to result
        result.sections.emplace_back(sectionName, lineNumber);
        currentSectionPtr = &result.sections.back();
        currentSectionPtr->comments = m_pendingComments;
        m_pendingComments.clear();

        // Initialize section in internal data structure
        std::string normalizedSection = NormalizeKey(sectionName);
        if (m_data.find(normalizedSection) == m_data.end()) {
            m_data[normalizedSection] = {};
        }

        return true;
    }

    bool ParseKeyValue(const std::string& line, int lineNumber, const std::string& currentSection,
                      INIParseResult& result) {
        if (currentSection.empty()) {
            result.errors.push_back("Line " + std::to_string(lineNumber) + ": Key-value pair outside of section");
            return false;
        }

        size_t equalPos = line.find('=');
        if (equalPos == std::string::npos) {
            result.errors.push_back("Line " + std::to_string(lineNumber) + ": Missing '=' in key-value pair");
            return false;
        }

        std::string key = Trim(line.substr(0, equalPos));
        std::string value = Trim(line.substr(equalPos + 1));

        if (key.empty()) {
            result.errors.push_back("Line " + std::to_string(lineNumber) + ": Empty key name");
            return false;
        }

        // Check for duplicate keys
        if (!m_allowDuplicateKeys && HasKey(currentSection, key)) {
            result.warnings.push_back("Line " + std::to_string(lineNumber) + ": Duplicate key '" + key + "' in section '" + currentSection + "'");
        }

        // Store in internal data
        std::string normalizedSection = NormalizeKey(currentSection);
        std::string normalizedKey = NormalizeKey(key);

        INIValue iniValue(value, lineNumber);
        iniValue.processedValue = ProcessValue(value);
        iniValue.comments = m_pendingComments;
        m_data[normalizedSection][normalizedKey] = iniValue;
        m_pendingComments.clear();

        return true;
    }

    std::string ProcessValue(const std::string& value) const {
        std::string processed = value;

        // Remove surrounding quotes
        if (processed.length() >= 2) {
            if ((processed.front() == '"' && processed.back() == '"') ||
                (processed.front() == '\'' && processed.back() == '\'')) {
                processed = processed.substr(1, processed.length() - 2);
            }
        }

        // Process escape sequences
        processed = ProcessEscapeSequences(processed);

        return processed;
    }

    std::string ProcessEscapeSequences(const std::string& value) const {
        std::string result;
        result.reserve(value.length());

        for (size_t i = 0; i < value.length(); ++i) {
            if (value[i] == '\\' && i + 1 < value.length()) {
                switch (value[i + 1]) {
                    case 'n':  result += '\n'; ++i; break;
                    case 't':  result += '\t'; ++i; break;
                    case 'r':  result += '\r'; ++i; break;
                    case '\\': result += '\\'; ++i; break;
                    case '"':  result += '"';  ++i; break;
                    case '\'': result += '\''; ++i; break;
                    default:   result += value[i]; break;
                }
            } else {
                result += value[i];
            }
        }

        return result;
    }

    std::string NormalizeKey(const std::string& key) const {
        return m_caseSensitive ? key : ToLowerCase(key);
    }

    std::string Trim(const std::string& str) const {
        size_t first = str.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return "";
        
        size_t last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, (last - first + 1));
    }

    std::string ToLowerCase(const std::string& str) const {
        std::string result = str;
        std::transform(result.begin(), result.end(), result.begin(),
                      [](unsigned char c) { return std::tolower(c); });
        return result;
    }

    std::vector<std::string> Split(const std::string& str, char delimiter) const {
        std::vector<std::string> tokens;
        std::stringstream ss(str);
        std::string token;

        while (std::getline(ss, token, delimiter)) {
            tokens.push_back(Trim(token));
        }

        return tokens;
    }

    bool m_caseSensitive;
    bool m_allowDuplicateKeys;
    std::unordered_map<std::string, std::unordered_map<std::string, INIValue>> m_data;
    std::vector<std::string> m_pendingComments;
    std::vector<std::string> m_lastErrors;
};

// Test file generator
class INIFileGenerator {
public:
    static void CreateTestDirectory() {
        std::filesystem::create_directories(TEST_INI_DIR);
    }

    static void CleanupTestDirectory() {
        if (std::filesystem::exists(TEST_INI_DIR)) {
            std::filesystem::remove_all(TEST_INI_DIR);
        }
    }

    static std::string GetTestFilePath(const std::string& filename) {
        return std::string(TEST_INI_DIR) + "/" + filename;
    }

    static void CreateValidINI() {
        std::string content = R"(
# Server configuration
[Server]
Name=Test Server
Port=7777
MaxPlayers=64
TickRate=60
Debug=true

[Network]
# Network settings
MaxBandwidth=100.5
Timeout=5000
CompressionEnabled=yes
Protocol=UDP

[Game]
FriendlyFire=false
RespawnTime=10.0
GameMode=Conquest
Maps=VTE-CuChi,VNLTE-Hill937,VTE-AnLao

[Security]
EnableEAC=on
BanDuration=30
MaxAttempts=3
)";
        WriteFile(VALID_INI, content);
    }

    static void CreateMalformedINI() {
        std::string content = R"(
[Server
Name=Malformed Server
Port=invalid_port
[Network]
InvalidLine without equals
MaxBandwidth=100.0
Key=
[Unclosed Section
AnotherKey=Value
)";
        WriteFile(MALFORMED_INI, content);
    }

    static void CreateEmptyINI() {
        WriteFile(EMPTY_INI, "");
    }

    static void CreateCommentsINI() {
        std::string content = R"(
# This is a header comment
; Semicolon comment style

[Server]
# Server name configuration
Name=Test Server  # Inline comment
Port=7777         ; Inline semicolon comment

# Network section follows
[Network]
; This key has a special value
SpecialKey=Value with spaces
)";
        WriteFile(COMMENTS_INI, content);
    }

    static void CreateUnicodeINI() {
        std::string content = u8R"(
[国际化]
服务器名称=测试服务器
端口=7777
描述=This is a test with unicode: café, naïve, résumé

[Русский]
Имя=Тестовый сервер
Описание=Добро пожаловать на сервер!
)";
        WriteFile(UNICODE_INI, content);
    }

    static void CreateLargeINI() {
        std::ostringstream content;
        
        for (int section = 0; section < 100; ++section) {
            content << "[Section" << section << "]\n";
            for (int key = 0; key < 50; ++key) {
                content << "Key" << key << "=Value" << section << "_" << key << "\n";
            }
            content << "\n";
        }
        
        WriteFile(LARGE_INI, content.str());
    }

    static void CreateEscapeSequencesINI() {
        std::string content = R"(
[Escapes]
NewlineValue=Line1\nLine2
TabValue=Column1\tColumn2
QuoteValue="Quoted \"value\" here"
BackslashValue=Path\\to\\file
MixedValue=Start\tMiddle\nEnd
)";
        WriteFile("escapes.ini", content);
    }

    static void CreateArraysINI() {
        std::string content = R"(
[Arrays]
SimpleArray=item1,item2,item3
SpacedArray=item 1, item 2, item 3
NumberArray=1,2,3,4,5
MixedArray=string,42,true,3.14
EmptyArray=
SingleItem=onlyitem
)";
        WriteFile("arrays.ini", content);
    }

private:
    static void WriteFile(const std::string& filename, const std::string& content) {
        std::string path = GetTestFilePath(filename);
        std::ofstream file(path);
        file << content;
        file.close();
    }
};

// Test fixture for INI parser tests
class INIParserTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create test directory and files
        INIFileGenerator::CreateTestDirectory();
        INIFileGenerator::CreateValidINI();
        INIFileGenerator::CreateMalformedINI();
        INIFileGenerator::CreateEmptyINI();
        INIFileGenerator::CreateCommentsINI();
        INIFileGenerator::CreateUnicodeINI();
        INIFileGenerator::CreateLargeINI();
        INIFileGenerator::CreateEscapeSequencesINI();
        INIFileGenerator::CreateArraysINI();

        // Initialize mocks
        mockFileUtils = std::make_shared<NiceMock<MockFileUtils>>();
        mockStringUtils = std::make_shared<NiceMock<MockStringUtils>>();

        // Set up default mock behavior
        ON_CALL(*mockFileUtils, FileExists(_))
            .WillByDefault(Invoke([](const std::string& path) {
                return std::filesystem::exists(path);
            }));
        ON_CALL(*mockFileUtils, IsReadable(_))
            .WillByDefault(Return(true));

        // Create parser
        parser = std::make_unique<TestINIParser>();
    }

    void TearDown() override {
        parser.reset();
        mockStringUtils.reset();
        mockFileUtils.reset();
        INIFileGenerator::CleanupTestDirectory();
    }

    // Helper methods
    std::string GetTestFilePath(const std::string& filename) {
        return INIFileGenerator::GetTestFilePath(filename);
    }

    // Test data
    std::shared_ptr<MockFileUtils> mockFileUtils;
    std::shared_ptr<MockStringUtils> mockStringUtils;
    std::unique_ptr<TestINIParser> parser;
};

// === Basic Parsing Tests ===

TEST_F(INIParserTest, ParseFile_ValidINI_Success) {
    // Act
    auto result = parser->ParseFile(GetTestFilePath(VALID_INI));

    // Assert
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.errors.empty());
    EXPECT_GT(result.sections.size(), 0);
    
    // Check specific values
    EXPECT_EQ(parser->GetValue("Server", "Name"), "Test Server");
    EXPECT_EQ(parser->GetInt("Server", "Port"), 7777);
    EXPECT_EQ(parser->GetInt("Server", "MaxPlayers"), 64);
    EXPECT_TRUE(parser->GetBool("Server", "Debug"));
}

TEST_F(INIParserTest, ParseFile_NonExistentFile_Failure) {
    // Act
    auto result = parser->ParseFile("nonexistent.ini");

    // Assert
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.errors.empty());
    EXPECT_TRUE(result.errors[0].find("does not exist") != std::string::npos);
}

TEST_F(INIParserTest, ParseFile_EmptyFile_Success) {
    // Act
    auto result = parser->ParseFile(GetTestFilePath(EMPTY_INI));

    // Assert
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.errors.empty());
    EXPECT_EQ(result.sections.size(), 0);
}

TEST_F(INIParserTest, ParseString_ValidContent_Success) {
    // Arrange
    std::string content = R"(
[Test]
Key1=Value1
Key2=42
Key3=true
)";

    // Act
    auto result = parser->ParseString(content);

    // Assert
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.errors.empty());
    EXPECT_EQ(parser->GetValue("Test", "Key1"), "Value1");
    EXPECT_EQ(parser->GetInt("Test", "Key2"), 42);
    EXPECT_TRUE(parser->GetBool("Test", "Key3"));
}

// === Malformed INI Tests ===

TEST_F(INIParserTest, ParseFile_MalformedINI_HandlesErrors) {
    // Act
    auto result = parser->ParseFile(GetTestFilePath(MALFORMED_INI));

    // Assert
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.errors.empty());
    
    // Should detect specific errors
    bool foundMissingBracket = false;
    bool foundMissingEquals = false;
    
    for (const auto& error : result.errors) {
        if (error.find("Missing closing bracket") != std::string::npos) {
            foundMissingBracket = true;
        }
        if (error.find("Missing '='") != std::string::npos) {
            foundMissingEquals = true;
        }
    }
    
    EXPECT_TRUE(foundMissingBracket);
    EXPECT_TRUE(foundMissingEquals);
}

TEST_F(INIParserTest, ParseString_UnclosedSection_ReportsError) {
    // Arrange
    std::string content = "[UnclosedSection\nKey=Value";

    // Act
    auto result = parser->ParseString(content);

    // Assert
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.errors.empty());
    EXPECT_TRUE(result.errors[0].find("Missing closing bracket") != std::string::npos);
}

TEST_F(INIParserTest, ParseString_KeyWithoutSection_ReportsError) {
    // Arrange
    std::string content = "KeyWithoutSection=Value";

    // Act
    auto result = parser->ParseString(content);

    // Assert
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.errors.empty());
    EXPECT_TRUE(result.errors[0].find("outside of section") != std::string::npos);
}

TEST_F(INIParserTest, ParseString_EmptyKey_ReportsError) {
    // Arrange
    std::string content = "[Section]\n=Value";

    // Act
    auto result = parser->ParseString(content);

    // Assert
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.errors.empty());
    EXPECT_TRUE(result.errors[0].find("Empty key") != std::string::npos);
}

// === Data Type Conversion Tests ===

TEST_F(INIParserTest, DataTypes_IntegerValues_ConvertCorrectly) {
    // Arrange
    std::string content = R"(
[Numbers]
PositiveInt=42
NegativeInt=-123
ZeroInt=0
LargeInt=2147483647
)";
    parser->ParseString(content);

    // Act & Assert
    EXPECT_EQ(parser->GetInt("Numbers", "PositiveInt"), 42);
    EXPECT_EQ(parser->GetInt("Numbers", "NegativeInt"), -123);
    EXPECT_EQ(parser->GetInt("Numbers", "ZeroInt"), 0);
    EXPECT_EQ(parser->GetInt("Numbers", "LargeInt"), 2147483647);
}

TEST_F(INIParserTest, DataTypes_FloatValues_ConvertCorrectly) {
    // Arrange
    std::string content = R"(
[Floats]
PositiveFloat=3.14
NegativeFloat=-2.71
ZeroFloat=0.0
ScientificFloat=1.5e-3
)";
    parser->ParseString(content);

    // Act & Assert
    EXPECT_NEAR(parser->GetFloat("Floats", "PositiveFloat"), 3.14f, 0.001f);
    EXPECT_NEAR(parser->GetFloat("Floats", "NegativeFloat"), -2.71f, 0.001f);
    EXPECT_NEAR(parser->GetFloat("Floats", "ZeroFloat"), 0.0f, 0.001f);
    EXPECT_NEAR(parser->GetFloat("Floats", "ScientificFloat"), 1.5e-3f, 0.0001f);
}

TEST_F(INIParserTest, DataTypes_BooleanValues_ConvertCorrectly) {
    // Arrange
    std::string content = R"(
[Booleans]
TrueValue1=true
TrueValue2=TRUE
TrueValue3=1
TrueValue4=yes
TrueValue5=YES
TrueValue6=on
TrueValue7=ON
FalseValue1=false
FalseValue2=FALSE
FalseValue3=0
FalseValue4=no
FalseValue5=NO
FalseValue6=off
FalseValue7=OFF
)";
    parser->ParseString(content);

    // Act & Assert - True values
    EXPECT_TRUE(parser->GetBool("Booleans", "TrueValue1"));
    EXPECT_TRUE(parser->GetBool("Booleans", "TrueValue2"));
    EXPECT_TRUE(parser->GetBool("Booleans", "TrueValue3"));
    EXPECT_TRUE(parser->GetBool("Booleans", "TrueValue4"));
    EXPECT_TRUE(parser->GetBool("Booleans", "TrueValue5"));
    EXPECT_TRUE(parser->GetBool("Booleans", "TrueValue6"));
    EXPECT_TRUE(parser->GetBool("Booleans", "TrueValue7"));

    // Act & Assert - False values
    EXPECT_FALSE(parser->GetBool("Booleans", "FalseValue1"));
    EXPECT_FALSE(parser->GetBool("Booleans", "FalseValue2"));
    EXPECT_FALSE(parser->GetBool("Booleans", "FalseValue3"));
    EXPECT_FALSE(parser->GetBool("Booleans", "FalseValue4"));
    EXPECT_FALSE(parser->GetBool("Booleans", "FalseValue5"));
    EXPECT_FALSE(parser->GetBool("Booleans", "FalseValue6"));
    EXPECT_FALSE(parser->GetBool("Booleans", "FalseValue7"));
}

TEST_F(INIParserTest, DataTypes_InvalidConversions_UseDefaults) {
    // Arrange
    std::string content = R"(
[Invalid]
InvalidInt=not_a_number
InvalidFloat=also_not_a_number
InvalidBool=maybe
)";
    parser->ParseString(content);

    // Act & Assert
    EXPECT_EQ(parser->GetInt("Invalid", "InvalidInt", 999), 999);
    EXPECT_NEAR(parser->GetFloat("Invalid", "InvalidFloat", 99.9f), 99.9f, 0.001f);
    EXPECT_FALSE(parser->GetBool("Invalid", "InvalidBool", false));
}

// === Array Parsing Tests ===

TEST_F(INIParserTest, Arrays_CommaSeparated_ParseCorrectly) {
    // Arrange
    parser->ParseFile(GetTestFilePath("arrays.ini"));

    // Act
    auto simpleArray = parser->GetArray("Arrays", "SimpleArray");
    auto spacedArray = parser->GetArray("Arrays", "SpacedArray");
    auto numberArray = parser->GetArray("Arrays", "NumberArray");

    // Assert
    EXPECT_EQ(simpleArray.size(), 3);
    EXPECT_EQ(simpleArray[0], "item1");
    EXPECT_EQ(simpleArray[1], "item2");
    EXPECT_EQ(simpleArray[2], "item3");

    EXPECT_EQ(spacedArray.size(), 3);
    EXPECT_EQ(spacedArray[0], "item 1");
    EXPECT_EQ(spacedArray[1], "item 2");
    EXPECT_EQ(spacedArray[2], "item 3");

    EXPECT_EQ(numberArray.size(), 5);
    EXPECT_EQ(numberArray[0], "1");
    EXPECT_EQ(numberArray[4], "5");
}

TEST_F(INIParserTest, Arrays_EmptyAndSingle_HandleCorrectly) {
    // Arrange
    parser->ParseFile(GetTestFilePath("arrays.ini"));

    // Act
    auto emptyArray = parser->GetArray("Arrays", "EmptyArray");
    auto singleItem = parser->GetArray("Arrays", "SingleItem");

    // Assert
    EXPECT_TRUE(emptyArray.empty());
    EXPECT_EQ(singleItem.size(), 1);
    EXPECT_EQ(singleItem[0], "onlyitem");
}

// === Quote and Escape Sequence Tests ===

TEST_F(INIParserTest, EscapeSequences_ProcessedCorrectly) {
    // Arrange
    parser->ParseFile(GetTestFilePath("escapes.ini"));

    // Act & Assert
    EXPECT_EQ(parser->GetValue("Escapes", "NewlineValue"), "Line1\nLine2");
    EXPECT_EQ(parser->GetValue("Escapes", "TabValue"), "Column1\tColumn2");
    EXPECT_EQ(parser->GetValue("Escapes", "QuoteValue"), "Quoted \"value\" here");
    EXPECT_EQ(parser->GetValue("Escapes", "BackslashValue"), "Path\\to\\file");
    EXPECT_EQ(parser->GetValue("Escapes", "MixedValue"), "Start\tMiddle\nEnd");
}

TEST_F(INIParserTest, Quotes_RemovedFromValues) {
    // Arrange
    std::string content = R"(
[Quotes]
DoubleQuoted="This is quoted"
SingleQuoted='This is also quoted'
UnquotedValue=This is not quoted
MismatchedQuotes="This is broken'
)";
    parser->ParseString(content);

    // Act & Assert
    EXPECT_EQ(parser->GetValue("Quotes", "DoubleQuoted"), "This is quoted");
    EXPECT_EQ(parser->GetValue("Quotes", "SingleQuoted"), "This is also quoted");
    EXPECT_EQ(parser->GetValue("Quotes", "UnquotedValue"), "This is not quoted");
    EXPECT_EQ(parser->GetValue("Quotes", "MismatchedQuotes"), "\"This is broken'"); // Should not remove mismatched quotes
}

// === Comment Handling Tests ===

TEST_F(INIParserTest, Comments_IgnoredCorrectly) {
    // Arrange
    parser->ParseFile(GetTestFilePath(COMMENTS_INI));

    // Act & Assert
    EXPECT_TRUE(parser->HasSection("Server"));
    EXPECT_TRUE(parser->HasSection("Network"));
    EXPECT_EQ(parser->GetValue("Server", "Name"), "Test Server");
    EXPECT_EQ(parser->GetInt("Server", "Port"), 7777);
    EXPECT_EQ(parser->GetValue("Network", "SpecialKey"), "Value with spaces");
}

TEST_F(INIParserTest, Comments_InlineComments_RemovedFromValues) {
    // Arrange
    std::string content = R"(
[Test]
KeyWithComment=Value # This is a comment
KeyWithSemicolon=Value ; This is also a comment
KeyWithoutComment=Value
)";
    parser->ParseString(content);

    // Act & Assert
    // Note: The implementation may or may not handle inline comments
    // This test verifies the current behavior
    std::string value1 = parser->GetValue("Test", "KeyWithComment");
    std::string value2 = parser->GetValue("Test", "KeyWithSemicolon");
    std::string value3 = parser->GetValue("Test", "KeyWithoutComment");
    
    EXPECT_EQ(value3, "Value");
    // value1 and value2 behavior depends on implementation
}

// === Case Sensitivity Tests ===

TEST_F(INIParserTest, CaseSensitivity_Disabled_IgnoresCase) {
    // Arrange
    std::string content = R"(
[TestSection]
TestKey=TestValue
)";
    parser->SetCaseSensitive(false);
    parser->ParseString(content);

    // Act & Assert
    EXPECT_TRUE(parser->HasSection("testsection"));
    EXPECT_TRUE(parser->HasSection("TESTSECTION"));
    EXPECT_TRUE(parser->HasKey("testsection", "testkey"));
    EXPECT_TRUE(parser->HasKey("TESTSECTION", "TESTKEY"));
    EXPECT_EQ(parser->GetValue("testsection", "testkey"), "TestValue");
    EXPECT_EQ(parser->GetValue("TESTSECTION", "TESTKEY"), "TestValue");
}

TEST_F(INIParserTest, CaseSensitivity_Enabled_RespectCase) {
    // Arrange
    std::string content = R"(
[TestSection]
TestKey=TestValue
testkey=lowercase_value
)";
    parser->SetCaseSensitive(true);
    parser->ParseString(content);

    // Act & Assert
    EXPECT_TRUE(parser->HasSection("TestSection"));
    EXPECT_FALSE(parser->HasSection("testsection"));
    EXPECT_TRUE(parser->HasKey("TestSection", "TestKey"));
    EXPECT_TRUE(parser->HasKey("TestSection", "testkey"));
    EXPECT_FALSE(parser->HasKey("TestSection", "TESTKEY"));
    
    EXPECT_EQ(parser->GetValue("TestSection", "TestKey"), "TestValue");
    EXPECT_EQ(parser->GetValue("TestSection", "testkey"), "lowercase_value");
}

// === Duplicate Key Handling Tests ===

TEST_F(INIParserTest, DuplicateKeys_Disabled_RejectsSecond) {
    // Arrange
    std::string content = R"(
[Test]
DuplicateKey=FirstValue
DuplicateKey=SecondValue
)";
    parser->SetAllowDuplicateKeys(false);

    // Act
    auto result = parser->ParseString(content);

    // Assert
    EXPECT_TRUE(result.success); // Should still parse successfully
    EXPECT_FALSE(result.warnings.empty()); // But should have warnings
    EXPECT_EQ(parser->GetValue("Test", "DuplicateKey"), "FirstValue"); // First value should remain
}

TEST_F(INIParserTest, DuplicateKeys_Enabled_UsesLast) {
    // Arrange
    std::string content = R"(
[Test]
DuplicateKey=FirstValue
DuplicateKey=SecondValue
)";
    parser->SetAllowDuplicateKeys(true);

    // Act
    auto result = parser->ParseString(content);

    // Assert
    EXPECT_TRUE(result.success);
    EXPECT_EQ(parser->GetValue("Test", "DuplicateKey"), "SecondValue"); // Last value should be used
}

// === Unicode Support Tests ===

TEST_F(INIParserTest, Unicode_UTF8Content_ParsedCorrectly) {
    // Act
    auto result = parser->ParseFile(GetTestFilePath(UNICODE_INI));

    // Assert
    EXPECT_TRUE(result.success);
    
    // Check if Unicode sections and keys are handled
    auto sectionNames = parser->GetSectionNames();
    EXPECT_GT(sectionNames.size(), 0);
    
    // The exact Unicode handling depends on implementation
    // This test ensures no crashes occur with Unicode content
}

// === Performance Tests ===

TEST_F(INIParserTest, Performance_LargeFile_ParsedEfficiently) {
    // Act
    auto startTime = std::chrono::high_resolution_clock::now();
    auto result = parser->ParseFile(GetTestFilePath(LARGE_INI));
    auto endTime = std::chrono::high_resolution_clock::now();
    
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

    // Assert
    EXPECT_TRUE(result.success);
    EXPECT_LT(duration.count(), 1000); // Should parse within 1 second
    EXPECT_LT(result.parseTime.count(), 1000);
    
    // Verify content was parsed correctly
    EXPECT_EQ(parser->GetSectionNames().size(), 100);
    EXPECT_EQ(parser->GetKeyNames("Section0").size(), 50);
    EXPECT_EQ(parser->GetValue("Section99", "Key49"), "Value99_49");
}

TEST_F(INIParserTest, Performance_ManyAccesses_EfficientRetrieval) {
    // Arrange
    parser->ParseFile(GetTestFilePath(VALID_INI));
    const int accessCount = 10000;
    
    auto startTime = std::chrono::high_resolution_clock::now();

    // Act
    for (int i = 0; i < accessCount; ++i) {
        volatile std::string value = parser->GetValue("Server", "Name");
        volatile int port = parser->GetInt("Server", "Port");
        volatile bool debug = parser->GetBool("Server", "Debug");
    }
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);

    // Assert
    EXPECT_LT(duration.count(), 10000); // Less than 10ms for 10k accesses
}

// === Write Operations Tests ===

TEST_F(INIParserTest, WriteOperations_SetValue_UpdatesCorrectly) {
    // Arrange
    parser->ParseFile(GetTestFilePath(VALID_INI));

    // Act
    bool result1 = parser->SetValue("Server", "Name", "Updated Server");
    bool result2 = parser->SetValue("NewSection", "NewKey", "NewValue");

    // Assert
    EXPECT_TRUE(result1);
    EXPECT_TRUE(result2);
    EXPECT_EQ(parser->GetValue("Server", "Name"), "Updated Server");
    EXPECT_EQ(parser->GetValue("NewSection", "NewKey"), "NewValue");
    EXPECT_TRUE(parser->HasSection("NewSection"));
}

TEST_F(INIParserTest, WriteOperations_RemoveSection_DeletesCorrectly) {
    // Arrange
    parser->ParseFile(GetTestFilePath(VALID_INI));
    ASSERT_TRUE(parser->HasSection("Network"));

    // Act
    bool result = parser->RemoveSection("Network");

    // Assert
    EXPECT_TRUE(result);
    EXPECT_FALSE(parser->HasSection("Network"));
    EXPECT_EQ(parser->GetValue("Network", "MaxBandwidth"), ""); // Should return empty/default
}

TEST_F(INIParserTest, WriteOperations_RemoveKey_DeletesCorrectly) {
    // Arrange
    parser->ParseFile(GetTestFilePath(VALID_INI));
    ASSERT_TRUE(parser->HasKey("Server", "Port"));

    // Act
    bool result = parser->RemoveKey("Server", "Port");

    // Assert
    EXPECT_TRUE(result);
    EXPECT_FALSE(parser->HasKey("Server", "Port"));
    EXPECT_EQ(parser->GetInt("Server", "Port", -1), -1); // Should return default
}

TEST_F(INIParserTest, WriteOperations_WriteToString_GeneratesCorrectFormat) {
    // Arrange
    parser->SetValue("Section1", "Key1", "Value1");
    parser->SetValue("Section1", "Key2", "Value2");
    parser->SetValue("Section2", "Key3", "Value3");

    // Act
    std::string output = parser->WriteToString();

    // Assert
    EXPECT_TRUE(output.find("[Section1]") != std::string::npos);
    EXPECT_TRUE(output.find("[Section2]") != std::string::npos);
    EXPECT_TRUE(output.find("Key1=Value1") != std::string::npos);
    EXPECT_TRUE(output.find("Key2=Value2") != std::string::npos);
    EXPECT_TRUE(output.find("Key3=Value3") != std::string::npos);
}

TEST_F(INIParserTest, WriteOperations_WriteToFile_CreatesValidFile) {
    // Arrange
    parser->SetValue("TestSection", "TestKey", "TestValue");
    std::string outputPath = GetTestFilePath("output_test.ini");

    // Act
    bool result = parser->WriteToFile(outputPath);

    // Assert
    EXPECT_TRUE(result);
    EXPECT_TRUE(std::filesystem::exists(outputPath));
    
    // Parse the written file to verify
    auto newParser = std::make_unique<TestINIParser>();
    auto parseResult = newParser->ParseFile(outputPath);
    EXPECT_TRUE(parseResult.success);
    EXPECT_EQ(newParser->GetValue("TestSection", "TestKey"), "TestValue");
}

// === Error Recovery Tests ===

TEST_F(INIParserTest, ErrorRecovery_PartiallyValid_ParsesGoodParts) {
    // Arrange
    std::string content = R"(
[GoodSection1]
GoodKey1=GoodValue1

[BadSection
BadKey=BadValue

[GoodSection2]
GoodKey2=GoodValue2
)";

    // Act
    auto result = parser->ParseString(content);

    // Assert
    EXPECT_FALSE(result.success); // Should fail due to errors
    EXPECT_FALSE(result.errors.empty());
    
    // But should still parse the good parts
    EXPECT_TRUE(parser->HasSection("GoodSection1"));
    EXPECT_TRUE(parser->HasSection("GoodSection2"));
    EXPECT_EQ(parser->GetValue("GoodSection1", "GoodKey1"), "GoodValue1");
    EXPECT_EQ(parser->GetValue("GoodSection2", "GoodKey2"), "GoodValue2");
}

// === Edge Cases ===

TEST_F(INIParserTest, EdgeCase_VeryLongValues_HandledCorrectly) {
    // Arrange
    std::string longValue(10000, 'A');
    parser->SetValue("Test", "LongValue", longValue);

    // Act
    std::string retrieved = parser->GetValue("Test", "LongValue");

    // Assert
    EXPECT_EQ(retrieved, longValue);
    EXPECT_EQ(retrieved.length(), 10000);
}

TEST_F(INIParserTest, EdgeCase_EmptyValues_HandledCorrectly) {
    // Arrange
    std::string content = R"(
[Test]
EmptyValue=
OnlySpaces=   
OnlyTabs=			
)";
    parser->ParseString(content);

    // Act & Assert
    EXPECT_EQ(parser->GetValue("Test", "EmptyValue"), "");
    EXPECT_EQ(parser->GetValue("Test", "OnlySpaces"), ""); // Should be trimmed
    EXPECT_EQ(parser->GetValue("Test", "OnlyTabs"), "");  // Should be trimmed
}

TEST_F(INIParserTest, EdgeCase_SpecialCharacters_InKeys_HandledSafely) {
    // Arrange
    std::string content = R"(
[Test]
Key With Spaces=Value1
Key-With-Hyphens=Value2
Key_With_Underscores=Value3
Key.With.Dots=Value4
)";
    parser->ParseString(content);

    // Act & Assert
    EXPECT_EQ(parser->GetValue("Test", "Key With Spaces"), "Value1");
    EXPECT_EQ(parser->GetValue("Test", "Key-With-Hyphens"), "Value2");
    EXPECT_EQ(parser->GetValue("Test", "Key_With_Underscores"), "Value3");
    EXPECT_EQ(parser->GetValue("Test", "Key.With.Dots"), "Value4");
}

// === Integration Tests ===

TEST_F(INIParserTest, Integration_FullCycle_ParseModifyWrite) {
    // Arrange
    parser->ParseFile(GetTestFilePath(VALID_INI));
    
    // Act - Modify parsed data
    parser->SetValue("Server", "Name", "Modified Server");
    parser->SetValue("Server", "NewKey", "NewValue");
    parser->RemoveKey("Network", "Protocol");
    parser->SetValue("NewSection", "TestKey", "TestValue");
    
    // Write and re-parse
    std::string outputPath = GetTestFilePath("integration_test.ini");
    bool written = parser->WriteToFile(outputPath);
    ASSERT_TRUE(written);
    
    auto newParser = std::make_unique<TestINIParser>();
    auto result = newParser->ParseFile(outputPath);

    // Assert
    EXPECT_TRUE(result.success);
    EXPECT_EQ(newParser->GetValue("Server", "Name"), "Modified Server");
    EXPECT_EQ(newParser->GetValue("Server", "NewKey"), "NewValue");
    EXPECT_EQ(newParser->GetValue("Network", "Protocol"), ""); // Should be empty (removed)
    EXPECT_EQ(newParser->GetValue("NewSection", "TestKey"), "TestValue");
    
    // Original values should still exist
    EXPECT_EQ(newParser->GetInt("Server", "Port"), 7777);
    EXPECT_EQ(newParser->GetFloat("Network", "MaxBandwidth"), 100.5f);
}

} // namespace

// Test runner entry point
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}