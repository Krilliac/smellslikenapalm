#include "Game/MantleAuthority.h"

#include <cmath>

namespace MantleAuthority {
namespace {

bool FiniteVector(const Vector3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool FinitePositive(float value) noexcept {
    return std::isfinite(value) && value > 0.0f;
}

bool ValidBounds(const Bounds& bounds) noexcept {
    return FiniteVector(bounds.min) && FiniteVector(bounds.max) &&
           bounds.min.x < bounds.max.x &&
           bounds.min.y < bounds.max.y &&
           bounds.min.z < bounds.max.z;
}

bool InsideAbsoluteEnvelope(const Vector3& value, float limit) noexcept {
    return std::abs(value.x) <= limit && std::abs(value.y) <= limit &&
           std::abs(value.z) <= limit;
}

} // namespace

Authority::Authority(const Config& config) : m_config(config) {}

bool Authority::IsConfigurationValid() const noexcept {
    return FinitePositive(m_config.absoluteWorldLimitUu) &&
           FinitePositive(m_config.maximumAuthorityErrorUu) &&
           std::isfinite(m_config.minimumHitDistanceUu) &&
           m_config.minimumHitDistanceUu >= 0.0f &&
           FinitePositive(m_config.maximumHitDistanceUu) &&
           m_config.minimumHitDistanceUu < m_config.maximumHitDistanceUu &&
           std::isfinite(m_config.minimumHitHeightUu) &&
           std::isfinite(m_config.maximumHitHeightUu) &&
           m_config.minimumHitHeightUu < m_config.maximumHitHeightUu &&
           FinitePositive(m_config.minimumHorizontalNormalLength) &&
           FinitePositive(m_config.maximumHorizontalNormalLength) &&
           m_config.minimumHorizontalNormalLength <
               m_config.maximumHorizontalNormalLength &&
           std::isfinite(m_config.maximumSurfaceFacingDot) &&
           m_config.maximumSurfaceFacingDot >= -1.0f &&
           m_config.maximumSurfaceFacingDot < 0.0f &&
           FinitePositive(m_config.maximumTraversalDistanceUu) &&
           FinitePositive(m_config.wallClearanceUu) &&
           FinitePositive(m_config.vaultHeightUu) &&
           FinitePositive(m_config.climbHeightUu) &&
           std::isfinite(m_config.cooldownSeconds) &&
           m_config.cooldownSeconds >= 0.0;
}

Decision Authority::TryAccept(const Request& request) {
    if (!IsConfigurationValid()) {
        return {RejectReason::InvalidConfiguration, {}};
    }
    if (!request.gameplayActive) {
        return {RejectReason::GameplayInactive, {}};
    }
    if (request.playerId == 0 || !request.playerAlive) {
        return {RejectReason::PlayerUnavailable, {}};
    }
    if (!std::isfinite(request.serverTimeSeconds) ||
        request.serverTimeSeconds < 0.0) {
        return {RejectReason::InvalidClock, {}};
    }
    if (!FiniteVector(request.authoritativePosition) ||
        !FiniteVector(request.clientTracePosition) ||
        !FiniteVector(request.hitLocation) ||
        !FiniteVector(request.hitNormal)) {
        return {RejectReason::InvalidInput, {}};
    }
    if (!ValidBounds(request.worldBounds)) {
        return {RejectReason::InvalidWorldBounds, {}};
    }

    const auto insideWorld = [&](const Vector3& position) {
        return InsideAbsoluteEnvelope(
                   position, m_config.absoluteWorldLimitUu) &&
               request.worldBounds.Contains(position);
    };
    if (!insideWorld(request.authoritativePosition) ||
        !insideWorld(request.clientTracePosition) ||
        !insideWorld(request.hitLocation)) {
        return {RejectReason::OutsideWorld, {}};
    }

    if (request.authoritativePosition.Distance(
            request.clientTracePosition) >
        m_config.maximumAuthorityErrorUu) {
        return {RejectReason::AuthorityMismatch, {}};
    }

    const Vector3 hitDelta =
        request.hitLocation - request.clientTracePosition;
    const float hitDistance2D = std::hypot(hitDelta.x, hitDelta.y);
    if (!std::isfinite(hitDistance2D) ||
        hitDistance2D < m_config.minimumHitDistanceUu ||
        hitDistance2D > m_config.maximumHitDistanceUu) {
        return {RejectReason::InvalidHitRange, {}};
    }
    if (hitDelta.z < m_config.minimumHitHeightUu ||
        hitDelta.z > m_config.maximumHitHeightUu) {
        return {RejectReason::InvalidHitHeight, {}};
    }

    const float normalLength2D =
        std::hypot(request.hitNormal.x, request.hitNormal.y);
    if (!std::isfinite(normalLength2D) ||
        normalLength2D < m_config.minimumHorizontalNormalLength ||
        normalLength2D > m_config.maximumHorizontalNormalLength) {
        return {RejectReason::InvalidSurfaceNormal, {}};
    }
    const Vector3 wallNormal{
        request.hitNormal.x / normalLength2D,
        request.hitNormal.y / normalLength2D,
        0.0f};
    const Vector3 traceDirection{
        hitDelta.x / hitDistance2D, hitDelta.y / hitDistance2D, 0.0f};
    if (wallNormal.Dot(traceDirection) >
        m_config.maximumSurfaceFacingDot) {
        return {RejectReason::SurfaceFacesAway, {}};
    }

    Traversal traversal;
    traversal.specialMove = request.wantsToClimb ? 2u : 1u;
    traversal.mantleCrouched = request.wantsToClimb;
    traversal.startLocation = request.authoritativePosition;
    traversal.endLocation = request.hitLocation -
        wallNormal * m_config.wallClearanceUu;
    traversal.height = request.wantsToClimb
        ? m_config.climbHeightUu
        : m_config.vaultHeightUu;
    traversal.endLocation.z =
        request.authoritativePosition.z + traversal.height;
    traversal.wallNormal = wallNormal * 100.0f;

    const Vector3 horizontalTraversal{
        traversal.endLocation.x - request.authoritativePosition.x,
        traversal.endLocation.y - request.authoritativePosition.y,
        0.0f};
    if (!FiniteVector(traversal.endLocation) ||
        !FiniteVector(traversal.wallNormal) ||
        horizontalTraversal.Length() >
            m_config.maximumTraversalDistanceUu ||
        !insideWorld(traversal.endLocation)) {
        return {RejectReason::InvalidDestination, {}};
    }

    {
        std::scoped_lock lock(m_mutex);
        const auto previous = m_lastAcceptedSeconds.find(request.playerId);
        if (previous != m_lastAcceptedSeconds.end()) {
            if (request.serverTimeSeconds < previous->second) {
                // A steady production clock cannot regress. Clear stale state
                // but reject this transition so a test/future clock source
                // cannot bypass the cooldown through a backwards timestamp.
                m_lastAcceptedSeconds.erase(previous);
                return {RejectReason::InvalidClock, {}};
            }
            if (request.serverTimeSeconds - previous->second <
                m_config.cooldownSeconds) {
                return {RejectReason::Cooldown, {}};
            }
        }
        m_lastAcceptedSeconds[request.playerId] = request.serverTimeSeconds;
    }

    return {RejectReason::None, traversal};
}

void Authority::ForgetPlayer(uint32_t playerId) {
    std::scoped_lock lock(m_mutex);
    m_lastAcceptedSeconds.erase(playerId);
}

void Authority::Clear() {
    std::scoped_lock lock(m_mutex);
    m_lastAcceptedSeconds.clear();
}

} // namespace MantleAuthority
