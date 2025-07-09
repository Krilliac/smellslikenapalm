// src/Game/Map.cpp – Implementation for Map

#include "Game/Map.h"
#include "Utils/Logger.h"
#include "Utils/FileUtils.h"
#include "Utils/StringUtils.h"
#include <fstream>
#include <sstream>
#include <filesystem>

Map::Map() {
    Logger::Info("Map constructed");
}

Map::~Map() {
    Clear();
}

bool Map::LoadFromFile(const std::string& mapFilePath) {
    Logger::Info("Loading Map from %s", mapFilePath.c_str());
    if (!std::filesystem::exists(mapFilePath)) {
        Logger::Error("Map file not found: %s", mapFilePath.c_str());
        return false;
    }

    std::ifstream file(mapFilePath);
    if (!file.is_open()) {
        Logger::Error("Failed to open map file: %s", mapFilePath.c_str());
        return false;
    }

    Clear();
    std::string line;
    enum class Section { None, Settings, NavMesh, StaticObjects } section = Section::None;

    while (std::getline(file, line)) {
        line = StringUtils::Trim(line);
        if (line.empty() || line[0] == '#') continue;

        if (line == "[Settings]") {
            section = Section::Settings;
            continue;
        } else if (line == "[NavMesh]") {
            section = Section::NavMesh;
            continue;
        } else if (line == "[StaticObjects]") {
            section = Section::StaticObjects;
            continue;
        }

        switch (section) {
            case Section::Settings:
                if (!ParseSettings(file)) {
                    Logger::Error("Failed to parse Settings section");
                    return false;
                }
                // after parsing, break out to while
                section = Section::None;
                break;

            case Section::NavMesh:
                if (!ParseNavMesh(file)) {
                    Logger::Error("Failed to parse NavMesh section");
                    return false;
                }
                section = Section::None;
                break;

            case Section::StaticObjects:
                if (!ParseStaticObjects(file)) {
                    Logger::Error("Failed to parse StaticObjects section");
                    return false;
                }
                section = Section::None;
                break;

            default:
                break;
        }
    }

    file.close();
    Logger::Info("Map loaded: %s (%zu nav vertices, %zu static objects)",
                 m_settings.name.c_str(),
                 m_navMeshVertices.size(),
                 m_staticObjects.size());
    return true;
}

bool Map::SaveToFile(const std::string& mapFilePath) const {
    Logger::Info("Saving Map to %s", mapFilePath.c_str());
    std::filesystem::path path(mapFilePath);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }

    std::ofstream file(mapFilePath, std::ios::trunc);
    if (!file.is_open()) {
        Logger::Error("Failed to open map file for writing: %s", mapFilePath.c_str());
        return false;
    }

    // Settings
    file << "[Settings]\n";
    file << "name="        << m_settings.name        << "\n";
    file << "displayName=" << m_settings.displayName << "\n";
    file << "description=" << m_settings.description << "\n";
    file << "environment=" << m_settings.environment << "\n";
    file << "timeOfDay="   << m_settings.timeOfDay   << "\n";
    file << "weather="     << m_settings.weather     << "\n";
    file << "sizeCategory="<< m_settings.sizeCategory<< "\n";
    file << "maxPlayers="  << m_settings.maxPlayers  << "\n";
    file << "minPlayers="  << m_settings.minPlayers  << "\n\n";

    // NavMesh
    file << "[NavMesh]\n";
    for (const auto& v : m_navMeshVertices) {
        file << v.x << "," << v.y << "," << v.z << "\n";
    }
    file << "\n";

    // Static Objects
    file << "[StaticObjects]\n";
    for (const auto& obj : m_staticObjects) {
        file << obj.id << ";"
             << obj.position.x << "," << obj.position.y << "," << obj.position.z << ";"
             << obj.type << "\n";
    }

    file.close();
    Logger::Info("Map saved successfully");
    return true;
}

const MapSettings& Map::GetSettings() const {
    return m_settings;
}

const std::vector<Vector3>& Map::GetNavMesh() const {
    return m_navMeshVertices;
}

const std::vector<MapObject>& Map::GetStaticObjects() const {
    return m_staticObjects;
}

void Map::AddStaticObject(const MapObject& obj) {
    m_staticObjects.push_back(obj);
    Logger::Debug("Added static object ID %u", obj.id);
}

void Map::RemoveStaticObject(uint32_t objectId) {
    m_staticObjects.erase(
        std::remove_if(m_staticObjects.begin(), m_staticObjects.end(),
            [&](const MapObject& o){ return o.id == objectId; }),
        m_staticObjects.end());
    Logger::Debug("Removed static object ID %u", objectId);
}

bool Map::IsPointInBounds(const Vector3& point) const {
    return point.x >= m_boundsMin.x && point.x <= m_boundsMax.x &&
           point.y >= m_boundsMin.y && point.y <= m_boundsMax.y &&
           point.z >= m_boundsMin.z && point.z <= m_boundsMax.z;
}

bool Map::ParseSettings(std::istream& in) {
    std::string line;
    while (std::getline(in, line)) {
        line = StringUtils::Trim(line);
        if (line.empty()) break;

        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        std::string key = StringUtils::Trim(line.substr(0, pos));
        std::string val = StringUtils::Trim(line.substr(pos+1));

        if (key == "name") m_settings.name = val;
        else if (key == "displayName") m_settings.displayName = val;
        else if (key == "description") m_settings.description = val;
        else if (key == "environment") m_settings.environment = val;
        else if (key == "timeOfDay") m_settings.timeOfDay = val;
        else if (key == "weather") m_settings.weather = val;
        else if (key == "sizeCategory") m_settings.sizeCategory = val;
        else if (key == "maxPlayers") m_settings.maxPlayers = std::stoi(val);
        else if (key == "minPlayers") m_settings.minPlayers = std::stoi(val);
    }

    // Derive bounds defaults (placeholder)
    m_boundsMin = Vector3{-1000,-1000,-100};
    m_boundsMax = Vector3{1000,1000,500};
    return true;
}

bool Map::ParseNavMesh(std::istream& in) {
    std::string line;
    while (std::getline(in, line)) {
        line = StringUtils::Trim(line);
        if (line.empty()) break;

        auto parts = StringUtils::Split(line, ',');
        if (parts.size() != 3) continue;

        Vector3 v;
        v.x = std::stof(parts[0]);
        v.y = std::stof(parts[1]);
        v.z = std::stof(parts[2]);
        m_navMeshVertices.push_back(v);
    }
    return true;
}

bool Map::ParseStaticObjects(std::istream& in) {
    std::string line;
    while (std::getline(in, line)) {
        line = StringUtils::Trim(line);
        if (line.empty()) break;

        auto segments = StringUtils::Split(line, ';');
        if (segments.size() != 3) continue;

        MapObject obj;
        obj.id = std::stoul(segments[0]);

        auto coords = StringUtils::Split(segments[1], ',');
        if (coords.size() == 3) {
            obj.position.x = std::stof(coords[0]);
            obj.position.y = std::stof(coords[1]);
            obj.position.z = std::stof(coords[2]);
        }

        obj.type = segments[2];
        m_staticObjects.push_back(obj);
    }
    return true;
}

void Map::Clear() {
    m_navMeshVertices.clear();
    m_staticObjects.clear();
    m_settings = MapSettings();
    m_boundsMin = Vector3();
    m_boundsMax = Vector3();
    Logger::Debug("Map data cleared");
}