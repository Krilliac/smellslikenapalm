// src/Physics/CollisionDetection.cpp
#include "Physics/CollisionDetection.h"
#include "Utils/Logger.h"
#include <algorithm>
#include <cmath>
#include <unordered_set>

CollisionDetection::CollisionDetection(float minX,float minY,float minZ,
                                       float maxX,float maxY,float maxZ,
                                       float cellSize)
  : m_minX(minX),m_minY(minY),m_minZ(minZ),
    m_maxX(maxX),m_maxY(maxY),m_maxZ(maxZ),
    m_cellSize(cellSize)
{
    Logger::Trace("[CollisionDetection::CollisionDetection] Entry - minX=%.4f, minY=%.4f, minZ=%.4f, maxX=%.4f, maxY=%.4f, maxZ=%.4f, cellSize=%.4f",
                  minX, minY, minZ, maxX, maxY, maxZ, cellSize);
    m_dimX = int((m_maxX-m_minX)/m_cellSize)+1;
    m_dimY = int((m_maxY-m_minY)/m_cellSize)+1;
    m_dimZ = int((m_maxZ-m_minZ)/m_cellSize)+1;
    Logger::Debug("[CollisionDetection::CollisionDetection] Computed grid dimensions: dimX=%d, dimY=%d, dimZ=%d, total cells=%d",
                  m_dimX, m_dimY, m_dimZ, m_dimX*m_dimY*m_dimZ);
    m_grid.resize(m_dimX*m_dimY*m_dimZ);
    Logger::Debug("[CollisionDetection::CollisionDetection] Grid resized to %zu cells", m_grid.size());
    Logger::Info("CollisionDetection initialized grid %dx%dx%d", m_dimX,m_dimY,m_dimZ);
    Logger::Trace("[CollisionDetection::CollisionDetection] Exit");
}

CollisionDetection::~CollisionDetection() {
    Logger::Trace("[CollisionDetection::~CollisionDetection] Entry - grid size=%zu, AABB count=%zu, sphere count=%zu",
                  m_grid.size(), m_aabbMap.size(), m_sphereMap.size());
    Logger::Info("[CollisionDetection::~CollisionDetection] CollisionDetection destroyed: had %zu AABBs and %zu spheres",
                 m_aabbMap.size(), m_sphereMap.size());
    Logger::Trace("[CollisionDetection::~CollisionDetection] Exit");
}

int CollisionDetection::CellIndex(int x,int y,int z) const {
    Logger::Trace("[CollisionDetection::CellIndex] Entry - x=%d, y=%d, z=%d", x, y, z);
    int index = (z*m_dimY + y)*m_dimX + x;
    Logger::Trace("[CollisionDetection::CellIndex] Exit - returning index=%d (formula: (z=%d * dimY=%d + y=%d) * dimX=%d + x=%d)",
                  index, z, m_dimY, y, m_dimX, x);
    return index;
}

void CollisionDetection::AddAABB(uint32_t id, const AABB& box) {
    Logger::Trace("[CollisionDetection::AddAABB] Entry - id=%u, box.min=(%.4f, %.4f, %.4f), box.max=(%.4f, %.4f, %.4f)",
                  id, box.min.x, box.min.y, box.min.z, box.max.x, box.max.y, box.max.z);
    m_aabbMap[id]=box;
    Logger::Debug("[CollisionDetection::AddAABB] AABB id=%u stored in aabbMap, total AABBs=%zu", id, m_aabbMap.size());
    InsertAABB(id,box);
    Logger::Info("[CollisionDetection::AddAABB] Added AABB id=%u with min=(%.4f, %.4f, %.4f), max=(%.4f, %.4f, %.4f)",
                 id, box.min.x, box.min.y, box.min.z, box.max.x, box.max.y, box.max.z);
    Logger::Trace("[CollisionDetection::AddAABB] Exit");
}

