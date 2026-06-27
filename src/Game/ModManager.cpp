// src/Game/ModManager.cpp – Implementation of mod/asset registration

#include "Game/ModManager.h"
#include "Game/WorkshopManager.h"
#include "Config/ServerConfig.h"
#include "Utils/Logger.h"
#include "Utils/StringUtils.h"
#include <filesystem>

ModManager::ModManager(std::shared_ptr<ServerConfig> serverConfig)
    : m_serverConfig(std::move(serverConfig))
{
    m_modsDir = m_serverConfig ? m_serverConfig->GetModsDir() : "data/mods/";
    Logger::Info("ModManager initialized (modsDir='%s')", m_modsDir.c_str());
}

ModManager::~ModManager() = default;

bool ModManager::Initialize(WorkshopManager* workshop)
{
    m_mods.clear();
    AddFromWorkshop(workshop);
    ScanModsDirectory();

    size_t missing = 0;
    for (const auto& m : m_mods) if (!m.present) ++missing;
    Logger::Info("ModManager: %zu mods/assets registered (%zu missing on disk)",
                 m_mods.size(), missing);
    return true;
}

bool ModManager::HasMod(const std::string& fileName) const
{
    for (const auto& m : m_mods) {
        if (StringUtils::EqualsIgnoreCase(m.fileName, fileName)) return true;
    }
    return false;
}

void ModManager::AddFromWorkshop(WorkshopManager* workshop)
{
    if (!workshop) return;

    auto consume = [&](const std::vector<WorkshopManager::Item>& items, bool isAsset) {
        for (const auto& it : items) {
            if (HasMod(it.fileName)) continue;
            Mod m;
            std::filesystem::path p(it.fileName);
            m.name       = p.stem().string();
            m.fileName   = it.fileName;
            m.path       = it.resolvedPath;
            m.isAsset    = isAsset;
            m.present    = it.present;
            m.workshopId = it.workshopId;
            m_mods.push_back(m);
        }
    };

    consume(workshop->GetItemsByType(WorkshopManager::ItemType::Mod),   /*isAsset=*/false);
    consume(workshop->GetItemsByType(WorkshopManager::ItemType::Asset), /*isAsset=*/true);
}

void ModManager::ScanModsDirectory()
{
    namespace fs = std::filesystem;
    if (m_modsDir.empty() || !fs::exists(m_modsDir)) {
        Logger::Debug("ModManager: mods directory '%s' does not exist; skipping scan", m_modsDir.c_str());
        return;
    }

    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(m_modsDir, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto& path = it->path();
        if (StringUtils::ToLower(path.extension().string()) != ".pak") continue;

        std::string fileName = path.filename().string();
        if (HasMod(fileName)) continue; // already registered via Workshop

        Mod m;
        m.name     = path.stem().string();
        m.fileName = fileName;
        m.path     = path.string();
        m.isAsset  = false; // local .pak treated as gameplay mod by default
        m.present  = true;
        m.workshopId = 0;
        m_mods.push_back(m);
        Logger::Debug("ModManager: discovered local mod '%s' at %s", m.name.c_str(), m.path.c_str());
    }
    if (ec) {
        Logger::Warn("ModManager: error scanning mods directory '%s': %s",
                     m_modsDir.c_str(), ec.message().c_str());
    }
}

size_t ModManager::Validate()
{
    namespace fs = std::filesystem;
    size_t missing = 0;
    for (auto& m : m_mods) {
        m.present = !m.path.empty() && fs::exists(m.path) && fs::is_regular_file(m.path);
        if (!m.present) ++missing;
    }
    Logger::Info("ModManager: validated %zu mods, %zu missing", m_mods.size(), missing);
    return missing;
}

std::vector<ModManager::Mod> ModManager::GetGameplayMods() const
{
    std::vector<Mod> out;
    for (const auto& m : m_mods) if (!m.isAsset) out.push_back(m);
    return out;
}

std::vector<ModManager::Mod> ModManager::GetAssets() const
{
    std::vector<Mod> out;
    for (const auto& m : m_mods) if (m.isAsset) out.push_back(m);
    return out;
}

std::vector<std::string> ModManager::GetClientManifest() const
{
    std::vector<std::string> out;
    out.reserve(m_mods.size());
    for (const auto& m : m_mods) {
        out.push_back(m.name + ":" + m.fileName);
    }
    return out;
}

bool ModManager::AllPresent() const
{
    for (const auto& m : m_mods) if (!m.present) return false;
    return true;
}

void ModManager::LogSummary() const
{
    Logger::Info("=== ModManager Summary ===");
    size_t mods = 0, assets = 0, missing = 0;
    for (const auto& m : m_mods) {
        if (m.isAsset) ++assets; else ++mods;
        if (!m.present) ++missing;
    }
    Logger::Info("Mods: %zu  Assets: %zu  Missing: %zu", mods, assets, missing);
    Logger::Info("==========================");
}
