#pragma once

#include "Math/Vector3.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

// Deterministic, stateful validation for authoritative movement samples.
//
// This type is intentionally main-thread-affine.  Network decoders may stage
// samples from worker threads, but the authoritative game thread owns both the
// Player mutation and this validator.  Spawn/teleport acceptance is never
// inferred from a packet gap: callers must explicitly ResetStateAt() after an
// authoritative lifecycle transition.
class MovementValidator {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    struct Config {
        float maxSpeed = 0.0f;             // Unreal units per second
        float maxAccel = 0.0f;             // Unreal units per second squared
        float maxTurnRateDeg = 0.0f;       // degrees per second
        float maxTeleportDistance = 0.0f;  // maximum packet-driven displacement
        std::chrono::milliseconds maxUpdateInterval{0};
        std::size_t maxClients = 0;
        float duplicateEpsilon = 0.001f;
    };

    enum class Failure : std::uint8_t {
        None = 0,
        InvalidConfig,
        InvalidClient,
        NonFiniteInput,
        InvalidForward,
        StateMissing,
        NonMonotonicTime,
        LongGap,
        TeleportDistance,
        Speed,
        Acceleration,
        TurnRate,
        CapacityExceeded,
    };

    struct Result {
        bool accepted = false;
        bool stateChanged = false;
        Failure failure = Failure::None;
        float distance = 0.0f;
        float speed = 0.0f;
        float acceleration = 0.0f;
        float turnRateDeg = 0.0f;
    };

    struct MovementState {
        Vector3 lastPosition;
        Vector3 lastVelocity;
        Vector3 lastForward;
        TimePoint lastUpdate{};
        bool hasKinematicSample = false;
    };

    explicit MovementValidator(const Config& config);

    bool IsConfigured() const { return configured_; }
    const Config& GetConfig() const { return config_; }

    // Validates one already-decoded sample.  Rejected samples never mutate the
    // stored state.  A byte-for-byte-equivalent same-time duplicate is accepted
    // idempotently and likewise does not mutate state.
    Result ValidateMovementDetailed(std::uint32_t clientId,
                                    const Vector3& newPosition,
                                    const Vector3& newForward,
                                    TimePoint timestamp);

    // Compatibility convenience for callers that only need accept/reject.
    bool ValidateMovement(std::uint32_t clientId,
                          const Vector3& newPosition,
                          const Vector3& newForward,
                          TimePoint timestamp) {
        return ValidateMovementDetailed(clientId, newPosition, newForward,
                                        timestamp)
            .accepted;
    }

    // Lifecycle-authoritative state changes.  Reset does not evict another
    // client when at capacity; a caller must RemoveState() on disconnect.
    bool ResetStateAt(std::uint32_t clientId, const Vector3& position,
                      const Vector3& forward, TimePoint timestamp);
    bool ResetState(std::uint32_t clientId, const Vector3& position,
                    const Vector3& forward) {
        return ResetStateAt(clientId, position, forward, Clock::now());
    }
    bool RemoveState(std::uint32_t clientId);
    void Clear();
    std::size_t PruneBefore(TimePoint cutoff);

    const MovementState* FindState(std::uint32_t clientId) const;
    std::size_t StateCount() const { return states_.size(); }

    static bool IsValidConfig(const Config& config);

private:
    static bool IsFinite(const Vector3& value);
    static bool NormalizeForward(const Vector3& value, Vector3& normalized);
    static float AngleDegrees(const Vector3& left, const Vector3& right);

    Result Reject(Failure failure, const Result& measurements,
                  std::uint32_t clientId) const;

    Config config_;
    bool configured_ = false;
    std::unordered_map<std::uint32_t, MovementState> states_;
};
