// src/Config/ConfigManager.h

#pragma once

#include <string>
#include <map>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>

// Interface for receiving config change notifications
class IConfigurationListener {
public:
    virtual ~IConfigurationListener() = default;
    virtual void OnConfigurationChanged() = 0;
};

class ConfigManager {
public:
    ConfigManager();
    ~ConfigManager();

    // Initialize configuration system, loads config/server.ini
    bool Initialize();

    // Load a specific configuration file
    bool LoadConfiguration(const std::string& configFile);

    // Reload the current configuration from disk
    bool ReloadConfiguration();

    // Import and merge another configuration file
    bool ImportConfiguration(const std::string& sourceFile);

    // Export current configuration to a new file
    bool ExportConfiguration(const std::string& targetFile);

    // Reset to default built-in configuration values
    void ResetToDefaults();

    // Save all configurations back to disk
    bool SaveAllConfigurations();

    // Explicitly save to a given file
    bool SaveConfiguration(const std::string& configFile);

    // Read config values
    std::string GetString(const std::string& key, const std::string& defaultValue = "") const;
    int         GetInt(const std::string& key, int defaultValue = 0) const;
    bool        GetBool(const std::string& key, bool defaultValue = false) const;
    float       GetFloat(const std::string& key, float defaultValue = 0.0f) const;

    // Write config values
    void SetString(const std::string& key, const std::string& value);
    void SetInt(const std::string& key, int value);
    void SetBool(const std::string& key, bool value);
    void SetFloat(const std::string& key, float value);

    // Query config structure
    bool HasKey(const std::string& key) const;
    void RemoveKey(const std::string& key);
    std::vector<std::string> GetSectionKeys(const std::string& section) const;
    std::vector<std::string> GetAllSections() const;

    // Control auto-save behavior
    void SetAutoSave(bool enabled);
    bool IsAutoSaveEnabled() const;

    // Register listeners for config change notifications
    void AddConfigurationListener(std::shared_ptr<IConfigurationListener> listener);
    void RemoveConfigurationListener(std::shared_ptr<IConfigurationListener> listener);

    // Live configuration reloading
    bool StartFileWatcher();
    void StopFileWatcher();
    bool IsFileWatcherRunning() const;

    // Configuration backup and rollback
    bool BackupConfiguration(const std::string& backupPath = "");
    bool RollbackConfiguration(const std::string& backupPath = "");
    std::vector<std::string> GetAvailableBackups() const;

private:

    // Validation and application steps
    bool ValidateConfiguration();
    bool ValidateServerConfig();
    bool ValidateNetworkConfig();
    bool ValidateSecurityConfig();
    bool ValidateEACConfig();
    bool ValidateGameConfig();

    void ApplySecurityConfiguration();
    void ApplyEACConfiguration();

    // Watcher and notification
    void InitializeConfigWatchers();
    void NotifyConfigurationChanged();

    // Helpers for save formatting
    std::string GetConfigComment(const std::string& section, const std::string& key);
    std::string GetCurrentTimestamp();

    // File watcher thread function
    void FileWatcherThread();

    // Member variables
    std::string m_primaryConfigFile;
    std::map<std::string, std::string> m_configValues;
    bool m_autoSave = false;
    std::vector<std::weak_ptr<IConfigurationListener>> m_listeners;

    // File watcher state
    std::thread m_fileWatcherThread;
    std::atomic<bool> m_fileWatcherRunning{false};
    std::mutex m_configMutex;

    // Backup tracking
    std::string m_backupDirectory;
    std::string m_backupPrefix;
};