#pragma once

#include "Physics/MovementValidator.h"

#include <chrono>
#include <cstdint>
#include <vector>

// Converts decoded UE3 movement RPC timing into the server steady-clock domain.
// Client TimeStamp is useful only as an intra-bunch delta: its absolute epoch is
// client-owned and cannot be compared with steady_clock or authoritative reset
// times. The newest sample is therefore anchored to receipt time and older
// samples are placed before it by their evidenced protocol deltas.
namespace MovementSampleTiming {

struct Sample {
    Vector3 position{};
    Vector3 forward{};
    bool hasClientTimestamp = false;
    float clientTimestampSeconds = 0.0f;
};

enum class Failure : std::uint8_t {
    None = 0,
    Empty,
    InvalidPolicy,
    NonFiniteTimestamp,
    MissingDistinctSampleTiming,
    NonMonotonicTimestamp,
    SameTimestampMutation,
    SpanExceeded,
};

struct PlannedSample {
    Sample sample{};
    MovementValidator::TimePoint timestamp{};
};

struct Plan {
    bool valid = false;
    Failure failure = Failure::None;
    std::vector<PlannedSample> samples;
};

Plan BuildPlan(
    const std::vector<Sample>& samples,
    MovementValidator::TimePoint receiptTime,
    std::chrono::milliseconds maximumBatchSpan,
    float duplicateEpsilon = 0.001f);

} // namespace MovementSampleTiming
