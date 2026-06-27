// src/Game/WorkshopManager.cpp – Implementation of Steam Workshop item management

#include "Game/WorkshopManager.h"
#include "Config/ServerConfig.h"
#include "Utils/Logger.h"
#include "Utils/StringUtils.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdlib>

namespace {
std::string JoinPath(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    if (b.empty()) return a;
    std::string out = a;
    if (out.back() != '/' && out.back() != '\\') out += '/';
    // strip a leading separator on b to avoid doubling
    size_t start = (b.front() == '/' || b.front() == '\\') ? 1 : 0;
    out += b.substr(start);
    return out;
}
} // namespace

WorkshopManager::WorkshopManager(std::shared_ptr<ServerConfig> serverConfig)
    : m_serverConfig(std::move(serverConfig))
{
    if (m_serverConfig) {
        m_manifestPath       = m_serverConfig->GetWorkshopItemsFile();
        m_dataDir            = m_serverConfig->GetDataDirectory();
        m_appId              = static_cast<uint32_t>(m_serverConfig->GetWorkshopAppId());
        m_downloadEnabled    = m_serverConfig->IsWorkshopDownloadEnabled();
        m_steamCmdPath       = m_serverConfig->GetSteamCmdPath();
        m_workshopInstallDir = m_serverConfig->GetWorkshopInstallDir();
    } else {
        m_manifestPath = "config/workshop_items.txt";
        m_dataDir      = "data/";
    }
    Logger::Info("WorkshopManager initialized (manifest='%s', appId=%u, downloadEnabled=%s)",
                 m_manifestPath.c_str(), m_appId, m_downloadEnabled ? "true" : "false");
}

WorkshopManager::~WorkshopManager() = default;

std::string WorkshopManager::TypeToString(ItemType t)
{
    switch (t) {
        case ItemType::Map:   return "map";
        case ItemType::Mod:   return "mod";
        case ItemType::Asset: return "asset";
        default:              return "unknown";
    }
}

WorkshopManager::ItemType WorkshopManager::ParseType(const std::string& s)
{
    std::string l = StringUtils::ToLower(StringUtils::Trim(s));
    if (l == "map")   return ItemType::Map;
    if (l == "mod")   return ItemType::Mod;
    if (l == "asset") return ItemType::Asset;
    return ItemType::Unknown;
}

bool WorkshopManager::Initialize()
{
    return Reload();
}

bool WorkshopManager::Reload()
{
    m_items.clear();

    if (!std::filesystem::exists(m_manifestPath)) {
        Logger::Warn("WorkshopManager: manifest not found: %s (no workshop items registered)",
                     m_manifestPath.c_str());
        return false;
    }

    std::ifstream file(m_manifestPath);
    if (!file.is_open()) {
        Logger::Error("WorkshopManager: failed to open manifest: %s", m_manifestPath.c_str());
        return false;
    }

    std::string line;
    size_t lineNo = 0;
    while (std::getline(file, line)) {
        ++lineNo;
        // Strip inline comments (everything after the first '#') and trim.
        auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        line = StringUtils::Trim(line);
        if (line.empty()) continue;

        // Whitespace-delimited: <WorkshopID> <Type> <FileName> [<LocalPath>]
        std::istringstream iss(line);
        std::string idStr, typeStr, fileName, localPath;
        if (!(iss >> idStr >> typeStr >> fileName)) {
            Logger::Warn("WorkshopManager: malformed line %zu in %s: '%s'",
                         lineNo, m_manifestPath.c_str(), line.c_str());
            continue;
        }
        iss >> localPath; // optional

        Item item;
        try {
            item.workshopId = std::stoull(idStr);
        } catch (...) {
            Logger::Warn("WorkshopManager: invalid workshop id '%s' on line %zu", idStr.c_str(), lineNo);
            continue;
        }
        item.type        = ParseType(typeStr);
        item.fileName    = fileName;
        item.localPath   = localPath;
        item.description = "";
        if (item.type == ItemType::Unknown) {
            Logger::Warn("WorkshopManager: unknown item type '%s' on line %zu (treating as asset)",
                         typeStr.c_str(), lineNo);
            item.type = ItemType::Asset;
        }

        ResolveAndValidate(item);
        m_items.push_back(item);
    }
    file.close();

    size_t missing = 0;
    for (const auto& it : m_items) if (!it.present) ++missing;
    Logger::Info("WorkshopManager: loaded %zu workshop items (%zu missing on disk)",
                 m_items.size(), missing);
    return true;
}

