// src/Game/ModManager.h – Mod (.pak) registration and client sync manifest

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

class ServerConfig;
class WorkshopManager;

// Tracks the gameplay mods and cosmetic assets the server expects clients to
// have. Sources:
//   * Steam Workshop items of type 'mod'/'asset' (via WorkshopManager), and
//   * loose .pak files placed in the configured mods directory.
//
// The manager validates each entry against disk and produces a manifest that the
// join/login path can require of connecting clients (docs/MAPS.md §6: mods and
// assets are "Required download before connecting").
class ModManager {
public:
    struct Mod {
        std::string name;        // logical id (filename stem)
        std::string fileName;    // e.g. night_ops.pak
        std::string path;        // resolved path on disk
        bool        isAsset = false; // true for cosmetic assets, false for gameplay mods
        bool        present = false;
        uint64_t    workshopId = 0;  // 0 if discovered locally rather than via Workshop
    };

    explicit ModManager(std::shared_ptr<ServerConfig> serverConfig);
    ~ModManager();

    // Populate from the Workshop manifest (optional) and a scan of the mods dir.
    bool Initialize(WorkshopManager* workshop);

    // Re-scan disk presence; returns the number of missing mods.
    size_t Validate();

    const std::vector<Mod>& GetMods() const { return m_mods; }
    std::vector<Mod> GetGameplayMods() const;
    std::vector<Mod> GetAssets() const;

    // Manifest of required client content as "name:fileName" entries.
    std::vector<std::string> GetClientManifest() const;

    bool AllPresent() const;
    void LogSummary() const;

private:
    void AddFromWorkshop(WorkshopManager* workshop);
    void ScanModsDirectory();
    bool HasMod(const std::string& fileName) const;

    std::shared_ptr<ServerConfig> m_serverConfig;
    std::string                   m_modsDir;
    std::vector<Mod>              m_mods;
};