void CollisionDetection::UpdateAABB(uint32_t id,const AABB& box) {
    Logger::Trace("[CollisionDetection::UpdateAABB] Entry - id=%u, new box.min=(%.4f, %.4f, %.4f), new box.max=(%.4f, %.4f, %.4f)",
                  id, box.min.x, box.min.y, box.min.z, box.max.x, box.max.y, box.max.z);
    Logger::Debug("[CollisionDetection::UpdateAABB] Removing AABB id=%u from grid before update", id);
    RemoveFromGrid(id);
    m_aabbMap[id]=box;
    Logger::Debug("[CollisionDetection::UpdateAABB] AABB id=%u map entry updated, reinserting into grid", id);
    InsertAABB(id,box);
    Logger::Info("[CollisionDetection::UpdateAABB] Updated AABB id=%u to min=(%.4f, %.4f, %.4f), max=(%.4f, %.4f, %.4f)",
                 id, box.min.x, box.min.y, box.min.z, box.max.x, box.max.y, box.max.z);
    Logger::Trace("[CollisionDetection::UpdateAABB] Exit");
}

void CollisionDetection::RemoveAABB(uint32_t id) {
    Logger::Trace("[CollisionDetection::RemoveAABB] Entry - id=%u", id);
    Logger::Debug("[CollisionDetection::RemoveAABB] Removing AABB id=%u from grid", id);
    RemoveFromGrid(id);
    m_aabbMap.erase(id);
    Logger::Debug("[CollisionDetection::RemoveAABB] AABB id=%u erased from aabbMap, remaining AABBs=%zu", id, m_aabbMap.size());
    Logger::Info("[CollisionDetection::RemoveAABB] Removed AABB id=%u, remaining AABBs=%zu", id, m_aabbMap.size());
    Logger::Trace("[CollisionDetection::RemoveAABB] Exit");
}

void CollisionDetection::AddSphere(uint32_t id,const Sphere& s) {
    Logger::Trace("[CollisionDetection::AddSphere] Entry - id=%u, center=(%.4f, %.4f, %.4f), radius=%.4f",
                  id, s.center.x, s.center.y, s.center.z, s.radius);
    m_sphereMap[id]=s;
    Logger::Debug("[CollisionDetection::AddSphere] Sphere id=%u stored in sphereMap, total spheres=%zu", id, m_sphereMap.size());
    InsertSphere(id,s);
    Logger::Info("[CollisionDetection::AddSphere] Added sphere id=%u at center=(%.4f, %.4f, %.4f) with radius=%.4f",
                 id, s.center.x, s.center.y, s.center.z, s.radius);
    Logger::Trace("[CollisionDetection::AddSphere] Exit");
}

void CollisionDetection::UpdateSphere(uint32_t id,const Sphere& s) {
    Logger::Trace("[CollisionDetection::UpdateSphere] Entry - id=%u, new center=(%.4f, %.4f, %.4f), new radius=%.4f",
                  id, s.center.x, s.center.y, s.center.z, s.radius);
    Logger::Debug("[CollisionDetection::UpdateSphere] Removing sphere id=%u from grid before update", id);
    RemoveFromGrid(id);
    m_sphereMap[id]=s;
    Logger::Debug("[CollisionDetection::UpdateSphere] Sphere id=%u map entry updated, reinserting into grid", id);
    InsertSphere(id,s);
    Logger::Info("[CollisionDetection::UpdateSphere] Updated sphere id=%u to center=(%.4f, %.4f, %.4f), radius=%.4f",
                 id, s.center.x, s.center.y, s.center.z, s.radius);
    Logger::Trace("[CollisionDetection::UpdateSphere] Exit");
}

void CollisionDetection::RemoveSphere(uint32_t id) {
    Logger::Trace("[CollisionDetection::RemoveSphere] Entry - id=%u", id);
    Logger::Debug("[CollisionDetection::RemoveSphere] Removing sphere id=%u from grid", id);
    RemoveFromGrid(id);
    m_sphereMap.erase(id);
    Logger::Debug("[CollisionDetection::RemoveSphere] Sphere id=%u erased from sphereMap, remaining spheres=%zu", id, m_sphereMap.size());
    Logger::Info("[CollisionDetection::RemoveSphere] Removed sphere id=%u, remaining spheres=%zu", id, m_sphereMap.size());
    Logger::Trace("[CollisionDetection::RemoveSphere] Exit");
}

