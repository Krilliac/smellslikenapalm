// src/Game/SpawnPoint.h

#pragma once

#include <string>
#include <cstdint>
#include "Math/Vector3.h"

struct SpawnPoint {
    uint32_t    id = 0;
    std::string name;
    Vector3     position;
    Vector3     rotation;
    uint32_t    teamId = 0;
    bool        enabled = true;

    // Inclusive objective-phase bounds for Territory maps. A negative bound
    // is unbounded. Other game modes ignore these fields.
    int         minTerritoryPhase = -1;
    int         maxTerritoryPhase = -1;

    // Optional canonical PackageMap object reference for the cooked
    // ROVolumePlayerStartGroup represented by this location. Retail's spawn
    // selection scene consumes these actors through ROTeamInfo h59. Network
    // code rebases nonzero refs through the client's frozen artifact layout.
    // Zero means the fixture has not been mapped to a cooked client object.
    uint32_t    retailSpawnVolumeRef = 0;
};
