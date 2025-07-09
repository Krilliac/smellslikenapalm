// src/Config/INIParserTypes.h

#pragma once

#include <string>
#include <vector>
#include <map>

// Severity levels for parsing and validation errors
enum class ErrorSeverity {
    INFO,
    WARNING,
    ERROR
};

// Represents a single key/value pair within a section
struct INIKeyValue {
    std::string key;
    std::string value;
    size_t      lineNumber;
};

// Represents one section in the INI file, including its key/value pairs
struct INISection {
    std::string sectionName;
    std::vector<INIKeyValue> keyValues;
};

// Represents a parsing or validation error encountered during parsing
struct ParsingError {
    ErrorSeverity severity;
    std::string   message;
    size_t        lineNumber;
    std::string   contextSection;
    std::string   contextKey;
};

// Configuration options for the INI parser
struct ParserConfig {
    bool allowMultilineValues       = true;
    bool preserveComments           = true;
    bool caseInsensitiveSections    = false;
    bool caseInsensitiveKeys        = false;
    std::string commentDelimiters   = "#;";
    std::string assignmentOperator  = "=";
    bool trimWhitespaceAroundValues = true;
};

// Represents a preserved comment to be re-emitted on save
struct PreservedComment {
    std::string section;      // section name, empty for global
    std::string key;          // key name, empty if comment before section
    std::string commentText;  // the raw comment (without delimiter)
    size_t      lineNumber;   // original line number in source
};

// Convenience alias for the full parsed data structure
using INIData = std::map<std::string, INISection>;