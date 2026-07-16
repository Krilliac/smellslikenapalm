#include "Physics/MovementSampleTiming.h"

#include <cmath>

namespace MovementSampleTiming {
namespace {

bool Equivalent(const Sample& left, const Sample& right,
                float epsilonSquared) {
    const Vector3 positionDelta = left.position - right.position;
    const Vector3 forwardDelta = left.forward - right.forward;
    const float positionDistanceSquared = positionDelta.LengthSquared();
    const float forwardDistanceSquared = forwardDelta.LengthSquared();
    return std::isfinite(positionDistanceSquared) &&
           std::isfinite(forwardDistanceSquared) &&
           positionDistanceSquared <= epsilonSquared &&
           forwardDistanceSquared <= epsilonSquared;
}

Plan Reject(Failure failure) {
    Plan plan;
    plan.failure = failure;
    return plan;
}

} // namespace

Plan BuildPlan(const std::vector<Sample>& samples,
               MovementValidator::TimePoint receiptTime,
               std::chrono::milliseconds maximumBatchSpan,
               float duplicateEpsilon) {
    if (samples.empty()) return Reject(Failure::Empty);
    if (maximumBatchSpan.count() <= 0 || !std::isfinite(duplicateEpsilon) ||
        duplicateEpsilon < 0.0f) {
        return Reject(Failure::InvalidPolicy);
    }

    const float epsilonSquared = duplicateEpsilon * duplicateEpsilon;
    bool allEquivalent = true;
    for (std::size_t i = 1; i < samples.size(); ++i) {
        if (!Equivalent(samples.front(), samples[i], epsilonSquared)) {
            allEquivalent = false;
            break;
        }
    }

    Plan plan;
    plan.samples.reserve(samples.size());
    if (samples.size() == 1 || allEquivalent) {
        for (const Sample& sample : samples) {
            plan.samples.push_back({sample, receiptTime});
        }
        plan.valid = true;
        return plan;
    }

    // Assigning one receipt timestamp to distinct positions fabricates zero
    // elapsed time. Every distinct multi-sample batch must carry protocol time.
    for (const Sample& sample : samples) {
        if (!sample.hasClientTimestamp) {
            return Reject(Failure::MissingDistinctSampleTiming);
        }
        if (!std::isfinite(sample.clientTimestampSeconds)) {
            return Reject(Failure::NonFiniteTimestamp);
        }
    }

    for (std::size_t i = 1; i < samples.size(); ++i) {
        const float previous = samples[i - 1].clientTimestampSeconds;
        const float current = samples[i].clientTimestampSeconds;
        if (current < previous) {
            return Reject(Failure::NonMonotonicTimestamp);
        }
        if (current == previous &&
            !Equivalent(samples[i - 1], samples[i], epsilonSquared)) {
            return Reject(Failure::SameTimestampMutation);
        }
    }

    const double spanSeconds = static_cast<double>(
        samples.back().clientTimestampSeconds) -
        static_cast<double>(samples.front().clientTimestampSeconds);
    const double maximumSpanSeconds =
        std::chrono::duration<double>(maximumBatchSpan).count();
    if (!std::isfinite(spanSeconds) || spanSeconds < 0.0 ||
        spanSeconds > maximumSpanSeconds) {
        return Reject(Failure::SpanExceeded);
    }

    const double newestClientTimestamp =
        static_cast<double>(samples.back().clientTimestampSeconds);
    for (const Sample& sample : samples) {
        const double ageSeconds = newestClientTimestamp -
            static_cast<double>(sample.clientTimestampSeconds);
        const auto age = std::chrono::duration_cast<
            MovementValidator::Clock::duration>(
                std::chrono::duration<double>(ageSeconds));
        plan.samples.push_back({sample, receiptTime - age});
    }
    plan.valid = true;
    return plan;
}

} // namespace MovementSampleTiming
