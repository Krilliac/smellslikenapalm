#pragma once

#include "Game/Bounds.h"
#include "Math/Vector3.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

struct BotNavigationConfig {
    bool enforceBounds = false;
    Bounds worldBounds{};
    float maxEdgeLength = 10000.0f;
    // Maximum distance from an arbitrary start/goal to its nearest authored
    // graph node.  This is deliberately independent of maxEdgeLength: a map
    // may contain long, explicitly reviewed transit links without allowing a
    // bot to snap onto that graph through an equally long un-authored chord.
    float maxEndpointSnapDistance = 10000.0f;
    float maxDirectRouteLength = 10000.0f;
};

struct BotWaypointNode {
    std::uint32_t id = 0;
    Vector3 position;
};

struct BotWaypointEdge {
    std::uint32_t fromId = 0;
    std::uint32_t toId = 0;
    bool bidirectional = false;
};

// Complete authored graph payload. Installing one is all-or-nothing so a bad
// map sidecar can never leave a partially connected graph active.
struct BotNavigationGraph {
    std::vector<BotWaypointNode> nodes;
    std::vector<BotWaypointEdge> edges;

    bool IsEmpty() const { return nodes.empty() && edges.empty(); }
};

struct BotRoute {
    std::vector<Vector3> waypoints;
    bool usedAuthoredGraph = false;
    bool usedDirectFallback = false;

    bool IsValid() const { return !waypoints.empty(); }
};

// Deterministic, headless navigation. Authored waypoint edges are directed;
// callers can opt into a bounded straight-line fallback when no path exists.
class BotNavigation {
public:
    explicit BotNavigation(const BotNavigationConfig& config = BotNavigationConfig{});

    bool Configure(const BotNavigationConfig& config);
    const BotNavigationConfig& GetConfig() const { return config_; }

    bool AddWaypoint(std::uint32_t id, const Vector3& position);
    bool RemoveWaypoint(std::uint32_t id);
    bool ConnectWaypoints(std::uint32_t fromId, std::uint32_t toId,
                          bool bidirectional = false);
    bool DisconnectWaypoints(std::uint32_t fromId, std::uint32_t toId,
                             bool bidirectional = false);
    bool ReplaceGraph(const BotNavigationGraph& graph);
    void Clear();

    bool HasWaypoint(std::uint32_t id) const;
    std::size_t GetWaypointCount() const { return nodes_.size(); }

    BotRoute BuildRoute(const Vector3& start, const Vector3& goal,
                        bool allowDirectFallback = true) const;

    // Moves along route by at most maxDistance and advances cursor as waypoint
    // targets are consumed. Invalid input leaves the position unchanged.
    Vector3 AdvanceAlongRoute(const Vector3& current, const BotRoute& route,
                              std::size_t& cursor, float maxDistance) const;

    bool IsPositionAllowed(const Vector3& position) const;

private:
    BotRoute BuildAuthoredRoute(const Vector3& start, const Vector3& goal) const;
    void PruneInvalidGraph();

    static bool IsFinite(const Vector3& position);
    static bool IsValidBounds(const Bounds& bounds);

    BotNavigationConfig config_;
    std::map<std::uint32_t, BotWaypointNode> nodes_;
    std::map<std::uint32_t, std::set<std::uint32_t>> edges_;
};
