// src/Config/INIParserTypes.h — Type definitions for INI parser

#pragma once

#include <string>
#include <vector>
#include <map>
#include <utility>

enum class ErrorSeverity {
    WARNING,
    ERROR
};

struct ParsingError {
    std::string message;
    std::string section;
    std::string key;
    size_t      lineNumber = 0;
    std::string line;              // original line text for error context
    ErrorSeverity severity = ErrorSeverity::WARNING;
};

struct INIKeyValue {
    std::string key;
    std::string value;
    std::string comment;
    size_t      lineNumber = 0;
};

struct INISection {
    std::string sectionName;
    std::string name;
    size_t      lineNumber = 0;
    std::map<std::string, INIKeyValue> keyValues;
    std::vector<std::string> comments;
};

struct PreservedComment {
    std::string section;
    std::string key;
    std::string text;
    size_t      lineNumber = 0;
    bool        beforeSection = false;
    bool        beforeKey = false;
};

struct ParserConfig {
    bool allowEmptyValues        = true;
    bool allowDuplicateKeys      = false;
    bool caseSensitive           = false;
    bool caseInsensitiveKeys     = true;
    bool trimWhitespace          = true;
    bool allowMultilineValues    = false;
    bool preserveComments        = true;
    bool strictMode              = false;

    std::vector<char> commentChars     = {'#', ';'};
    std::pair<char, char> sectionBrackets = {'[', ']'};
    char keyValueSeparator             = '=';
    char escapeChar                    = '\\';
    char arraySeparator                = ',';
    std::vector<char> quotingChars     = {'"', '\''};

    std::string headerComment;
    std::string footerComment;
    std::string sectionNamePattern;
};
