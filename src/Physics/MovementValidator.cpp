// src/Input/MovementValidator.cpp
#include "Input/MovementValidator.h"
#include "Utils/Logger.h"
#include <algorithm>

MovementValidator::MovementValidator(const Config& cfg)
    : m_cfg(cfg)
{}

MovementValidator::~MovementValidator() = default;

bool MovementValidator::ValidateMovement(uint32_t clientId,
                                         const Vector3& newPos,
                                         const Vector3& newForward,
                                         const std::chrono::steady_clock::time_point& timestamp)
{
    auto it = m_states.find(clientId);
    if (it == m_states.end()) {
        // no prior state: accept and initialize
        m_states[clientId] = { newPos, Vector3{0,0,0}, timestamp };
        return true;
    }

    MovementState& st = it->second;
    auto dt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(timestamp - st.lastUpdate);
    if (dt_ms > m_cfg.maxUpdateInterval) {
        Logger::Warn("MovementValidator: client %u update interval too large (%lums)", clientId, dt_ms.count());
        // treat as teleport reset
        st = { newPos, Vector3{0,0,0}, timestamp };
        return true;
    }
    float dt = dt_ms.count() / 1000.0f;
    if (dt <= 0) {
        Logger::Warn("MovementValidator: client %u non-positive dt", clientId);
        return false;
    }

    // 1. Speed check
    float speed = ComputeSpeed(st, newPos, dt);
    if (speed > m_cfg.maxSpeed) {
        Logger::Warn("MovementValidator: client %u speed %.2f > max %.2f", clientId, speed, m_cfg.maxSpeed);
        return false;
    }

    // 2. Acceleration check
    float accel = ComputeAccel(st, speed, dt);
    if (accel > m_cfg.maxAccel) {
        Logger::Warn("MovementValidator: client %u accel %.2f > max %.2f", clientId, accel, m_cfg.maxAccel);
        return false;
    }

    // 3. Turn rate check
    float turnRate = ComputeTurnRate(st, newForward, dt);
    if (turnRate > m_cfg.maxTurnRateDeg) {
        Logger::Warn("MovementValidator: client %u turnRate %.2f°/s > max %.2f°/s", clientId, turnRate, m_cfg.maxTurnRateDeg);
        return false;
    }

    // 4. Teleport detection
    float dist = (newPos - st.lastPosition).Length();
    if (dist > m_cfg.maxTeleportDistance && speed < 0.01f) {
        Logger::Warn("MovementValidator: client %u teleport distance %.2f > max %.2f", clientId, dist, m_cfg.maxTeleportDistance);
        return false;
    }

    // update state
    Vector3 newVel = (newPos - st.lastPosition) * (1.0f / dt);
    st = { newPos, newVel, timestamp };
    return true;
}

void MovementValidator::ResetState(uint32_t clientId, const Vector3& pos, const Vector3& forward) {
    m_states[clientId] = { pos, Vector3{0,0,0}, std::chrono::steady_clock::now() };
}

float MovementValidator::ComputeSpeed(const MovementState& st, const Vector3& newPos, float dt) const {
    float dist = (newPos - st.lastPosition).Length();
    return dist / dt;
}

float MovementValidator::ComputeAccel(const MovementState& st, float speed, float dt) const {
    float oldSpeed = st.lastVelocity.Length();
    return std::abs(speed - oldSpeed) / dt;
}

float MovementValidator::ComputeTurnRate(const MovementState& st, const Vector3& newForward, float dt) const {
    // angle between old and new forward in degrees / dt
    float dot = std::clamp(st.lastVelocity.Length() > 0
                    ? st.lastVelocity.Normalized().Dot(newForward)
                    : 1.0f,
                  -1.0f, 1.0f);
    float angleRad = std::acos(dot);
    float angleDeg = angleRad * (180.0f / 3.14159265f);
    return angleDeg / dt;
}