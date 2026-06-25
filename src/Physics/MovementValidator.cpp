// src/Physics/MovementValidator.cpp
#include "Physics/MovementValidator.h"
#include "Utils/Logger.h"
#include "../../telemetry/TelemetryManager.h"
#include <algorithm>

MovementValidator::MovementValidator(const Config& cfg)
    : m_cfg(cfg)
{
    Logger::Trace("[MovementValidator::MovementValidator] Entry - constructing with config (maxSpeed=%.2f, maxAccel=%.2f, maxTurnRateDeg=%.2f, maxTeleportDistance=%.2f)",
                  cfg.maxSpeed, cfg.maxAccel, cfg.maxTurnRateDeg, cfg.maxTeleportDistance);
    Logger::Info("[MovementValidator::MovementValidator] MovementValidator constructed with config: maxSpeed=%.2f, maxAccel=%.2f, maxTurnRateDeg=%.2f, maxTeleportDistance=%.2f, maxUpdateInterval=%lums",
                 cfg.maxSpeed, cfg.maxAccel, cfg.maxTurnRateDeg, cfg.maxTeleportDistance, cfg.maxUpdateInterval.count());
    Logger::Trace("[MovementValidator::MovementValidator] Exit");
}

MovementValidator::~MovementValidator() {
    Logger::Trace("[MovementValidator::~MovementValidator] Entry - destroying MovementValidator, tracked states count=%zu", m_states.size());
    Logger::Info("[MovementValidator::~MovementValidator] MovementValidator destroyed, was tracking %zu client states", m_states.size());
    Logger::Trace("[MovementValidator::~MovementValidator] Exit");
}