void CollisionDetection::InsertAABB(uint32_t id,const AABB& b) {
    Logger::Trace("[CollisionDetection::InsertAABB] Entry - id=%u, min=(%.4f, %.4f, %.4f), max=(%.4f, %.4f, %.4f)",
                  id, b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z);
    int minX=int((b.min.x-m_minX)/m_cellSize);
    int minY=int((b.min.y-m_minY)/m_cellSize);
    int minZ=int((b.min.z-m_minZ)/m_cellSize);
    int maxX=int((b.max.x-m_minX)/m_cellSize);
    int maxY=int((b.max.y-m_minY)/m_cellSize);
    int maxZ=int((b.max.z-m_minZ)/m_cellSize);
    Logger::Debug("[CollisionDetection::InsertAABB] AABB id=%u grid cell range: x=[%d,%d], y=[%d,%d], z=[%d,%d]",
                  id, minX, maxX, minY, maxY, minZ, maxZ);
    int cellsInserted = 0;
    int cellsSkipped = 0;
    for(int z=minZ;z<=maxZ;++z)for(int y=minY;y<=maxY;++y)for(int x=minX;x<=maxX;++x){
        if(x>=0&&x<m_dimX&&y>=0&&y<m_dimY&&z>=0&&z<m_dimZ){
            m_grid[CellIndex(x,y,z)].aabbs.push_back(id);
            cellsInserted++;
        } else {
            cellsSkipped++;
            Logger::Debug("[CollisionDetection::InsertAABB] AABB id=%u: cell (%d,%d,%d) is out of grid bounds, skipped", id, x, y, z);
        }
    }
    Logger::Debug("[CollisionDetection::InsertAABB] AABB id=%u inserted into %d cells, %d cells were out of bounds",
                  id, cellsInserted, cellsSkipped);
    Logger::Trace("[CollisionDetection::InsertAABB] Exit");
}

void CollisionDetection::InsertSphere(uint32_t id,const Sphere& s) {
    Logger::Trace("[CollisionDetection::InsertSphere] Entry - id=%u, center=(%.4f, %.4f, %.4f), radius=%.4f",
                  id, s.center.x, s.center.y, s.center.z, s.radius);
    int cx=int((s.center.x-m_minX)/m_cellSize);
    int cy=int((s.center.y-m_minY)/m_cellSize);
    int cz=int((s.center.z-m_minZ)/m_cellSize);
    Logger::Debug("[CollisionDetection::InsertSphere] Sphere id=%u computed cell coords: cx=%d, cy=%d, cz=%d", id, cx, cy, cz);
    if(cx>=0&&cx<m_dimX&&cy>=0&&cy<m_dimY&&cz>=0&&cz<m_dimZ){
        m_grid[CellIndex(cx,cy,cz)].spheres.push_back(id);
        Logger::Debug("[CollisionDetection::InsertSphere] Sphere id=%u inserted into grid cell (%d,%d,%d) at index %d",
                      id, cx, cy, cz, CellIndex(cx,cy,cz));
    } else {
        Logger::Warn("[CollisionDetection::InsertSphere] Sphere id=%u at cell (%d,%d,%d) is OUT OF grid bounds (dims=%d,%d,%d), not inserted",
                     id, cx, cy, cz, m_dimX, m_dimY, m_dimZ);
    }
    Logger::Trace("[CollisionDetection::InsertSphere] Exit");
}

