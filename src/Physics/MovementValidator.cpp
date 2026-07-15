#include "Physics/MovementValidator.h"

#include "Utils/Logger.h"
#include "../../telemetry/TelemetryManager.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr float kRadiansToDegrees = 57.29577951308232f;
}

MovementValidator::MovementValidator(const Config& config)
    : config_(config), configured_(IsValidConfig(config)) {
    if (!configured_) {
        Logger::Error("[MovementValidator] invalid configuration; validator will fail closed");
    }
}

bool MovementValidator::IsValidConfig(const Config& config) {
    return std::isfinite(config.maxSpeed) && config.maxSpeed > 0.0f &&
           std::isfinite(config.maxAccel) && config.maxAccel > 0.0f &&
           std::isfinite(config.maxTurnRateDeg) &&
           config.maxTurnRateDeg > 0.0f &&
           std::isfinite(config.maxTeleportDistance) &&
           config.maxTeleportDistance > 0.0f &&
           config.maxUpdateInterval.count() > 0 && config.maxClients > 0 &&
           std::isfinite(config.duplicateEpsilon) &&
           config.duplicateEpsilon >= 0.0f;
}

bool MovementValidator::IsFinite(const Vector3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool MovementValidator::NormalizeForward(const Vector3& value,
                                         Vector3& normalized) {
    if (!IsFinite(value)) return false;
    const float lengthSquared = value.LengthSquared();
    if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-12f) {
        return false;
    }
    const float inverseLength = 1.0f / std::sqrt(lengthSquared);
    normalized = value * inverseLength;
    return IsFinite(normalized);
}

float MovementValidator::AngleDegrees(const Vector3& left,
                                      const Vector3& right) {
    const float dot = std::clamp(left.Dot(right), -1.0f, 1.0f);
    return std::acos(dot) * kRadiansToDegrees;
}

MovementValidator::Result MovementValidator::Reject(
    Failure failure, const Result& measurements, std::uint32_t clientId) const {
    Result result = measurements;
    result.accepted = false;
    result.stateChanged = false;
    result.failure = failure;

    if (failure == Failure::TeleportDistance || failure == Failure::Speed ||
        failure == Failure::Acceleration || failure == Failure::TurnRate) {
        TELEMETRY_INCREMENT_SPEED_HACK();
        Logger::Warn(
            "[MovementValidator] rejected client %u movement (reason=%u "
            "distance=%.2f speed=%.2f accel=%.2f turn=%.2f)",
            clientId, static_cast<unsigned>(failure), result.distance,
            result.speed, result.acceleration, result.turnRateDeg);
    }
    return result;
}

