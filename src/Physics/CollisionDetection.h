// src/Physics/CollisionDetection.h
#pragma once

#include <vector>
#include <functional>
#include "Math/Vector3.h"

struct AABB {
    Vector3 min;
    Vector3 max;
};

struct Sphere {
    Vector3 center;
    float   radius;
};

struct CollisionPair {
    uint32_t idA;
    uint32_t idB;
};

class CollisionDetection {
public:
    using CollisionCallback = std::function<void(uint32_t a, uint32_t b)>;

    CollisionDetection(float worldMinX, float worldMinY, float worldMinZ,
                       float worldMaxX, float worldMaxY, float worldMaxZ,
                       float cellSize);
    ~CollisionDetection();

    // Register dynamic objects
    void AddAABB(uint32_t id, const AABB& box);
    void UpdateAABB(uint32_t id, const AABB& box);
    void RemoveAABB(uint32_t id);

    void AddSphere(uint32_t id, const Sphere& sphere);
    void UpdateSphere(uint32_t id, const Sphere& sphere);
    void RemoveSphere(uint32_t id);

    // Perform broadphase + narrowphase and invoke callback on each collision
    void DetectCollisions(CollisionCallback cb);

    // Raycast against all AABBs and spheres; returns first hit ID or 0
    uint32_t Raycast(const Vector3& origin,
                     const Vector3& direction,
                     float maxDistance,
                     Vector3& outHitPoint);

private:
    struct Cell {
        std::vector<uint32_t> aabbs;
        std::vector<uint32_t> spheres;
    };

    float m_minX, m_minY, m_minZ;
    float m_maxX, m_maxY, m_maxZ;
    float m_cellSize;
    int   m_dimX, m_dimY, m_dimZ;

    std::vector<Cell> m_grid;
    std::unordered_map<uint32_t, AABB>   m_aabbMap;
    std::unordered_map<uint32_t, Sphere> m_sphereMap;

    int  CellIndex(int x, int y, int z) const;
    void InsertAABB(uint32_t id, const AABB& b);
    void InsertSphere(uint32_t id, const Sphere& s);
    void RemoveFromGrid(uint32_t id);

    bool TestAABBAABB(const AABB& a, const AABB& b) const;
    bool TestSphereSphere(const Sphere& s1, const Sphere& s2) const;
    bool TestAABBSphere(const AABB& box, const Sphere& s) const;
    bool RayIntersectsAABB(const Vector3& o, const Vector3& d, const AABB& box, float& t) const;
    bool RayIntersectsSphere(const Vector3& o, const Vector3& d, const Sphere& s, float& t) const;
};