# RS2V Server API Reference

This document provides a comprehensive reference for all public classes, methods, and functions exposed by the RS2V Custom Server codebase. It is organized by module and includes usage notes and default values where applicable.

## Table of Contents

1. [ConfigManager](#configmanager)  
2. [ConfigValidator](#configvalidator)  
3. [ServerConfig](#serverconfig)  
4. [NetworkConfig](#networkconfig)  
5. [SecurityConfig](#securityconfig)  
6. [PerformanceConfig](#performanceconfig)  
7. [DebugConfig](#debugconfig)  
8. [GameConfig](#gameconfig)  
9. [MapConfig](#mapconfig)  
10. [INIParser](#iniparser)  
11. [INIParserTypes](#iniparsertypes)  

## ConfigManager

Manages loading, saving, and querying the unified `config/server.ini`.  

### Constructors & Lifecycle  
- **ConfigManager()**  
- **~ConfigManager()**  

### Initialization  
- **bool Initialize()**  
  Ensures `config/` exists, loads `config/server.ini`, sets up watchers.

### Loading & Saving  
- **bool ReloadConfiguration()**  
  Reloads from `server.ini`, notifies listeners.  
- **bool ImportConfiguration(const std::string& sourceFile)**  
  Merges another INI into current settings.  
- **bool ExportConfiguration(const std::string& targetFile)**  
  Writes current settings to a new file.  
- **void ResetToDefaults()**  
  Clears and repopulates with built-in defaults.  
- **bool SaveAllConfigurations()**  
  Writes primary config file.  
- **bool SaveConfiguration(const std::string& configFile)**  
  Writes one file.

### Accessors & Mutators  
- **std::string GetString(const std::string& key, const std::string& defaultValue="") const**  
- **int GetInt(const std::string& key, int defaultValue=0) const**  
- **bool GetBool(const std::string& key, bool defaultValue=false) const**  
- **float GetFloat(const std::string& key, float defaultValue=0.0f) const**  
- **void SetString(const std::string& key, const std::string& value)**  
- **void SetInt(const std::string& key, int value)**  
- **void SetBool(const std::string& key, bool value)**  
- **void SetFloat(const std::string& key, float value)**  

### Structure Queries  
- **bool HasKey(const std::string& key) const**  
- **void RemoveKey(const std::string& key)**  
- **std::vector GetSectionKeys(const std::string& section) const**  
- **std::vector GetAllSections() const**  

### Auto-Save Control  
- **void SetAutoSave(bool enabled)**  
- **bool IsAutoSaveEnabled() const**  

### Change Notification  
- **void AddConfigurationListener(std::shared_ptr listener)**  
- **void RemoveConfigurationListener(std::shared_ptr listener)**  

## ConfigValidator

Validates the consolidated `server.ini` structure and contents.

### Constructors & Lifecycle  
- **ConfigValidator()**  
- **~ConfigValidator()**  

### Initialization  
- **bool Initialize()**  

### Validation  
- **ValidationResult ValidateConfiguration(const std::map& config)**  
- **bool ValidateConfigurationFile(const std::string& configFile)**  

**ValidationResult** fields:  
- `bool isValid`  
- `std::vector errors`  
- `std::vector warnings`

## ServerConfig

Typed getters for every key in `config/server.ini`.

### Constructor  
- **ServerConfig(const std::shared_ptr& mgr)**  

### General Section  
- **std::string GetServerName()**  
- **int GetMaxPlayers()**  
- **std::string GetMapRotationFile()**  
- **std::string GetGameModesFile()**  
- **std::string GetMotdFile()**  
- **int GetTickRate()**  
- **int GetTimeSyncInterval()**  
- **bool IsAnnouncementsEnabled()**  
- **std::string GetDataDirectory()**  
- **std::string GetLogDirectory()**  
- **bool IsAdminRconOnly()**  

### Network Section  
- **int GetPort()**  
- **std::string GetBindAddress()**  
- **int GetMaxPacketSize()**  
- **int GetClientIdleTimeout()** (seconds)  
- **int GetHeartbeatInterval()** (seconds)  
- **bool IsDualStack()**  
- **bool IsReliableTransport()**  

### Security Section  
- **bool IsSteamAuthEnabled()**  
- **bool IsFallbackCustomAuth()**  
- **std::string GetCustomAuthTokensFile()**  
- **bool IsBanManagerEnabled()**  
- **std::string GetBanListFile()**  
- **bool IsAntiCheatEnabled()**  
- **std::string GetAntiCheatMode()**  
- **std::string GetEacScannerConfigFile()**  

### Logging Section  
- **std::string GetLogLevel()**  
- **bool IsConsoleLogging()**  
- **bool IsFileLogging()**  
- **std::string GetLogFileName()**  
- **int GetLogMaxSizeMb()**  
- **int GetLogMaxFiles()**  
- **std::string GetLogTimestampFormat()**  

### Performance Section  
- **int GetMaxCpuCores()**  
- **int GetCpuAffinityMask()**  
- **bool IsDynamicTuningEnabled()**  

#### ThreadPool  
- **int GetWorkerThreadCount()**  
- **int GetMaxTaskQueueLength()**  
- **int GetSpillThreshold()**  

#### MemoryPool  
- **int GetPreallocateChunks()**  
- **int GetMaxChunks()**  

#### Profiler  
- **bool IsProfilingEnabled()**  
- **int GetProfilerMinRecordMs()**  
- **int GetProfilerBufferSize()**  
- **int GetProfilerFlushInterval()**  
- **std::string GetProfilerOutputFormat()**  
- **std::string GetProfilerOutputPath()**  

#### Telemetry  
- **int GetTelemetryBatchSize()**  
- **int GetTelemetryFlushInterval()**  

### Admin/RCON Section  
- **bool IsRconEnabled()**  
- **int GetRconPort()**  
- **std::string GetRconPassword()**  
- **int GetRconMinLevel()**  
- **std::string GetAdminListFile()**  
- **bool IsChatAuthEnabled()**  

## NetworkConfig

Holds network settings for sockets and timeouts.

### Constructor  
- **NetworkConfig(const ServerConfig& cfg)**  

### Accessors  
- **int GetPort()**  
- **const std::string& GetBindAddress()**  
- **int GetMaxPacketSize()**  
- **int GetClientIdleTimeoutMs()**  
- **int GetHeartbeatIntervalMs()**  
- **bool IsDualStack()**  
- **bool IsReliableTransport()**  

## SecurityConfig

Holds authentication, ban manager, and anti-cheat settings.

### Constructor  
- **SecurityConfig(const ServerConfig& cfg)**  

### Accessors  
- **bool IsSteamAuthEnabled()**  
- **bool IsFallbackCustomAuth()**  
- **const std::string& GetCustomAuthTokensFile()**  
- **bool IsBanManagerEnabled()**  
- **const std::string& GetBanListFile()**  
- **bool IsAntiCheatEnabled()**  
- **const std::string& GetAntiCheatMode()**  
- **const std::string& GetEacScannerConfigFile()**  

## PerformanceConfig

Holds all performance tuning parameters.

### Constructor  
- **PerformanceConfig(const ServerConfig& cfg)**  

### Accessors  
- **int GetMaxCpuCores()**  
- **int GetCpuAffinityMask()**  
- **bool IsDynamicTuningEnabled()**  

#### ThreadPool  
- **int GetWorkerThreadCount()**  
- **int GetMaxTaskQueueLength()**  
- **int GetSpillThreshold()**  

#### MemoryPool  
- **int GetPreallocateChunks()**  
- **int GetMaxChunks()**  

#### Profiler  
- **bool IsProfilingEnabled()**  
- **int GetProfilerMinRecordMs()**  
- **int GetProfilerBufferSize()**  
- **int GetProfilerFlushInterval()**  
- **const std::string& GetProfilerOutputFormat()**  
- **const std::string& GetProfilerOutputPath()**  

#### Telemetry  
- **int GetTelemetryBatchSize()**  
- **int GetTelemetryFlushInterval()**  

## DebugConfig

Holds debug and dynamic toggle settings for modules.

### Constructor  
- **DebugConfig(const ServerConfig& cfg)**  

### Accessors  
- **bool IsDebugEnabled()**  
- **bool IsLogToFile()**  
- **const std::string& GetDebugLogPath()**  
- **int GetLogMaxSizeMb()**  
- **int GetLogMaxFiles()**  
- **int GetVerbosityLevel()**  
- **bool IsConsoleDebugOutput()**  
- **bool IsModuleEnabled(const std::string& module)**  
- **int GetModuleVerbosity(const std::string& module)**  
- **bool GetToggle(const std::string& toggleName)**  
- **std::vector ListToggles()**  

## GameConfig

Provides game-specific rules and data paths.

### Constructor  
- **GameConfig(const ServerConfig& cfg)**  

### Accessors  
- **std::vector GetMapRotation()**  
- **std::vector GetGameModes()**  
- **bool IsFriendlyFire()**  
- **int GetRespawnDelay()**  
- **bool IsRoundTimerEnabled()**  
- **int GetRoundTimeLimit()**  
- **bool IsScoreLimitEnabled()**  
- **int GetScoreLimit()**  
- **bool IsVehicleSpawningEnabled()**  
- **std::string GetMapsIniPath()**  
- **std::string GetModesIniPath()**  
- **std::string GetTeamsIniPath()**  
- **std::string GetWeaponsIniPath()**  

## MapConfig

Loads and stores per-map metadata.

### Types  
- **struct MapDefinition**  

### Constructors & Lifecycle  
- **MapConfig()**  
- **~MapConfig()**  

### Methods  
- **bool Initialize(const std::string& configPath)**  
- **bool Load()**  
- **bool Save()**  
- **void CreateDefaultConfig()**  
- **const MapDefinition* GetDefinition(const std::string& name)**  
- **std::vector GetAvailableMaps()**  

## INIParser

Generic INI parsing utility.

### Constructors & Lifecycle  
- **INIParser()**  
- **~INIParser()**  

### Initialization & Settings  
- **bool Initialize()**  

### Parsing & Saving  
- **bool ParseFile(const std::string& filename)**  
- **bool ParseString(const std::string& content)**  
- **bool SaveToFile(const std::string& filename)**  

### Querying & Modification  
- **std::string GetValue(const std::string& section, const std::string& key, const std::string& defaultValue="") const**  
- **int GetIntValue(const std::string& section, const std::string& key, int defaultValue=0) const**  
- **float GetFloatValue(const std::string& section, const std::string& key, float defaultValue=0.0f) const**  
- **bool GetBoolValue(const std::string& section, const std::string& key, bool defaultValue=false) const**  
- **std::vector GetArrayValue(const std::string& section, const std::string& key, const std::vector& defaultValue={}) const**  
- **void SetValue(const std::string& section, const std::string& key, const std::string& value)**  
- **void SetIntValue(const std::string& section, const std::string& key, int value)**  
- **void SetFloatValue(const std::string& section, const std::string& key, float value)**  
- **void SetBoolValue(const std::string& section, const std::string& key, bool value)**  
- **void SetArrayValue(const std::string& section, const std::string& key, const std::vector& value)**  
- **bool HasSection(const std::string& section) const**  
- **bool HasKey(const std::string& section, const std::string& key) const**  
- **std::vector GetSectionNames() const**  
- **std::vector GetKeyNames(const std::string& section) const**  
- **void RemoveSection(const std::string& section)**  
- **void RemoveKey(const std::string& section, const std::string& key)**  

## INIParserTypes

Defines types used by INIParser.

```cpp
enum class ErrorSeverity { INFO, WARNING, ERROR };

struct INIKeyValue { std::string key, value; size_t lineNumber; };

struct INISection {
    std::string sectionName;
    std::vector keyValues;
};

struct ParsingError {
    ErrorSeverity severity;
    std::string message;
    size_t lineNumber;
    std::string contextSection;
    std::string contextKey;
};

struct ParserConfig {
    bool allowMultilineValues       = true;
    bool preserveComments           = true;
    bool caseInsensitiveSections    = false;
    bool caseInsensitiveKeys        = false;
    std::string commentDelimiters   = "#;";
    std::string assignmentOperator  = "=";
    bool trimWhitespaceAroundValues = true;
};

struct PreservedComment {
    std::string section, key, commentText;
    size_t      lineNumber;
};

using INIData = std::map;
```

*End of API Reference*