// src/Game/WorkshopManager.h – Steam Workshop item management

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>

class ServerConfig;

// Loads and manages the Steam Workshop items declared in
// config/workshop_items.txt (custom maps, gameplay mods, cosmetic assets).
//
// Responsibilities:
//   * parse the workshop manifest into structured items,
//   * resolve and validate each item's expected local file,
//   * optionally orchestrate downloads via steamcmd (RS2:V app id 418460),
//   * expose the set of items clients must have to join (docs/MAPS.md §6).
//
// Note: actual asset transport to the client (fast-download / Steam) is handled
// elsewhere; this class is the server-side source of truth for what is required
// and what is present on disk.
class WorkshopManager {
public:
    enum class ItemType { Map, Mod, Asset, Unknown };

    struct Item {
        uint64_t    workshopId = 0;
        ItemType    type       = ItemType::Unknown;
        std::string fileName;       // expected filename after extraction
        std::string localPath;      // path relative to the data directory
        std::string description;
        bool        present    = false;  // resolved file exists on disk
        std::string resolvedPath;        // absolute/relative path checked
    };

    explicit WorkshopManager(std::shared_ptr<ServerConfig> serverConfig);
    ~WorkshopManager();

    // Parse the manifest, resolve and validate items. Returns false only if the
    // manifest file could not be read (an empty/missing manifest is not an error).
    bool Initialize();

    // Re-read the manifest from disk (e.g. after an admin edit + reload).
    bool Reload();

    // Re-check the on-disk presence of every item. Returns the number missing.
    size_t ValidateItems();

    // Build the steamcmd command line that would download the given item.
    std::string BuildSteamCmdCommand(const Item& item) const;

    // Attempt to download all missing items via steamcmd. When download is
    // disabled (default), this only logs the commands that would run (dry-run).
    // Returns the number of items successfully downloaded (0 in dry-run).
    size_t DownloadMissing();

    const std::vector<Item>& GetItems() const { return m_items; }
    std::vector<Item>        GetMissingItems() const;
    std::vector<Item>        GetItemsByType(ItemType type) const;

    // The items a connecting client is required to have (all maps/mods/assets).
    std::vector<Item>        GetRequiredClientItems() const { return m_items; }

    bool   AllItemsPresent() const;
    void   LogSummary() const;

    static std::string TypeToString(ItemType t);
    static ItemType    ParseType(const std::string& s);

private:
    void ResolveAndValidate(Item& item) const;

    std::shared_ptr<ServerConfig> m_serverConfig;
    std::string                   m_manifestPath;
    std::string                   m_dataDir;
    uint32_t                      m_appId            = 418460; // RS2: Vietnam
    bool                          m_downloadEnabled  = false;
    std::string                   m_steamCmdPath     = "steamcmd";
    std::string                   m_workshopInstallDir;
    std::vector<Item>             m_items;
};
