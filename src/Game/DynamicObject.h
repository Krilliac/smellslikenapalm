// src/Game/DynamicObject.h – Header for DynamicObject

#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include "Math/Vector3.h"

struct DynamicObject {
    uint32_t id = 0;
    Vector3  position;
    Vector3  velocity;
    bool     stateChanged = false;

    void Update(double deltaTime) {
        Vector3 oldPos = position;
        position += velocity * static_cast<float>(deltaTime);
        stateChanged = (position != oldPos);
    }

    bool HasStateChanged() const {
        return stateChanged;
    }

    std::vector<uint8_t> Serialize() const {
        std::vector<uint8_t> data;
        data.resize(sizeof(id) + sizeof(position));
        size_t offset = 0;
        memcpy(data.data() + offset, &id, sizeof(id));
        offset += sizeof(id);
        memcpy(data.data() + offset, &position, sizeof(position));
        return data;
    }
};