bool MovementValidator::ValidateMovement(uint32_t clientId,
                                         const Vector3& newPos,
                                         const Vector3& newForward,
                                         const std::chrono::steady_clock::time_point& timestamp)
{
    Logger::Trace("[MovementValidator::ValidateMovement] Entry - clientId=%u, newPos=(%.4f, %.4f, %.4f), newForward=(%.4f, %.4f, %.4f)",
                  clientId, newPos.x, newPos.y, newPos.z, newForward.x, newForward.y, newForward.z);

    auto it = m_states.find(clientId);
    if (it == m_states.end()) {
        // no prior state: accept and initialize
        Logger::Debug("[MovementValidator::ValidateMovement] No prior state found for client %u - initializing new state", clientId);
        m_states[clientId] = { newPos, Vector3{0,0,0}, timestamp };
        Logger::Info("[MovementValidator::ValidateMovement] Initialized new movement state for client %u at position (%.4f, %.4f, %.4f)", clientId, newPos.x, newPos.y, newPos.z);
        Logger::Trace("[MovementValidator::ValidateMovement] Exit - returning true (new client initialized)");
        return true;
    }

    Logger::Debug("[MovementValidator::ValidateMovement] Found existing state for client %u - lastPosition=(%.4f, %.4f, %.4f), lastVelocity=(%.4f, %.4f, %.4f)",
                  clientId, it->second.lastPosition.x, it->second.lastPosition.y, it->second.lastPosition.z,
                  it->second.lastVelocity.x, it->second.lastVelocity.y, it->second.lastVelocity.z);

    MovementState& st = it->second;
    auto dt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(timestamp - st.lastUpdate);
    Logger::Debug("[MovementValidator::ValidateMovement] Client %u: dt_ms=%lums, maxUpdateInterval=%lums", clientId, dt_ms.count(), m_cfg.maxUpdateInterval.count());

    if (dt_ms > m_cfg.maxUpdateInterval) {
        Logger::Warn("MovementValidator: client %u update interval too large (%lums)", clientId, dt_ms.count());
        Logger::Debug("[MovementValidator::ValidateMovement] Client %u: treating as teleport reset due to large update interval", clientId);
        // treat as teleport reset
        st = { newPos, Vector3{0,0,0}, timestamp };
        Logger::Info("[MovementValidator::ValidateMovement] Client %u state reset due to large update interval - new position (%.4f, %.4f, %.4f)", clientId, newPos.x, newPos.y, newPos.z);
        Logger::Trace("[MovementValidator::ValidateMovement] Exit - returning true (teleport reset)");
        return true;
    }
    float dt = dt_ms.count() / 1000.0f;
    Logger::Debug("[MovementValidator::ValidateMovement] Client %u: dt=%.6f seconds", clientId, dt);

    if (dt <= 0) {
        Logger::Warn("MovementValidator: client %u non-positive dt", clientId);
        Logger::Debug("[MovementValidator::ValidateMovement] Client %u: dt=%.6f is non-positive, rejecting movement", clientId, dt);
        Logger::Trace("[MovementValidator::ValidateMovement] Exit - returning false (non-positive dt)");
        return false;
    }

    // 1. Speed check
    float speed = ComputeSpeed(st, newPos, dt);
    Logger::Debug("[MovementValidator::ValidateMovement] Client %u: computed speed=%.4f, maxSpeed=%.4f", clientId, speed, m_cfg.maxSpeed);
    if (speed > m_cfg.maxSpeed) {
        Logger::Warn("MovementValidator: client %u speed %.2f > max %.2f", clientId, speed, m_cfg.maxSpeed);
        Logger::Debug("[MovementValidator::ValidateMovement] Client %u: SPEED CHECK FAILED - speed %.4f exceeds max %.4f", clientId, speed, m_cfg.maxSpeed);
        TELEMETRY_INCREMENT_SPEED_HACK();
        Logger::Trace("[MovementValidator::ValidateMovement] Exit - returning false (speed exceeded)");
        return false;
    }
    Logger::Debug("[MovementValidator::ValidateMovement] Client %u: speed check PASSED (%.4f <= %.4f)", clientId, speed, m_cfg.maxSpeed);

    // 2. Acceleration check
    float accel = ComputeAccel(st, speed, dt);
    Logger::Debug("[MovementValidator::ValidateMovement] Client %u: computed accel=%.4f, maxAccel=%.4f", clientId, accel, m_cfg.maxAccel);
    if (accel > m_cfg.maxAccel) {
        Logger::Warn("MovementValidator: client %u accel %.2f > max %.2f", clientId, accel, m_cfg.maxAccel);
        Logger::Debug("[MovementValidator::ValidateMovement] Client %u: ACCELERATION CHECK FAILED - accel %.4f exceeds max %.4f", clientId, accel, m_cfg.maxAccel);
        TELEMETRY_INCREMENT_SPEED_HACK();
        Logger::Trace("[MovementValidator::ValidateMovement] Exit - returning false (acceleration exceeded)");
        return false;
    }
    Logger::Debug("[MovementValidator::ValidateMovement] Client %u: acceleration check PASSED (%.4f <= %.4f)", clientId, accel, m_cfg.maxAccel);

    // 3. Turn rate check
    float turnRate = ComputeTurnRate(st, newForward, dt);
    Logger::Debug("[MovementValidator::ValidateMovement] Client %u: computed turnRate=%.4f deg/s, maxTurnRateDeg=%.4f deg/s", clientId, turnRate, m_cfg.maxTurnRateDeg);
    if (turnRate > m_cfg.maxTurnRateDeg) {
        Logger::Warn("MovementValidator: client %u turnRate %.2f°/s > max %.2f°/s", clientId, turnRate, m_cfg.maxTurnRateDeg);
        Logger::Debug("[MovementValidator::ValidateMovement] Client %u: TURN RATE CHECK FAILED - turnRate %.4f exceeds max %.4f", clientId, turnRate, m_cfg.maxTurnRateDeg);
        TELEMETRY_INCREMENT_SPEED_HACK();
        Logger::Trace("[MovementValidator::ValidateMovement] Exit - returning false (turn rate exceeded)");
        return false;
    }
    Logger::Debug("[MovementValidator::ValidateMovement] Client %u: turn rate check PASSED (%.4f <= %.4f)", clientId, turnRate, m_cfg.maxTurnRateDeg);

    // 4. Teleport detection
    float dist = (newPos - st.lastPosition).Length();
    Logger::Debug("[MovementValidator::ValidateMovement] Client %u: teleport check - dist=%.4f, maxTeleportDistance=%.4f, speed=%.4f", clientId, dist, m_cfg.maxTeleportDistance, speed);
    if (dist > m_cfg.maxTeleportDistance && speed < 0.01f) {
        Logger::Warn("MovementValidator: client %u teleport distance %.2f > max %.2f", clientId, dist, m_cfg.maxTeleportDistance);
        Logger::Debug("[MovementValidator::ValidateMovement] Client %u: TELEPORT CHECK FAILED - dist=%.4f > maxTeleportDistance=%.4f while speed=%.4f < 0.01", clientId, dist, m_cfg.maxTeleportDistance, speed);
        TELEMETRY_INCREMENT_SPEED_HACK();
        Logger::Trace("[MovementValidator::ValidateMovement] Exit - returning false (teleport detected)");
        return false;
    }
    Logger::Debug("[MovementValidator::ValidateMovement] Client %u: teleport check PASSED", clientId);

    // update state
    Vector3 newVel = (newPos - st.lastPosition) * (1.0f / dt);
    Logger::Debug("[MovementValidator::ValidateMovement] Client %u: updating state - newVelocity=(%.4f, %.4f, %.4f), newPosition=(%.4f, %.4f, %.4f)",
                  clientId, newVel.x, newVel.y, newVel.z, newPos.x, newPos.y, newPos.z);
    st = { newPos, newVel, timestamp };
    Logger::Info("[MovementValidator::ValidateMovement] Client %u movement validated successfully - pos=(%.4f, %.4f, %.4f), vel=(%.4f, %.4f, %.4f), speed=%.4f, accel=%.4f, turnRate=%.4f",
                 clientId, newPos.x, newPos.y, newPos.z, newVel.x, newVel.y, newVel.z, speed, accel, turnRate);
    Logger::Trace("[MovementValidator::ValidateMovement] Exit - returning true (all checks passed)");
    return true;
}

