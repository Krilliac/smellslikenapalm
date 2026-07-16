#include "TestFramework.h"

#include "Physics/MovementValidator.h"
#include "Physics/MovementSampleTiming.h"

#include <chrono>
#include <limits>

namespace {
using namespace std::chrono_literals;

MovementValidator::Config TestConfig(std::size_t maxClients = 4) {
    MovementValidator::Config config;
    config.maxSpeed = 100.0f;
    config.maxAccel = 1000.0f;
    config.maxTurnRateDeg = 180.0f;
    config.maxTeleportDistance = 1000.0f;
    config.maxUpdateInterval = 1000ms;
    config.maxClients = maxClients;
    config.duplicateEpsilon = 0.001f;
    return config;
}

MovementValidator::TimePoint At(std::chrono::milliseconds offset) {
    return MovementValidator::TimePoint{} + offset;
}
}  // namespace

TEST(MovementValidator, InvalidConfigurationFailsClosed) {
    MovementValidator::Config config = TestConfig();
    config.maxSpeed = 0.0f;
    MovementValidator validator(config);

    EXPECT_FALSE(validator.IsConfigured());
    EXPECT_FALSE(validator.ResetStateAt(
        1, Vector3::Zero(), Vector3::Forward(), At(0ms)));
    const auto result = validator.ValidateMovementDetailed(
        1, Vector3::Zero(), Vector3::Forward(), At(1ms));
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.failure, MovementValidator::Failure::InvalidConfig);
}

TEST(MovementValidator, RequiresExplicitAuthoritativeReset) {
    MovementValidator validator(TestConfig());
    const auto result = validator.ValidateMovementDetailed(
        7, Vector3(50000.0f, 0.0f, 0.0f), Vector3::Forward(), At(10ms));

    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.failure, MovementValidator::Failure::StateMissing);
    EXPECT_EQ(validator.StateCount(), static_cast<std::size_t>(0));
}

TEST(MovementValidator, ValidSampleUsesSubMillisecondPrecision) {
    MovementValidator validator(TestConfig());
    ASSERT_TRUE(validator.ResetStateAt(
        1, Vector3::Zero(), Vector3::Right(), At(0ms)));

    const auto result = validator.ValidateMovementDetailed(
        1, Vector3(0.05f, 0.0f, 0.0f), Vector3::Right(),
        MovementValidator::TimePoint{} + std::chrono::microseconds(500));
    ASSERT_TRUE(result.accepted);
    EXPECT_TRUE(result.stateChanged);
    EXPECT_NEAR(result.speed, 100.0f, 0.01f);
    ASSERT_TRUE(validator.FindState(1) != nullptr);
    EXPECT_EQ(validator.FindState(1)->lastPosition, Vector3(0.05f, 0.0f, 0.0f));
}

TEST(MovementValidator, RejectionNeverMutatesLastAcceptedState) {
    MovementValidator validator(TestConfig());
    ASSERT_TRUE(validator.ResetStateAt(
        1, Vector3::Zero(), Vector3::Right(), At(0ms)));

    const auto rejected = validator.ValidateMovementDetailed(
        1, Vector3(50.0f, 0.0f, 0.0f), Vector3::Right(), At(100ms));
    EXPECT_FALSE(rejected.accepted);
    EXPECT_EQ(rejected.failure, MovementValidator::Failure::Speed);
    ASSERT_TRUE(validator.FindState(1) != nullptr);
    EXPECT_EQ(validator.FindState(1)->lastPosition, Vector3::Zero());
    EXPECT_EQ(validator.FindState(1)->lastUpdate, At(0ms));

    const auto recovered = validator.ValidateMovementDetailed(
        1, Vector3(10.0f, 0.0f, 0.0f), Vector3::Right(), At(100ms));
    EXPECT_TRUE(recovered.accepted);
}

