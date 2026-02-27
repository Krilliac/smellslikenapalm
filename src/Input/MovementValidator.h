// src/Input/MovementValidator.h
#pragma once

#include <cstdint>
#include <unordered_map>
#include <chrono>
#include <cmath>
#include "Math/Vector3.h"

class MovementValidator {
public:
    struct Config {
        float maxSpeed = 2000.0f;
        float maxAccel = 5000.0f;
        float maxTurnRateDeg = 720.0f;
        float maxTeleportDistance = 5000.0f;
        std::chrono::milliseconds maxUpdateInterval{5000};
    };

    explicit MovementValidator(const Config& cfg);
    ~MovementValidator();

    bool ValidateMovement(uint32_t clientId,
                          const Vector3& newPos,
                          const Vector3& newForward,
                          const std::chrono::steady_clock::time_point& timestamp);

    void ResetState(uint32_t clientId, const Vector3& pos, const Vector3& forward);

private:
    struct MovementState {
        Vector3 lastPosition;
        Vector3 lastVelocity;
        std::chrono::steady_clock::time_point lastUpdate;
    };

    Config m_cfg;
    std::unordered_map<uint32_t, MovementState> m_states;

    float ComputeSpeed(const MovementState& st, const Vector3& newPos, float dt) const;
    float ComputeAccel(const MovementState& st, float speed, float dt) const;
    float ComputeTurnRate(const MovementState& st, const Vector3& newForward, float dt) const;
};
