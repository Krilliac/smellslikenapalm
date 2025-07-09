// src/Config/INIParser.h

#pragma once

#include <string>
#include <vector>
#include <map>
#include "Config/INIParserTypes.h"  // Definitions for INISection, INIKeyValue, ParsingError, ParserConfig, ErrorSeverity

class INIParser {
public:
    enum class LineType {
        EMPTY,
        COMMENT,
        SECTION,
        KEY_VALUE,
        MULTILINE_START,
        INVALID
    };

    INIParser();
    ~INIParser();

    // Initialize parser settings and validation patterns
    bool Initialize();

    // Parse INI data from file or string
    bool ParseFile(const std::string& filename);
    bool ParseString(const std::string& content);

    // Save current data back to file
    bool SaveToFile(const std::string& filename);

    // Querying values
    std::string                GetValue(const std::string& section, const std::string& key, const std::string& defaultValue = "") const;
    int                        GetIntValue(const std::string& section, const std::string& key, int defaultValue = 0) const;
    float                      GetFloatValue(const std::string& section, const std::string& key, float defaultValue = 0.0f) const;
    bool                       GetBoolValue(const std::string& section, const std::string& key, bool defaultValue = false) const;
    std::vector<std::string>   GetArrayValue(const std::string& section, const std::string& key, const std::vector<std::string>& defaultValue = {}) const;

    // Modify values
    void SetValue(const std::string& section, const std::string& key, const std::string& value);
    void SetIntValue(const std::string& section, const std::string& key, int value);
    void SetFloatValue(const std::string& section, const std::string& key, float value);
    void SetBoolValue(const std::string& section, const std::string& key, bool value);
    void SetArrayValue(const std::string& section, const std::string& key, const std::vector<std::string>& value);

    // Section and key enumeration
    bool                      HasSection(const std::string& section) const;
    bool                      HasKey(const std::string& section, const std::string& key) const;
    std::vector<std::string>  GetSectionNames() const;
    std::vector<std::string>  GetKeyNames(const std::string& section) const;

    // Remove entries
    void RemoveSection(const std::string& section);
    void RemoveKey(const std::string& section, const std::string& key);

    // Debug and error reporting
    const std::vector<ParsingError>& GetParsingErrors() const;
    bool                             HasParsingErrors() const;
    void                             LogParsingStatistics();
    void                             LogParsingErrors();

    // Raw data access
    std::map<std::string, std::map<std::string, std::string>> GetAllData() const;
    void                                                     SetAllData(const std::map<std::string, std::map<std::string, std::string>>& data);

private:
    // Core parse routines
    bool ParseStream(std::istream& stream);
    LineType ProcessLine(std::string& line, std::string& currentSection, size_t lineNumber);
    LineType ProcessSectionLine(const std::string& line, std::string& currentSection, size_t lineNumber);
    LineType ProcessKeyValueLine(const std::string& line, const std::string& currentSection, size_t lineNumber);
    void    ProcessMultilineValue(std::istream& stream, const std::string& currentSection, size_t& lineNumber);

    // Helpers
    bool IsCommentLine(const std::string& line) const;
    bool IsSectionLine(const std::string& line) const;
    bool IsKeyValueLine(const std::string& line) const;
    bool IsMultilineStart(const std::string& line) const;
    bool IsValidSectionName(const std::string& name) const;
    bool IsValidKeyName(const std::string& name) const;
    std::string NormalizeKey(const std::string& key) const;
    std::string ProcessQuotedValue(const std::string& value) const;
    std::string ProcessEscapeSequences(const std::string& value) const;

    // Validation
    bool PostProcessValidation();
    bool ValidateRS2VConfiguration();
    void ValidateRS2VKeys();
    bool ValidateCrossSectionReferences();
    bool ValidateRequiredSections();
    bool ValidateValueFormats();
    void ValidatePortValues();
    void ValidateIPAddressValues();
    void ValidateBooleanValues();
    void ValidateNumericRanges();
    bool IsValidIPAddress(const std::string& ip) const;
    bool IsValidBooleanValue(const std::string& value) const;

    // Comment preservation
    void PreserveComment(const std::string& section, const std::string& comment, size_t lineNumber);
    void WritePreservedComments(std::ostream& stream, const std::string& section, bool beforeSection);
    void WritePreservedCommentsForKey(std::ostream& stream, const std::string& section, const std::string& key, bool beforeKey);

    // Writing routines
    bool WriteToStream(std::ostream& stream);
    void WriteSectionContent(std::ostream& stream, const INISection& section);
    void WriteComment(std::ostream& stream, const std::string& comment);
    bool ShouldQuoteValue(const std::string& value) const;
    std::string QuoteValue(const std::string& value) const;

    // Initialization
    void InitializeParserSettings();
    void InitializeValidationPatterns();

    // Data members
    std::map<std::string, INISection>       m_sections;
    std::vector<ParsingError>               m_parsingErrors;
    std::vector<PreservedComment>           m_preservedComments;
    ParserConfig                            m_parserConfig;
    std::string                             m_currentFile;
};