void WorkshopManager::ResolveAndValidate(Item& item) const
{
    // Full path = <dataDir>/<localPath>/<fileName>, with localPath optional.
    std::string dir = item.localPath.empty() ? m_dataDir : JoinPath(m_dataDir, item.localPath);
    item.resolvedPath = JoinPath(dir, item.fileName);
    item.present = std::filesystem::exists(item.resolvedPath) &&
                   std::filesystem::is_regular_file(item.resolvedPath);
}

size_t WorkshopManager::ValidateItems()
{
    size_t missing = 0;
    for (auto& it : m_items) {
        ResolveAndValidate(it);
        if (!it.present) ++missing;
    }
    Logger::Info("WorkshopManager: validated %zu items, %zu missing", m_items.size(), missing);
    return missing;
}

std::string WorkshopManager::BuildSteamCmdCommand(const Item& item) const
{
    // steamcmd +force_install_dir <dir> +login anonymous
    //          +workshop_download_item <appId> <id> +quit
    std::ostringstream cmd;
    cmd << m_steamCmdPath;
    if (!m_workshopInstallDir.empty()) {
        cmd << " +force_install_dir \"" << m_workshopInstallDir << "\"";
    }
    cmd << " +login anonymous"
        << " +workshop_download_item " << m_appId << " " << item.workshopId
        << " +quit";
    return cmd.str();
}

size_t WorkshopManager::DownloadMissing()
{
    auto missing = GetMissingItems();
    if (missing.empty()) {
        Logger::Info("WorkshopManager: all workshop items already present; nothing to download");
        return 0;
    }

    size_t downloaded = 0;
    for (const auto& item : missing) {
        std::string cmd = BuildSteamCmdCommand(item);
        if (!m_downloadEnabled) {
            Logger::Info("WorkshopManager: [dry-run] would download item %llu (%s): %s",
                         (unsigned long long)item.workshopId,
                         TypeToString(item.type).c_str(), cmd.c_str());
            continue;
        }

        Logger::Info("WorkshopManager: downloading item %llu via steamcmd: %s",
                     (unsigned long long)item.workshopId, cmd.c_str());
        int rc = std::system(cmd.c_str());
        if (rc != 0) {
            Logger::Error("WorkshopManager: steamcmd returned %d for item %llu",
                          rc, (unsigned long long)item.workshopId);
            continue;
        }
        // Re-validate; the operator is expected to stage downloaded content into
        // the configured localPath (steamcmd places it under steamapps/workshop).
        Item probe = item;
        ResolveAndValidate(probe);
        if (probe.present) {
            Logger::Info("WorkshopManager: item %llu now present at %s",
                         (unsigned long long)item.workshopId, probe.resolvedPath.c_str());
            ++downloaded;
        } else {
            Logger::Warn("WorkshopManager: item %llu downloaded but not found at expected path %s "
                         "(stage it into the configured local_path)",
                         (unsigned long long)item.workshopId, probe.resolvedPath.c_str());
        }
    }

    // Refresh presence flags on the live item list.
    ValidateItems();
    return downloaded;
}

std::vector<WorkshopManager::Item> WorkshopManager::GetMissingItems() const
{
    std::vector<Item> out;
    for (const auto& it : m_items) if (!it.present) out.push_back(it);
    return out;
}

std::vector<WorkshopManager::Item> WorkshopManager::GetItemsByType(ItemType type) const
{
    std::vector<Item> out;
    for (const auto& it : m_items) if (it.type == type) out.push_back(it);
    return out;
}

bool WorkshopManager::AllItemsPresent() const
{
    for (const auto& it : m_items) if (!it.present) return false;
    return true;
}

void WorkshopManager::LogSummary() const
{
    Logger::Info("=== WorkshopManager Summary ===");
    Logger::Info("Manifest: %s", m_manifestPath.c_str());
    Logger::Info("App ID: %u  Download enabled: %s", m_appId, m_downloadEnabled ? "yes" : "no");
    size_t maps = 0, mods = 0, assets = 0, missing = 0;
    for (const auto& it : m_items) {
        switch (it.type) {
            case ItemType::Map:   ++maps;   break;
            case ItemType::Mod:   ++mods;   break;
            case ItemType::Asset: ++assets; break;
            default: break;
        }
        if (!it.present) ++missing;
    }
    Logger::Info("Items: %zu total (%zu maps, %zu mods, %zu assets), %zu missing",
                 m_items.size(), maps, mods, assets, missing);
    Logger::Info("===============================");
}
