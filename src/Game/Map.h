// src/Game/Map.h – Header for Map

#pragma once

#include <string>
#include <vector>
#include "Math/Vector3.h"

struct MapSettings {
    std::string name;
    std::string displayName;
    std::string description;
    std::string environment;
    std::string timeOfDay;
    std::string weather;
    std::string sizeCategory;
    int         maxPlayers = 0;
    int         minPlayers = 0;
};

struct MapObject {
    uint32_t    id;
    Vector3     position;
    std::string type;
};

class Map {
public:
    Map();
    ~Map();

    bool LoadFromFile(const std::string& mapFilePath);
    bool SaveToFile(const std::string& mapFilePath) const;

    const MapSettings& GetSettings() const;
    const std::vector<Vector3>& GetNavMesh() const;
    const std::vector<MapObject>& GetStaticObjects() const;

    void AddStaticObject(const MapObject& obj);
    void RemoveStaticObject(uint32_t objectId);

    bool IsPointInBounds(const Vector3& point) const;

private:
    bool ParseSettings(std::istream& in);
    bool ParseNavMesh(std::istream& in);
    bool ParseStaticObjects(std::istream& in);
    void Clear();

    MapSettings                m_settings;
    std::vector<Vector3>       m_navMeshVertices;
    std::vector<MapObject>     m_staticObjects;
    Vector3                    m_boundsMin;
    Vector3                    m_boundsMax;
};