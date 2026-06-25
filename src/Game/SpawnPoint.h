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
};
