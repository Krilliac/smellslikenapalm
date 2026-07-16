// src/Time/FrameTiming.h
// Shared conversion policy for measured wall-clock frame deltas.

#pragma once

#include <algorithm>
#include <cmath>

namespace FrameTiming {

// Large debugger/OS-pause deltas must not be applied to physics and gameplay in
// one giant step. Normal server load is still accounted for exactly down to
// 10 Hz (or to a deliberately configured slower nominal tick).
inline constexpr float kMaxTickDeltaSeconds = 0.100f;
inline constexpr float kDefaultTickDeltaSeconds = 1.0f / 60.0f;

// Production passes a steady-clock measurement. A non-positive/non-finite
// value means the caller did not provide one (for example, a deterministic
// direct GameServer::Run() in a test), so use the configured nominal interval.
inline float ResolveTickDeltaSeconds(float measuredSeconds,
                                     float nominalSeconds) noexcept {
    const float fallback =
        (std::isfinite(nominalSeconds) && nominalSeconds > 0.0f)
            ? nominalSeconds
            : kDefaultTickDeltaSeconds;

    if (!std::isfinite(measuredSeconds) || measuredSeconds <= 0.0f) {
        return fallback;
    }
    const float maxStep = std::max(kMaxTickDeltaSeconds, fallback);
    return std::min(measuredSeconds, maxStep);
}

} // namespace FrameTiming
