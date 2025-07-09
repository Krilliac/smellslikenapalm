// src/Physics/CollisionDetection.cpp
#include "Physics/CollisionDetection.h"
#include "Utils/Logger.h"
#include <cmath>
#include <unordered_set>

CollisionDetection::CollisionDetection(float minX,float minY,float minZ,
                                       float maxX,float maxY,float maxZ,
                                       float cellSize)
  : m_minX(minX),m_minY(minY),m_minZ(minZ),
    m_maxX(maxX),m_maxY(maxY),m_maxZ(maxZ),
    m_cellSize(cellSize)
{
    m_dimX = int((m_maxX-m_minX)/m_cellSize)+1;
    m_dimY = int((m_maxY-m_minY)/m_cellSize)+1;
    m_dimZ = int((m_maxZ-m_minZ)/m_cellSize)+1;
    m_grid.resize(m_dimX*m_dimY*m_dimZ);
    Logger::Info("CollisionDetection initialized grid %dx%dx%d", m_dimX,m_dimY,m_dimZ);
}

CollisionDetection::~CollisionDetection() = default;

int CollisionDetection::CellIndex(int x,int y,int z) const {
    return (z*m_dimY + y)*m_dimX + x;
}

void CollisionDetection::AddAABB(uint32_t id, const AABB& box) {
    m_aabbMap[id]=box;
    InsertAABB(id,box);
}

void CollisionDetection::UpdateAABB(uint32_t id,const AABB& box) {
    RemoveFromGrid(id);
    m_aabbMap[id]=box;
    InsertAABB(id,box);
}

void CollisionDetection::RemoveAABB(uint32_t id) {
    RemoveFromGrid(id);
    m_aabbMap.erase(id);
}

void CollisionDetection::AddSphere(uint32_t id,const Sphere& s) {
    m_sphereMap[id]=s;
    InsertSphere(id,s);
}

void CollisionDetection::UpdateSphere(uint32_t id,const Sphere& s) {
    RemoveFromGrid(id);
    m_sphereMap[id]=s;
    InsertSphere(id,s);
}

void CollisionDetection::RemoveSphere(uint32_t id) {
    RemoveFromGrid(id);
    m_sphereMap.erase(id);
}

void CollisionDetection::InsertAABB(uint32_t id,const AABB& b) {
    int minX=int((b.min.x-m_minX)/m_cellSize);
    int minY=int((b.min.y-m_minY)/m_cellSize);
    int minZ=int((b.min.z-m_minZ)/m_cellSize);
    int maxX=int((b.max.x-m_minX)/m_cellSize);
    int maxY=int((b.max.y-m_minY)/m_cellSize);
    int maxZ=int((b.max.z-m_minZ)/m_cellSize);
    for(int z=minZ;z<=maxZ;++z)for(int y=minY;y<=maxY;++y)for(int x=minX;x<=maxX;++x){
        if(x>=0&&x<m_dimX&&y>=0&&y<m_dimY&&z>=0&&z<m_dimZ){
            m_grid[CellIndex(x,y,z)].aabbs.push_back(id);
        }
    }
}

void CollisionDetection::InsertSphere(uint32_t id,const Sphere& s) {
    int cx=int((s.center.x-m_minX)/m_cellSize);
    int cy=int((s.center.y-m_minY)/m_cellSize);
    int cz=int((s.center.z-m_minZ)/m_cellSize);
    if(cx>=0&&cx<m_dimX&&cy>=0&&cy<m_dimY&&cz>=0&&cz<m_dimZ){
        m_grid[CellIndex(cx,cy,cz)].spheres.push_back(id);
    }
}

void CollisionDetection::RemoveFromGrid(uint32_t id) {
    for(auto& cell:m_grid){
        auto& v=cell.aabbs;
        v.erase(std::remove(v.begin(),v.end(),id),v.end());
        auto& s=cell.spheres;
        s.erase(std::remove(s.begin(),s.end(),id),s.end());
    }
}

bool CollisionDetection::TestAABBAABB(const AABB& a,const AABB& b) const {
    return (a.min.x<=b.max.x && a.max.x>=b.min.x) &&
           (a.min.y<=b.max.y && a.max.y>=b.min.y) &&
           (a.min.z<=b.max.z && a.max.z>=b.min.z);
}

bool CollisionDetection::TestSphereSphere(const Sphere& s1,const Sphere& s2) const {
    float r=s1.radius+s2.radius;
    return (s1.center-s2.center).LengthSquared() <= r*r;
}

bool CollisionDetection::TestAABBSphere(const AABB& b,const Sphere& s) const {
    float d=0;
    auto clamp=[](float v,float lo,float hi){return v<lo?lo:(v>hi?hi:v);};
    d += std::pow(s.center.x - clamp(s.center.x,b.min.x,b.max.x),2);
    d += std::pow(s.center.y - clamp(s.center.y,b.min.y,b.max.y),2);
    d += std::pow(s.center.z - clamp(s.center.z,b.min.z,b.max.z),2);
    return d <= s.radius*s.radius;
}

void CollisionDetection::DetectCollisions(CollisionCallback cb) {
    std::unordered_set<uint64_t> seen;
    for(auto& cell:m_grid){
        // AABB vs AABB
        for(size_t i=0;i<cell.aabbs.size();++i)for(size_t j=i+1;j<cell.aabbs.size();++j){
            uint32_t a=cell.aabbs[i], b=cell.aabbs[j];
            uint64_t key=((uint64_t)a<<32)|b;
            if(!seen.insert(key).second) continue;
            if(TestAABBAABB(m_aabbMap[a],m_aabbMap[b])) cb(a,b);
        }
        // Sphere vs Sphere
        for(size_t i=0;i<cell.spheres.size();++i)for(size_t j=i+1;j<cell.spheres.size();++j){
            uint32_t a=cell.spheres[i], b=cell.spheres[j];
            if(TestSphereSphere(m_sphereMap[a],m_sphereMap[b])) cb(a,b);
        }
        // AABB vs Sphere
        for(auto a:cell.aabbs)for(auto s:cell.spheres){
            if(TestAABBSphere(m_aabbMap[a],m_sphereMap[s])) cb(a,s);
        }
    }
}

uint32_t CollisionDetection::Raycast(const Vector3& o,const Vector3& d,float maxDist,Vector3& hp) {
    float best=std::numeric_limits<float>::infinity();
    uint32_t hitId=0;
    float t;
    for(auto& kv:m_aabbMap){
        if(RayIntersectsAABB(o,d,kv.second,t) && t<best && t<=maxDist){
            best=t; hitId=kv.first;
        }
    }
    for(auto& kv:m_sphereMap){
        if(RayIntersectsSphere(o,d,kv.second,t) && t<best && t<=maxDist){
            best=t; hitId=kv.first;
        }
    }
    if(hitId){
        hp = o + d * best;
    }
    return hitId;
}

// Ray–object tests omitted for brevity; implement slab method for AABB and geometric for sphere.