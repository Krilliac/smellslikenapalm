#include "Game/BotNavigation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <utility>

namespace {

constexpr float kDistanceEpsilon = 0.0001f;

struct NavigationQueueEntry {
    float distance = 0.0f;
    std::uint32_t nodeId = 0;
};

struct NavigationQueueGreater {
    bool operator()(const NavigationQueueEntry& left,
                    const NavigationQueueEntry& right) const {
        if (left.distance != right.distance) {
            return left.distance > right.distance;
        }
        return left.nodeId > right.nodeId;
    }
};

} // namespace

BotNavigation::BotNavigation(const BotNavigationConfig& config) {
    Configure(config);
}

bool BotNavigation::Configure(const BotNavigationConfig& config) {
    if (!std::isfinite(config.maxEdgeLength) || config.maxEdgeLength <= 0.0f ||
        !std::isfinite(config.maxEndpointSnapDistance) ||
        config.maxEndpointSnapDistance <= 0.0f ||
        !std::isfinite(config.maxDirectRouteLength) ||
        config.maxDirectRouteLength < 0.0f ||
        (config.enforceBounds && !IsValidBounds(config.worldBounds))) {
        return false;
    }

    config_ = config;
    PruneInvalidGraph();
    return true;
}

bool BotNavigation::AddWaypoint(std::uint32_t id, const Vector3& position) {
    if (id == 0 || !IsPositionAllowed(position)) {
        return false;
    }

    nodes_[id] = BotWaypointNode{id, position};
    PruneInvalidGraph();
    return true;
}

bool BotNavigation::RemoveWaypoint(std::uint32_t id) {
    if (nodes_.erase(id) == 0) {
        return false;
    }

    edges_.erase(id);
    for (auto& edgeList : edges_) {
        edgeList.second.erase(id);
    }
    return true;
}

bool BotNavigation::ConnectWaypoints(std::uint32_t fromId, std::uint32_t toId,
                                     bool bidirectional) {
    const auto fromIt = nodes_.find(fromId);
    const auto toIt = nodes_.find(toId);
    if (fromId == toId || fromIt == nodes_.end() || toIt == nodes_.end()) {
        return false;
    }

    const float distance = fromIt->second.position.Distance(toIt->second.position);
    if (!std::isfinite(distance) || distance <= kDistanceEpsilon ||
        distance > config_.maxEdgeLength) {
        return false;
    }

    edges_[fromId].insert(toId);
    if (bidirectional) {
        edges_[toId].insert(fromId);
    }
    return true;
}

bool BotNavigation::DisconnectWaypoints(std::uint32_t fromId, std::uint32_t toId,
                                        bool bidirectional) {
    bool removed = false;
    const auto fromIt = edges_.find(fromId);
    if (fromIt != edges_.end()) {
        removed = fromIt->second.erase(toId) != 0 || removed;
    }

    if (bidirectional) {
        const auto toIt = edges_.find(toId);
        if (toIt != edges_.end()) {
            removed = toIt->second.erase(fromId) != 0 || removed;
        }
    }
    return removed;
}

bool BotNavigation::ReplaceGraph(const BotNavigationGraph& graph) {
    // Validate into a separate instance so duplicate ids, missing endpoints,
    // invalid positions, and overlong edges cannot partially mutate the live
    // graph. An empty payload intentionally clears the current graph.
    BotNavigation candidate(config_);
    for (const BotWaypointNode& node : graph.nodes) {
        if (candidate.HasWaypoint(node.id) ||
            !candidate.AddWaypoint(node.id, node.position)) {
            return false;
        }
    }

    std::set<std::pair<std::uint32_t, std::uint32_t>> directedEdges;
    for (const BotWaypointEdge& edge : graph.edges) {
        const auto forward = std::make_pair(edge.fromId, edge.toId);
        const auto reverse = std::make_pair(edge.toId, edge.fromId);
        if (!directedEdges.insert(forward).second ||
            (edge.bidirectional && !directedEdges.insert(reverse).second) ||
            !candidate.ConnectWaypoints(edge.fromId, edge.toId,
                                        edge.bidirectional)) {
            return false;
        }
    }

    nodes_.swap(candidate.nodes_);
    edges_.swap(candidate.edges_);
    return true;
}