TEST(MovementValidator, SameTimestampDuplicateIsIdempotent) {
    MovementValidator validator(TestConfig());
    ASSERT_TRUE(validator.ResetStateAt(
        1, Vector3(2.0f, 3.0f, 4.0f), Vector3::Forward(), At(50ms)));

    const auto result = validator.ValidateMovementDetailed(
        1, Vector3(2.0f, 3.0f, 4.0f), Vector3(0.0f, 2.0f, 0.0f), At(50ms));
    EXPECT_TRUE(result.accepted);
    EXPECT_FALSE(result.stateChanged);
    EXPECT_EQ(result.failure, MovementValidator::Failure::None);
}

TEST(MovementValidator, SameTimestampMutationIsRejected) {
    MovementValidator validator(TestConfig());
    ASSERT_TRUE(validator.ResetStateAt(
        1, Vector3::Zero(), Vector3::Forward(), At(50ms)));

    const auto result = validator.ValidateMovementDetailed(
        1, Vector3(0.1f, 0.0f, 0.0f), Vector3::Forward(), At(50ms));
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.failure, MovementValidator::Failure::NonMonotonicTime);
    EXPECT_EQ(validator.FindState(1)->lastPosition, Vector3::Zero());
}

TEST(MovementValidator, LongGapCannotAuthorizeAPacketTeleport) {
    MovementValidator validator(TestConfig());
    ASSERT_TRUE(validator.ResetStateAt(
        1, Vector3::Zero(), Vector3::Forward(), At(0ms)));

    const auto stale = validator.ValidateMovementDetailed(
        1, Vector3(500.0f, 0.0f, 0.0f), Vector3::Forward(), At(2000ms));
    EXPECT_FALSE(stale.accepted);
    EXPECT_EQ(stale.failure, MovementValidator::Failure::LongGap);
    EXPECT_EQ(validator.FindState(1)->lastPosition, Vector3::Zero());

    ASSERT_TRUE(validator.ResetStateAt(
        1, Vector3(500.0f, 0.0f, 0.0f), Vector3::Forward(), At(2000ms)));
    EXPECT_TRUE(validator.ValidateMovement(
        1, Vector3(501.0f, 0.0f, 0.0f), Vector3::Forward(), At(2100ms)));
}

TEST(MovementValidator, TeleportDistanceIsIndependentOfSpeedExpression) {
    auto config = TestConfig();
    config.maxSpeed = 10000.0f;
    config.maxTeleportDistance = 10.0f;
    MovementValidator validator(config);
    ASSERT_TRUE(validator.ResetStateAt(
        1, Vector3::Zero(), Vector3::Right(), At(0ms)));

    const auto result = validator.ValidateMovementDetailed(
        1, Vector3(11.0f, 0.0f, 0.0f), Vector3::Right(), At(100ms));
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.failure, MovementValidator::Failure::TeleportDistance);
}

TEST(MovementValidator, VectorAccelerationRejectsInstantDirectionReversal) {
    MovementValidator validator(TestConfig());
    ASSERT_TRUE(validator.ResetStateAt(
        1, Vector3::Zero(), Vector3::Right(), At(0ms)));
    ASSERT_TRUE(validator.ValidateMovement(
        1, Vector3(10.0f, 0.0f, 0.0f), Vector3::Right(), At(100ms)));

    const auto result = validator.ValidateMovementDetailed(
        1, Vector3::Zero(), Vector3::Right(), At(200ms));
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.failure, MovementValidator::Failure::Acceleration);
    EXPECT_NEAR(result.acceleration, 2000.0f, 0.1f);
}

TEST(MovementValidator, TurnRateUsesPriorForwardEvenWhileStationary) {
    auto config = TestConfig();
    config.maxTurnRateDeg = 90.0f;
    MovementValidator validator(config);
    ASSERT_TRUE(validator.ResetStateAt(
        1, Vector3::Zero(), Vector3::Right(), At(0ms)));

    const auto result = validator.ValidateMovementDetailed(
        1, Vector3::Zero(), Vector3::Left(), At(500ms));
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.failure, MovementValidator::Failure::TurnRate);
    EXPECT_NEAR(result.turnRateDeg, 360.0f, 0.1f);
}