void CollisionDetection::RemoveFromGrid(uint32_t id) {
    Logger::Trace("[CollisionDetection::RemoveFromGrid] Entry - id=%u, grid size=%zu", id, m_grid.size());
    int cellsWithAABBRemoved = 0;
    int cellsWithSphereRemoved = 0;
    for(auto& cell:m_grid){
        auto& v=cell.aabbs;
        size_t oldAABBSize = v.size();
        v.erase(std::remove(v.begin(),v.end(),id),v.end());
        if (v.size() < oldAABBSize) {
            cellsWithAABBRemoved++;
        }
        auto& s=cell.spheres;
        size_t oldSphereSize = s.size();
        s.erase(std::remove(s.begin(),s.end(),id),s.end());
        if (s.size() < oldSphereSize) {
            cellsWithSphereRemoved++;
        }
    }
    Logger::Debug("[CollisionDetection::RemoveFromGrid] id=%u removed from %d AABB cell entries and %d sphere cell entries",
                  id, cellsWithAABBRemoved, cellsWithSphereRemoved);
    Logger::Trace("[CollisionDetection::RemoveFromGrid] Exit");
}

bool CollisionDetection::TestAABBAABB(const AABB& a,const AABB& b) const {
    Logger::Trace("[CollisionDetection::TestAABBAABB] Entry - a.min=(%.4f, %.4f, %.4f), a.max=(%.4f, %.4f, %.4f), b.min=(%.4f, %.4f, %.4f), b.max=(%.4f, %.4f, %.4f)",
                  a.min.x, a.min.y, a.min.z, a.max.x, a.max.y, a.max.z,
                  b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z);
    bool overlapX = (a.min.x<=b.max.x && a.max.x>=b.min.x);
    bool overlapY = (a.min.y<=b.max.y && a.max.y>=b.min.y);
    bool overlapZ = (a.min.z<=b.max.z && a.max.z>=b.min.z);
    bool result = overlapX && overlapY && overlapZ;
    Logger::Debug("[CollisionDetection::TestAABBAABB] Overlap test: X=%s, Y=%s, Z=%s => result=%s",
                  overlapX ? "true" : "false", overlapY ? "true" : "false", overlapZ ? "true" : "false",
                  result ? "COLLISION" : "no collision");
    Logger::Trace("[CollisionDetection::TestAABBAABB] Exit - returning %s", result ? "true" : "false");
    return result;
}

bool CollisionDetection::TestSphereSphere(const Sphere& s1,const Sphere& s2) const {
    Logger::Trace("[CollisionDetection::TestSphereSphere] Entry - s1.center=(%.4f, %.4f, %.4f), s1.radius=%.4f, s2.center=(%.4f, %.4f, %.4f), s2.radius=%.4f",
                  s1.center.x, s1.center.y, s1.center.z, s1.radius,
                  s2.center.x, s2.center.y, s2.center.z, s2.radius);
    float r=s1.radius+s2.radius;
    float dist2 = (s1.center-s2.center).LengthSquared();
    bool result = dist2 <= r*r;
    Logger::Debug("[CollisionDetection::TestSphereSphere] combinedRadius=%.4f, dist2=%.4f, threshold(r2)=%.4f => %s",
                  r, dist2, r*r, result ? "COLLISION" : "no collision");
    Logger::Trace("[CollisionDetection::TestSphereSphere] Exit - returning %s", result ? "true" : "false");
    return result;
}

bool CollisionDetection::TestAABBSphere(const AABB& b,const Sphere& s) const {
    Logger::Trace("[CollisionDetection::TestAABBSphere] Entry - box.min=(%.4f, %.4f, %.4f), box.max=(%.4f, %.4f, %.4f), sphere.center=(%.4f, %.4f, %.4f), sphere.radius=%.4f",
                  b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z,
                  s.center.x, s.center.y, s.center.z, s.radius);
    float d=0;
    auto clamp=[](float v,float lo,float hi){return v<lo?lo:(v>hi?hi:v);};
    float dx = s.center.x - clamp(s.center.x,b.min.x,b.max.x);
    float dy = s.center.y - clamp(s.center.y,b.min.y,b.max.y);
    float dz = s.center.z - clamp(s.center.z,b.min.z,b.max.z);
    d = dx*dx + dy*dy + dz*dz;
    Logger::Debug("[CollisionDetection::TestAABBSphere] Closest point deltas: dx=%.4f, dy=%.4f, dz=%.4f, d(dist2)=%.4f, radius2=%.4f",
                  dx, dy, dz, d, s.radius*s.radius);
    bool result = d <= s.radius*s.radius;
    Logger::Debug("[CollisionDetection::TestAABBSphere] Result: %s (d=%.4f %s radius2=%.4f)",
                  result ? "COLLISION" : "no collision", d, result ? "<=" : ">", s.radius*s.radius);
    Logger::Trace("[CollisionDetection::TestAABBSphere] Exit - returning %s", result ? "true" : "false");
    return result;
}