void BotNavigation::Clear() {
    nodes_.clear();
    edges_.clear();
}

bool BotNavigation::HasWaypoint(std::uint32_t id) const {
    return nodes_.find(id) != nodes_.end();
}

BotRoute BotNavigation::BuildRoute(const Vector3& start, const Vector3& goal,
                                   bool allowDirectFallback) const {
    if (!IsPositionAllowed(start) || !IsPositionAllowed(goal)) {
        return BotRoute{};
    }

    BotRoute route = BuildAuthoredRoute(start, goal);
    if (route.IsValid()) {
        return route;
    }

    const float distance = start.Distance(goal);
    if (allowDirectFallback && std::isfinite(distance) &&
        distance <= config_.maxDirectRouteLength) {
        route.waypoints.push_back(goal);
        route.usedDirectFallback = true;
    }
    return route;
}

Vector3 BotNavigation::AdvanceAlongRoute(const Vector3& current, const BotRoute& route,
                                         std::size_t& cursor, float maxDistance) const {
    if (!IsPositionAllowed(current) || !std::isfinite(maxDistance) || maxDistance < 0.0f ||
        cursor > route.waypoints.size()) {
        return current;
    }

    float remaining = maxDistance;
    Vector3 position = current;
    while (cursor < route.waypoints.size()) {
        const Vector3& target = route.waypoints[cursor];
        if (!IsPositionAllowed(target)) {
            return position;
        }

        const Vector3 delta = target - position;
        const float distance = delta.Length();
        if (!std::isfinite(distance)) {
            return position;
        }

        if (distance <= kDistanceEpsilon) {
            position = target;
            ++cursor;
            continue;
        }

        if (remaining <= 0.0f) {
            break;
        }

        if (remaining + kDistanceEpsilon >= distance) {
            position = target;
            remaining = std::max(0.0f, remaining - distance);
            ++cursor;
            continue;
        }

        const Vector3 candidate = position + delta * (remaining / distance);
        if (IsPositionAllowed(candidate)) {
            position = candidate;
        }
        remaining = 0.0f;
    }
    return position;
}

bool BotNavigation::IsPositionAllowed(const Vector3& position) const {
    return IsFinite(position) &&
           (!config_.enforceBounds || config_.worldBounds.Contains(position));
}