TEST(MovementValidator, RejectsNonFiniteAndZeroForwardInputs) {
    MovementValidator validator(TestConfig());
    ASSERT_TRUE(validator.ResetStateAt(
        1, Vector3::Zero(), Vector3::Forward(), At(0ms)));

    const float nan = std::numeric_limits<float>::quiet_NaN();
    auto result = validator.ValidateMovementDetailed(
        1, Vector3(nan, 0.0f, 0.0f), Vector3::Forward(), At(10ms));
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.failure, MovementValidator::Failure::NonFiniteInput);

    result = validator.ValidateMovementDetailed(
        1, Vector3::Zero(), Vector3::Zero(), At(10ms));
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.failure, MovementValidator::Failure::InvalidForward);
    EXPECT_EQ(validator.FindState(1)->lastUpdate, At(0ms));
}

TEST(MovementValidator, CapacityRemovalAndPruningAreExplicit) {
    MovementValidator validator(TestConfig(2));
    ASSERT_TRUE(validator.ResetStateAt(
        1, Vector3::Zero(), Vector3::Forward(), At(0ms)));
    ASSERT_TRUE(validator.ResetStateAt(
        2, Vector3::Zero(), Vector3::Forward(), At(1000ms)));
    EXPECT_FALSE(validator.ResetStateAt(
        3, Vector3::Zero(), Vector3::Forward(), At(2000ms)));

    EXPECT_EQ(validator.PruneBefore(At(500ms)), static_cast<std::size_t>(1));
    EXPECT_TRUE(validator.FindState(1) == nullptr);
    EXPECT_TRUE(validator.FindState(2) != nullptr);
    EXPECT_TRUE(validator.ResetStateAt(
        3, Vector3::Zero(), Vector3::Forward(), At(2000ms)));
    EXPECT_TRUE(validator.RemoveState(2));
    EXPECT_FALSE(validator.RemoveState(2));
    EXPECT_EQ(validator.StateCount(), static_cast<std::size_t>(1));
}