void CollisionDetection::DetectCollisions(CollisionCallback cb) {
    Logger::Trace("[CollisionDetection::DetectCollisions] Entry - grid size=%zu, AABBs=%zu, spheres=%zu",
                  m_grid.size(), m_aabbMap.size(), m_sphereMap.size());
    std::unordered_set<uint64_t> seen;
    int aabbCollisions = 0;
    int sphereCollisions = 0;
    int aabbSphereCollisions = 0;
    int totalPairsChecked = 0;
    int cellsProcessed = 0;
    for(auto& cell:m_grid){
        cellsProcessed++;
        // AABB vs AABB
        for(size_t i=0;i<cell.aabbs.size();++i)for(size_t j=i+1;j<cell.aabbs.size();++j){
            uint32_t a=cell.aabbs[i], b=cell.aabbs[j];
            uint64_t key=((uint64_t)a<<32)|b;
            if(!seen.insert(key).second) {
                Logger::Debug("[CollisionDetection::DetectCollisions] AABB pair (%u, %u) already tested, skipping", a, b);
                continue;
            }
            totalPairsChecked++;
            if(TestAABBAABB(m_aabbMap[a],m_aabbMap[b])) {
                aabbCollisions++;
                Logger::Info("[CollisionDetection::DetectCollisions] AABB-AABB collision detected: id=%u vs id=%u", a, b);
                cb(a,b);
            }
        }
        // Sphere vs Sphere
        for(size_t i=0;i<cell.spheres.size();++i)for(size_t j=i+1;j<cell.spheres.size();++j){
            uint32_t a=cell.spheres[i], b=cell.spheres[j];
            totalPairsChecked++;
            if(TestSphereSphere(m_sphereMap[a],m_sphereMap[b])) {
                sphereCollisions++;
                Logger::Info("[CollisionDetection::DetectCollisions] Sphere-Sphere collision detected: id=%u vs id=%u", a, b);
                cb(a,b);
            }
        }
        // AABB vs Sphere
        for(auto a:cell.aabbs)for(auto s:cell.spheres){
            totalPairsChecked++;
            if(TestAABBSphere(m_aabbMap[a],m_sphereMap[s])) {
                aabbSphereCollisions++;
                Logger::Info("[CollisionDetection::DetectCollisions] AABB-Sphere collision detected: AABB id=%u vs Sphere id=%u", a, s);
                cb(a,s);
            }
        }
    }
    Logger::Debug("[CollisionDetection::DetectCollisions] Detection complete: cells processed=%d, pairs checked=%d, AABB-AABB collisions=%d, Sphere-Sphere collisions=%d, AABB-Sphere collisions=%d",
                  cellsProcessed, totalPairsChecked, aabbCollisions, sphereCollisions, aabbSphereCollisions);
    Logger::Info("[CollisionDetection::DetectCollisions] Total collisions found: %d (AABB-AABB=%d, Sphere-Sphere=%d, AABB-Sphere=%d)",
                 aabbCollisions + sphereCollisions + aabbSphereCollisions, aabbCollisions, sphereCollisions, aabbSphereCollisions);
    Logger::Trace("[CollisionDetection::DetectCollisions] Exit");
}

