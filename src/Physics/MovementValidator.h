// src/Input/MovementValidator.h
#pragma once

#include "Math/Vector3.h"
#include <cstdint>
#include <chrono>

class MovementValidator {
public:
    struct MovementState {
        Vector3 lastPosition;
        Vector3 lastVelocity;
        std::chrono::steady_clock::time_point lastUpdate;
    };

    // Configuration limits
    struct Config {
        float maxSpeed;              // units per second
        float maxAccel;              // units per second²
        float maxTurnRateDeg;        // degrees per second
        float maxTeleportDistance;   // units
        std::chrono::milliseconds maxUpdateInterval;
    };

    MovementValidator(const Config& cfg);
    ~MovementValidator();

    // Call on each movement packet; returns true if valid
    bool ValidateMovement(uint32_t clientId,
                          const Vector3& newPos,
                          const Vector3& newForward,  // normalized direction
                          const std::chrono::steady_clock::time_point& timestamp);

    // Reset state when player spawns or teleport legitimately
    void ResetState(uint32_t clientId, const Vector3& pos, const Vector3& forward);

private:
    Config m_cfg;

    // Per-client state
    std::unordered_map<uint32_t, MovementState> m_states;

    // Helpers
    float ComputeSpeed(const MovementState& st, const Vector3& newPos, float dt) const;
    float ComputeAccel(const MovementState& st, float speed, float dt) const;
    float ComputeTurnRate(const MovementState& st, const Vector3& newForward, float dt) const;
};