TEST(MovementSampleTiming, DistinctBatchUsesProtocolDeltasAtReceipt) {
    std::vector<MovementSampleTiming::Sample> samples{
        {Vector3(1.0f, 0.0f, 0.0f), Vector3::Forward(), true, 10.0f},
        {Vector3(2.0f, 0.0f, 0.0f), Vector3::Forward(), true, 10.025f},
    };

    const auto receipt = At(1000ms);
    const auto plan = MovementSampleTiming::BuildPlan(
        samples, receipt, 1000ms);

    ASSERT_TRUE(plan.valid);
    ASSERT_EQ(plan.samples.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(plan.samples[1].timestamp, receipt);
    const double firstSampleAgeMs = std::chrono::duration<double, std::milli>(
        receipt - plan.samples[0].timestamp).count();
    EXPECT_NEAR(firstSampleAgeMs, 25.0, 0.01);
}

TEST(MovementSampleTiming, DistinctBatchRequiresCompleteProtocolTiming) {
    std::vector<MovementSampleTiming::Sample> samples{
        {Vector3(1.0f, 0.0f, 0.0f), Vector3::Forward(), true, 10.0f},
        {Vector3(2.0f, 0.0f, 0.0f), Vector3::Forward(), false, 0.0f},
    };

    const auto plan = MovementSampleTiming::BuildPlan(
        samples, At(1000ms), 1000ms);
    EXPECT_FALSE(plan.valid);
    EXPECT_EQ(plan.failure,
              MovementSampleTiming::Failure::MissingDistinctSampleTiming);
}

TEST(MovementSampleTiming, SameTimestampMutationIsRejectedBeforeValidation) {
    std::vector<MovementSampleTiming::Sample> samples{
        {Vector3(1.0f, 0.0f, 0.0f), Vector3::Forward(), true, 10.0f},
        {Vector3(2.0f, 0.0f, 0.0f), Vector3::Forward(), true, 10.0f},
    };

    const auto plan = MovementSampleTiming::BuildPlan(
        samples, At(1000ms), 1000ms);
    EXPECT_FALSE(plan.valid);
    EXPECT_EQ(plan.failure,
              MovementSampleTiming::Failure::SameTimestampMutation);
}

TEST(MovementSampleTiming, UntimedDuplicateBatchRemainsIdempotent) {
    MovementValidator validator(TestConfig());
    ASSERT_TRUE(validator.ResetStateAt(
        1, Vector3::Zero(), Vector3::Forward(), At(900ms)));
    std::vector<MovementSampleTiming::Sample> samples{
        {Vector3(10.0f, 0.0f, 0.0f), Vector3::Forward(), false, 0.0f},
        {Vector3(10.0f, 0.0f, 0.0f), Vector3::Forward(), false, 0.0f},
    };

    const auto plan = MovementSampleTiming::BuildPlan(
        samples, At(1000ms), 1000ms);
    ASSERT_TRUE(plan.valid);
    ASSERT_EQ(plan.samples.size(), static_cast<std::size_t>(2));
    const auto first = validator.ValidateMovementDetailed(
        1, plan.samples[0].sample.position, plan.samples[0].sample.forward,
        plan.samples[0].timestamp);
    const auto duplicate = validator.ValidateMovementDetailed(
        1, plan.samples[1].sample.position, plan.samples[1].sample.forward,
        plan.samples[1].timestamp);
    EXPECT_TRUE(first.accepted);
    EXPECT_TRUE(first.stateChanged);
    EXPECT_TRUE(duplicate.accepted);
    EXPECT_FALSE(duplicate.stateChanged);
}

TEST(MovementSampleTiming, RetransmitInLaterReceiptWindowRemainsValid) {
    MovementValidator validator(TestConfig());
    ASSERT_TRUE(validator.ResetStateAt(
        1, Vector3::Zero(), Vector3::Forward(), At(900ms)));
    const std::vector<MovementSampleTiming::Sample> retransmit{
        {Vector3(10.0f, 0.0f, 0.0f), Vector3::Forward(), true, 42.0f},
    };

    const auto firstPlan = MovementSampleTiming::BuildPlan(
        retransmit, At(1000ms), 1000ms);
    const auto secondPlan = MovementSampleTiming::BuildPlan(
        retransmit, At(1100ms), 1000ms);
    ASSERT_TRUE(firstPlan.valid);
    ASSERT_TRUE(secondPlan.valid);
    EXPECT_TRUE(validator.ValidateMovement(
        1, firstPlan.samples[0].sample.position,
        firstPlan.samples[0].sample.forward,
        firstPlan.samples[0].timestamp));
    const auto second = validator.ValidateMovementDetailed(
        1, secondPlan.samples[0].sample.position,
        secondPlan.samples[0].sample.forward,
        secondPlan.samples[0].timestamp);
    EXPECT_TRUE(second.accepted);
    EXPECT_TRUE(second.stateChanged);
    EXPECT_EQ(validator.FindState(1)->lastPosition,
              Vector3(10.0f, 0.0f, 0.0f));
}

TEST(MovementSampleTiming, RejectsRegressingAndOverspanClientTime) {
    std::vector<MovementSampleTiming::Sample> samples{
        {Vector3(1.0f, 0.0f, 0.0f), Vector3::Forward(), true, 12.0f},
        {Vector3(2.0f, 0.0f, 0.0f), Vector3::Forward(), true, 11.0f},
    };
    auto plan = MovementSampleTiming::BuildPlan(
        samples, At(1000ms), 1000ms);
    EXPECT_FALSE(plan.valid);
    EXPECT_EQ(plan.failure,
              MovementSampleTiming::Failure::NonMonotonicTimestamp);

    samples[0].clientTimestampSeconds = 10.0f;
    samples[1].clientTimestampSeconds = 12.0f;
    plan = MovementSampleTiming::BuildPlan(
        samples, At(3000ms), 1000ms);
    EXPECT_FALSE(plan.valid);
    EXPECT_EQ(plan.failure, MovementSampleTiming::Failure::SpanExceeded);
}

RS2V_TEST_MAIN()