BotRoute BotNavigation::BuildAuthoredRoute(const Vector3& start, const Vector3& goal) const {
    BotRoute route;
    if (nodes_.empty()) {
        return route;
    }

    std::uint32_t startNodeId = 0;
    std::uint32_t goalNodeId = 0;
    float startNodeDistance = std::numeric_limits<float>::infinity();
    float goalNodeDistance = std::numeric_limits<float>::infinity();

    for (const auto& entry : nodes_) {
        const float fromStart = start.Distance(entry.second.position);
        if (fromStart <= config_.maxEndpointSnapDistance &&
            (fromStart + kDistanceEpsilon < startNodeDistance ||
             (std::fabs(fromStart - startNodeDistance) <= kDistanceEpsilon &&
              (startNodeId == 0 || entry.first < startNodeId)))) {
            startNodeDistance = fromStart;
            startNodeId = entry.first;
        }

        const float toGoal = entry.second.position.Distance(goal);
        if (toGoal <= config_.maxEndpointSnapDistance &&
            (toGoal + kDistanceEpsilon < goalNodeDistance ||
             (std::fabs(toGoal - goalNodeDistance) <= kDistanceEpsilon &&
              (goalNodeId == 0 || entry.first < goalNodeId)))) {
            goalNodeDistance = toGoal;
            goalNodeId = entry.first;
        }
    }

    if (startNodeId == 0 || goalNodeId == 0) {
        return route;
    }

    const float infinity = std::numeric_limits<float>::infinity();
    std::map<std::uint32_t, float> distances;
    std::map<std::uint32_t, std::uint32_t> predecessors;
    for (const auto& entry : nodes_) {
        distances[entry.first] = infinity;
    }
    distances[startNodeId] = 0.0f;

    std::priority_queue<NavigationQueueEntry, std::vector<NavigationQueueEntry>,
                        NavigationQueueGreater>
        pending;
    pending.push(NavigationQueueEntry{0.0f, startNodeId});

    while (!pending.empty()) {
        const NavigationQueueEntry current = pending.top();
        pending.pop();
        if (current.distance > distances[current.nodeId] + kDistanceEpsilon) {
            continue;
        }
        if (current.nodeId == goalNodeId) {
            break;
        }

        const auto edgeIt = edges_.find(current.nodeId);
        if (edgeIt == edges_.end()) {
            continue;
        }

        for (std::uint32_t neighborId : edgeIt->second) {
            const auto neighborIt = nodes_.find(neighborId);
            const auto currentIt = nodes_.find(current.nodeId);
            if (neighborIt == nodes_.end() || currentIt == nodes_.end()) {
                continue;
            }

            const float edgeDistance =
                currentIt->second.position.Distance(neighborIt->second.position);
            if (!std::isfinite(edgeDistance) || edgeDistance <= kDistanceEpsilon ||
                edgeDistance > config_.maxEdgeLength) {
                continue;
            }

            const float candidateDistance = current.distance + edgeDistance;
            const auto predecessorIt = predecessors.find(neighborId);
            const bool lowerPredecessor =
                predecessorIt == predecessors.end() || current.nodeId < predecessorIt->second;
            if (candidateDistance + kDistanceEpsilon < distances[neighborId] ||
                (std::fabs(candidateDistance - distances[neighborId]) <= kDistanceEpsilon &&
                 lowerPredecessor)) {
                distances[neighborId] = candidateDistance;
                predecessors[neighborId] = current.nodeId;
                pending.push(NavigationQueueEntry{candidateDistance, neighborId});
            }
        }
    }

    if (!std::isfinite(distances[goalNodeId])) {
        return route;
    }

    std::vector<std::uint32_t> path;
    std::uint32_t cursor = goalNodeId;
    path.push_back(cursor);
    while (cursor != startNodeId) {
        const auto predecessorIt = predecessors.find(cursor);
        if (predecessorIt == predecessors.end() || path.size() > nodes_.size()) {
            return BotRoute{};
        }
        cursor = predecessorIt->second;
        path.push_back(cursor);
    }
    std::reverse(path.begin(), path.end());

    route.waypoints.reserve(path.size() + 1);
    for (std::uint32_t nodeId : path) {
        route.waypoints.push_back(nodes_.at(nodeId).position);
    }
    if (route.waypoints.empty() || route.waypoints.back() != goal) {
        route.waypoints.push_back(goal);
    }
    route.usedAuthoredGraph = true;
    return route;
}

void BotNavigation::PruneInvalidGraph() {
    for (auto it = nodes_.begin(); it != nodes_.end();) {
        if (!IsPositionAllowed(it->second.position)) {
            const std::uint32_t removedId = it->first;
            it = nodes_.erase(it);
            edges_.erase(removedId);
            for (auto& edgeList : edges_) {
                edgeList.second.erase(removedId);
            }
        } else {
            ++it;
        }
    }

    for (auto& edgeList : edges_) {
        const auto fromIt = nodes_.find(edgeList.first);
        for (auto edgeIt = edgeList.second.begin(); edgeIt != edgeList.second.end();) {
            const auto toIt = nodes_.find(*edgeIt);
            if (fromIt == nodes_.end() || toIt == nodes_.end() ||
                fromIt->second.position.Distance(toIt->second.position) <=
                    kDistanceEpsilon ||
                fromIt->second.position.Distance(toIt->second.position) >
                    config_.maxEdgeLength) {
                edgeIt = edgeList.second.erase(edgeIt);
            } else {
                ++edgeIt;
            }
        }
    }
}

bool BotNavigation::IsFinite(const Vector3& position) {
    return std::isfinite(position.x) && std::isfinite(position.y) &&
           std::isfinite(position.z);
}

bool BotNavigation::IsValidBounds(const Bounds& bounds) {
    return IsFinite(bounds.min) && IsFinite(bounds.max) && bounds.min.x <= bounds.max.x &&
           bounds.min.y <= bounds.max.y && bounds.min.z <= bounds.max.z;
}