MovementValidator::Result MovementValidator::ValidateMovementDetailed(
    std::uint32_t clientId, const Vector3& newPosition,
    const Vector3& newForward, TimePoint timestamp) {
    Result result;
    if (!configured_) return Reject(Failure::InvalidConfig, result, clientId);
    if (clientId == 0) return Reject(Failure::InvalidClient, result, clientId);
    if (!IsFinite(newPosition) || !IsFinite(newForward)) {
        return Reject(Failure::NonFiniteInput, result, clientId);
    }

    Vector3 normalizedForward;
    if (!NormalizeForward(newForward, normalizedForward)) {
        return Reject(Failure::InvalidForward, result, clientId);
    }

    auto stateIt = states_.find(clientId);
    if (stateIt == states_.end()) {
        return Reject(Failure::StateMissing, result, clientId);
    }
    const MovementState& state = stateIt->second;

    if (timestamp < state.lastUpdate) {
        return Reject(Failure::NonMonotonicTime, result, clientId);
    }

    const Vector3 displacement = newPosition - state.lastPosition;
    result.distance = displacement.Length();
    if (!std::isfinite(result.distance)) {
        return Reject(Failure::NonFiniteInput, result, clientId);
    }

    if (timestamp == state.lastUpdate) {
        const float epsilonSquared =
            config_.duplicateEpsilon * config_.duplicateEpsilon;
        const bool samePosition = displacement.LengthSquared() <= epsilonSquared;
        const bool sameForward =
            (normalizedForward - state.lastForward).LengthSquared() <=
            epsilonSquared;
        if (samePosition && sameForward) {
            result.accepted = true;
            return result;  // idempotent duplicate; deliberately no mutation
        }
        return Reject(Failure::NonMonotonicTime, result, clientId);
    }

    const auto elapsed = timestamp - state.lastUpdate;
    if (elapsed > config_.maxUpdateInterval) {
        return Reject(Failure::LongGap, result, clientId);
    }
    const float deltaSeconds =
        std::chrono::duration<float>(elapsed).count();
    if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f) {
        return Reject(Failure::NonMonotonicTime, result, clientId);
    }

    if (result.distance > config_.maxTeleportDistance) {
        return Reject(Failure::TeleportDistance, result, clientId);
    }

    const Vector3 velocity = displacement * (1.0f / deltaSeconds);
    result.speed = velocity.Length();
    if (!IsFinite(velocity) || !std::isfinite(result.speed)) {
        return Reject(Failure::NonFiniteInput, result, clientId);
    }
    if (result.speed > config_.maxSpeed) {
        return Reject(Failure::Speed, result, clientId);
    }

    if (state.hasKinematicSample) {
        result.acceleration =
            (velocity - state.lastVelocity).Length() / deltaSeconds;
        if (!std::isfinite(result.acceleration)) {
            return Reject(Failure::NonFiniteInput, result, clientId);
        }
        if (result.acceleration > config_.maxAccel) {
            return Reject(Failure::Acceleration, result, clientId);
        }
    }

    result.turnRateDeg =
        AngleDegrees(state.lastForward, normalizedForward) / deltaSeconds;
    if (!std::isfinite(result.turnRateDeg)) {
        return Reject(Failure::NonFiniteInput, result, clientId);
    }
    if (result.turnRateDeg > config_.maxTurnRateDeg) {
        return Reject(Failure::TurnRate, result, clientId);
    }

    MovementState accepted;
    accepted.lastPosition = newPosition;
    accepted.lastVelocity = velocity;
    accepted.lastForward = normalizedForward;
    accepted.lastUpdate = timestamp;
    accepted.hasKinematicSample = true;
    stateIt->second = accepted;

    result.accepted = true;
    result.stateChanged = true;
    return result;
}

bool MovementValidator::ResetStateAt(std::uint32_t clientId,
                                     const Vector3& position,
                                     const Vector3& forward,
                                     TimePoint timestamp) {
    if (!configured_ || clientId == 0 || !IsFinite(position) ||
        !IsFinite(forward)) {
        return false;
    }
    Vector3 normalizedForward;
    if (!NormalizeForward(forward, normalizedForward)) return false;

    auto stateIt = states_.find(clientId);
    if (stateIt == states_.end() && states_.size() >= config_.maxClients) {
        return false;
    }

    MovementState reset;
    reset.lastPosition = position;
    reset.lastVelocity = Vector3::Zero();
    reset.lastForward = normalizedForward;
    reset.lastUpdate = timestamp;
    reset.hasKinematicSample = false;
    states_[clientId] = reset;
    return true;
}

bool MovementValidator::RemoveState(std::uint32_t clientId) {
    return clientId != 0 && states_.erase(clientId) != 0;
}

void MovementValidator::Clear() {
    states_.clear();
}

std::size_t MovementValidator::PruneBefore(TimePoint cutoff) {
    std::size_t removed = 0;
    for (auto it = states_.begin(); it != states_.end();) {
        if (it->second.lastUpdate < cutoff) {
            it = states_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

const MovementValidator::MovementState* MovementValidator::FindState(
    std::uint32_t clientId) const {
    const auto it = states_.find(clientId);
    return it == states_.end() ? nullptr : &it->second;
}