void MovementValidator::ResetState(uint32_t clientId, const Vector3& pos, const Vector3& forward) {
    Logger::Trace("[MovementValidator::ResetState] Entry - clientId=%u, pos=(%.4f, %.4f, %.4f), forward=(%.4f, %.4f, %.4f)",
                  clientId, pos.x, pos.y, pos.z, forward.x, forward.y, forward.z);
    m_states[clientId] = { pos, Vector3{0,0,0}, std::chrono::steady_clock::now() };
    Logger::Info("[MovementValidator::ResetState] Client %u state reset to position (%.4f, %.4f, %.4f) with zero velocity", clientId, pos.x, pos.y, pos.z);
    Logger::Trace("[MovementValidator::ResetState] Exit");
}

float MovementValidator::ComputeSpeed(const MovementState& st, const Vector3& newPos, float dt) const {
    Logger::Trace("[MovementValidator::ComputeSpeed] Entry - lastPosition=(%.4f, %.4f, %.4f), newPos=(%.4f, %.4f, %.4f), dt=%.6f",
                  st.lastPosition.x, st.lastPosition.y, st.lastPosition.z, newPos.x, newPos.y, newPos.z, dt);
    float dist = (newPos - st.lastPosition).Length();
    float speed = dist / dt;
    Logger::Debug("[MovementValidator::ComputeSpeed] dist=%.4f, dt=%.6f, speed=%.4f", dist, dt, speed);
    Logger::Trace("[MovementValidator::ComputeSpeed] Exit - returning speed=%.4f", speed);
    return speed;
}

float MovementValidator::ComputeAccel(const MovementState& st, float speed, float dt) const {
    Logger::Trace("[MovementValidator::ComputeAccel] Entry - lastVelocity=(%.4f, %.4f, %.4f), speed=%.4f, dt=%.6f",
                  st.lastVelocity.x, st.lastVelocity.y, st.lastVelocity.z, speed, dt);
    float oldSpeed = st.lastVelocity.Length();
    float accel = std::abs(speed - oldSpeed) / dt;
    Logger::Debug("[MovementValidator::ComputeAccel] oldSpeed=%.4f, speedDelta=%.4f, accel=%.4f", oldSpeed, std::abs(speed - oldSpeed), accel);
    Logger::Trace("[MovementValidator::ComputeAccel] Exit - returning accel=%.4f", accel);
    return accel;
}

float MovementValidator::ComputeTurnRate(const MovementState& st, const Vector3& newForward, float dt) const {
    Logger::Trace("[MovementValidator::ComputeTurnRate] Entry - lastVelocity=(%.4f, %.4f, %.4f), newForward=(%.4f, %.4f, %.4f), dt=%.6f",
                  st.lastVelocity.x, st.lastVelocity.y, st.lastVelocity.z, newForward.x, newForward.y, newForward.z, dt);
    // angle between old and new forward in degrees / dt
    float lastVelLength = st.lastVelocity.Length();
    Logger::Debug("[MovementValidator::ComputeTurnRate] lastVelocity length=%.4f", lastVelLength);
    float dot = std::clamp(lastVelLength > 0
                    ? st.lastVelocity.Normalized().Dot(newForward)
                    : 1.0f,
                  -1.0f, 1.0f);
    Logger::Debug("[MovementValidator::ComputeTurnRate] dot product=%.6f (clamped to [-1,1])", dot);
    float angleRad = std::acos(dot);
    float angleDeg = angleRad * (180.0f / 3.14159265f);
    float turnRate = angleDeg / dt;
    Logger::Debug("[MovementValidator::ComputeTurnRate] angleRad=%.6f, angleDeg=%.4f, turnRate=%.4f deg/s", angleRad, angleDeg, turnRate);
    Logger::Trace("[MovementValidator::ComputeTurnRate] Exit - returning turnRate=%.4f", turnRate);
    return turnRate;
}
