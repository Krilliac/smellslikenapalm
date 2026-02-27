// src/Game/EntityManager.h -- Stub EntityManager for scripting facade
#pragma once

#include <cstdint>
#include <string>
#include "Math/Vector3.h"

class EntityManager {
public:
    EntityManager() = default;
    ~EntityManager() = default;

    bool SpawnEntity(const std::string& /*className*/, const Vector3& /*pos*/, int* /*outId*/) { return false; }
    void RemoveEntity(int /*entityId*/) {}
    bool IsEntityValid(int /*entityId*/) { return false; }
    void SetEntityPosition(int /*entityId*/, const Vector3& /*pos*/) {}
    Vector3 GetEntityPosition(int /*entityId*/) { return {}; }
    float GetEntityHealth(int /*entityId*/) { return 0.0f; }
    void SetEntityHealth(int /*entityId*/, float /*health*/) {}
    std::string GetEntityClass(int /*entityId*/) { return ""; }
    int GetEntityCount() { return 0; }
    int GetEntityCountByClass(const std::string& /*className*/) { return 0; }
};