uint32_t CollisionDetection::Raycast(const Vector3& o,const Vector3& d,float maxDist,Vector3& hp) {
    Logger::Trace("[CollisionDetection::Raycast] Entry - origin=(%.4f, %.4f, %.4f), direction=(%.4f, %.4f, %.4f), maxDist=%.4f",
                  o.x, o.y, o.z, d.x, d.y, d.z, maxDist);
    float best=std::numeric_limits<float>::infinity();
    uint32_t hitId=0;
    float t;
    Logger::Debug("[CollisionDetection::Raycast] Testing against %zu AABBs", m_aabbMap.size());
    int aabbsTested = 0;
    for(auto& kv:m_aabbMap){
        aabbsTested++;
        if(RayIntersectsAABB(o,d,kv.second,t) && t<best && t<=maxDist){
            Logger::Debug("[CollisionDetection::Raycast] AABB id=%u hit at t=%.4f (previous best=%.4f), updating best hit",
                          kv.first, t, best);
            best=t; hitId=kv.first;
        }
    }
    Logger::Debug("[CollisionDetection::Raycast] Tested %d AABBs, best hit so far: id=%u at t=%.4f", aabbsTested, hitId, best);
    Logger::Debug("[CollisionDetection::Raycast] Testing against %zu spheres", m_sphereMap.size());
    int spheresTested = 0;
    for(auto& kv:m_sphereMap){
        spheresTested++;
        if(RayIntersectsSphere(o,d,kv.second,t) && t<best && t<=maxDist){
            Logger::Debug("[CollisionDetection::Raycast] Sphere id=%u hit at t=%.4f (previous best=%.4f), updating best hit",
                          kv.first, t, best);
            best=t; hitId=kv.first;
        }
    }
    Logger::Debug("[CollisionDetection::Raycast] Tested %d spheres, final best hit: id=%u at t=%.4f", spheresTested, hitId, best);
    if(hitId){
        hp = o + d * best;
        Logger::Info("[CollisionDetection::Raycast] Raycast HIT: id=%u at t=%.4f, hitPoint=(%.4f, %.4f, %.4f)",
                     hitId, best, hp.x, hp.y, hp.z);
    } else {
        Logger::Debug("[CollisionDetection::Raycast] Raycast MISS: no intersections found within maxDist=%.4f", maxDist);
    }
    Logger::Trace("[CollisionDetection::Raycast] Exit - returning hitId=%u", hitId);
    return hitId;
}

bool CollisionDetection::RayIntersectsAABB(const Vector3& o, const Vector3& d, const AABB& box, float& t) const {
    Logger::Trace("[CollisionDetection::RayIntersectsAABB] Entry - origin=(%.4f, %.4f, %.4f), dir=(%.4f, %.4f, %.4f), box.min=(%.4f, %.4f, %.4f), box.max=(%.4f, %.4f, %.4f)",
                  o.x, o.y, o.z, d.x, d.y, d.z, box.min.x, box.min.y, box.min.z, box.max.x, box.max.y, box.max.z);
    float tmin = -std::numeric_limits<float>::infinity();
    float tmax = std::numeric_limits<float>::infinity();

    auto slab = [&](float origin, float dir, float bmin, float bmax) -> bool {
        if (std::fabs(dir) < 1e-8f) {
            bool inside = (origin >= bmin && origin <= bmax);
            Logger::Debug("[CollisionDetection::RayIntersectsAABB] Slab test: dir~=0, origin=%.4f in [%.4f, %.4f] => %s",
                          origin, bmin, bmax, inside ? "inside" : "outside");
            return inside;
        }
        float t1 = (bmin - origin) / dir;
        float t2 = (bmax - origin) / dir;
        if (t1 > t2) std::swap(t1, t2);
        if (t1 > tmin) tmin = t1;
        if (t2 < tmax) tmax = t2;
        Logger::Debug("[CollisionDetection::RayIntersectsAABB] Slab test: origin=%.4f, dir=%.4f, [%.4f,%.4f] => t1=%.4f, t2=%.4f, tmin=%.4f, tmax=%.4f, valid=%s",
                      origin, dir, bmin, bmax, t1, t2, tmin, tmax, (tmin <= tmax) ? "true" : "false");
        return tmin <= tmax;
    };

    if (!slab(o.x, d.x, box.min.x, box.max.x)) {
        Logger::Debug("[CollisionDetection::RayIntersectsAABB] X slab test failed");
        Logger::Trace("[CollisionDetection::RayIntersectsAABB] Exit - returning false (X slab miss)");
        return false;
    }
    if (!slab(o.y, d.y, box.min.y, box.max.y)) {
        Logger::Debug("[CollisionDetection::RayIntersectsAABB] Y slab test failed");
        Logger::Trace("[CollisionDetection::RayIntersectsAABB] Exit - returning false (Y slab miss)");
        return false;
    }
    if (!slab(o.z, d.z, box.min.z, box.max.z)) {
        Logger::Debug("[CollisionDetection::RayIntersectsAABB] Z slab test failed");
        Logger::Trace("[CollisionDetection::RayIntersectsAABB] Exit - returning false (Z slab miss)");
        return false;
    }

    t = tmin >= 0 ? tmin : tmax;
    bool result = t >= 0;
    Logger::Debug("[CollisionDetection::RayIntersectsAABB] All slab tests passed: tmin=%.4f, tmax=%.4f, chosen t=%.4f, result=%s",
                  tmin, tmax, t, result ? "HIT" : "miss (t<0)");
    Logger::Trace("[CollisionDetection::RayIntersectsAABB] Exit - returning %s, t=%.4f", result ? "true" : "false", t);
    return result;
}

