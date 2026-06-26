// src/Game/Bounds.h

#pragma once

#include "Math/Vector3.h"

struct Bounds {
    Vector3 min;
    Vector3 max;

    bool Contains(const Vector3& point) const {
        return point.x >= min.x && point.x <= max.x &&
               point.y >= min.y && point.y <= max.y &&
               point.z >= min.z && point.z <= max.z;
    }

    Vector3 Center() const {
        return Vector3((min.x + max.x) * 0.5f,
                       (min.y + max.y) * 0.5f,
                       (min.z + max.z) * 0.5f);
    }

    Vector3 Size() const {
        return Vector3(max.x - min.x,
                       max.y - min.y,
                       max.z - min.z);
    }
};