bool CollisionDetection::RayIntersectsSphere(const Vector3& o, const Vector3& d, const Sphere& s, float& t) const {
    Logger::Trace("[CollisionDetection::RayIntersectsSphere] Entry - origin=(%.4f, %.4f, %.4f), dir=(%.4f, %.4f, %.4f), sphere.center=(%.4f, %.4f, %.4f), sphere.radius=%.4f",
                  o.x, o.y, o.z, d.x, d.y, d.z, s.center.x, s.center.y, s.center.z, s.radius);
    Vector3 oc = o - s.center;
    float a = d.Dot(d);
    float b = 2.0f * oc.Dot(d);
    float c = oc.Dot(oc) - s.radius * s.radius;
    float disc = b * b - 4.0f * a * c;
    Logger::Debug("[CollisionDetection::RayIntersectsSphere] oc=(%.4f, %.4f, %.4f), a=%.4f, b=%.4f, c=%.4f, discriminant=%.4f",
                  oc.x, oc.y, oc.z, a, b, c, disc);
    if (disc < 0) {
        Logger::Debug("[CollisionDetection::RayIntersectsSphere] Discriminant %.4f < 0, no intersection", disc);
        Logger::Trace("[CollisionDetection::RayIntersectsSphere] Exit - returning false (negative discriminant)");
        return false;
    }
    float sqrtDisc = std::sqrt(disc);
    float t0 = (-b - sqrtDisc) / (2.0f * a);
    float t1 = (-b + sqrtDisc) / (2.0f * a);
    Logger::Debug("[CollisionDetection::RayIntersectsSphere] sqrtDisc=%.4f, t0=%.4f, t1=%.4f", sqrtDisc, t0, t1);
    if (t0 >= 0) {
        t = t0;
        Logger::Debug("[CollisionDetection::RayIntersectsSphere] Using near intersection t0=%.4f", t0);
        Logger::Trace("[CollisionDetection::RayIntersectsSphere] Exit - returning true, t=%.4f", t);
        return true;
    }
    if (t1 >= 0) {
        t = t1;
        Logger::Debug("[CollisionDetection::RayIntersectsSphere] t0<0, using far intersection t1=%.4f (ray origin inside sphere)", t1);
        Logger::Trace("[CollisionDetection::RayIntersectsSphere] Exit - returning true, t=%.4f", t);
        return true;
    }
    Logger::Debug("[CollisionDetection::RayIntersectsSphere] Both t0=%.4f and t1=%.4f are negative, sphere is behind ray", t0, t1);
    Logger::Trace("[CollisionDetection::RayIntersectsSphere] Exit - returning false (both intersections behind ray)");
    return false;
}
