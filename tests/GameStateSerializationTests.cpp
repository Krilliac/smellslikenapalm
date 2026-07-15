#include "TestFramework.h"

#include "Game/GameState.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include <vector>

namespace {

constexpr size_t kTeamCountOffset =
    sizeof(uint8_t) * 2 + sizeof(uint32_t) + sizeof(int64_t);
constexpr size_t kTeamScoreWireSize = sizeof(uint32_t) * 5;
constexpr uint32_t kFixtureTeamCount = 2;
constexpr size_t kObjectiveCountOffset =
    kTeamCountOffset + sizeof(uint32_t) +
    kFixtureTeamCount * kTeamScoreWireSize;
constexpr size_t kObjectiveWireSize =
    sizeof(uint32_t) * 2 + sizeof(float) + sizeof(uint8_t) + 3 + sizeof(int64_t);
constexpr size_t kFixtureWireSize =
    kObjectiveCountOffset + sizeof(uint32_t) + 2 * kObjectiveWireSize;

template <typename T>
void AppendScalar(std::vector<uint8_t>& bytes, const T& value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* first = reinterpret_cast<const uint8_t*>(&value);
    bytes.insert(bytes.end(), first, first + sizeof(T));
}

template <typename T>
T ReadScalarAt(const std::vector<uint8_t>& bytes, size_t offset)
{
    static_assert(std::is_trivially_copyable_v<T>);
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

template <typename T>
void WriteScalarAt(std::vector<uint8_t>& bytes, size_t offset, const T& value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

void AppendTeamScore(std::vector<uint8_t>& bytes,
                     uint32_t teamId,
                     uint32_t score,
                     uint32_t kills,
                     uint32_t deaths,
                     uint32_t objectivesCaptured)
{
    AppendScalar(bytes, teamId);
    AppendScalar(bytes, score);
    AppendScalar(bytes, kills);
    AppendScalar(bytes, deaths);
    AppendScalar(bytes, objectivesCaptured);
}

void AppendObjective(std::vector<uint8_t>& bytes,
                     uint32_t objectiveId,
                     uint32_t controllingTeam,
                     float captureProgress,
                     bool isNeutral,
                     int64_t captureTicks)
{
    AppendScalar(bytes, objectiveId);
    AppendScalar(bytes, controllingTeam);
    AppendScalar(bytes, captureProgress);
    AppendScalar(bytes, static_cast<uint8_t>(isNeutral));
    bytes.insert(bytes.end(), 3, uint8_t{0});
    AppendScalar(bytes, captureTicks);
}

std::vector<uint8_t> BuildLegacyCompatibleFixture()
{
    std::vector<uint8_t> bytes;
    bytes.reserve(kFixtureWireSize);

    AppendScalar(bytes, static_cast<uint8_t>(GamePhase::PostRound));
    AppendScalar(bytes, static_cast<uint8_t>(MatchState::Finished));
    AppendScalar(bytes, uint32_t{7});
    AppendScalar(bytes, int64_t{0});

    AppendScalar(bytes, kFixtureTeamCount);
    AppendTeamScore(bytes, 1, 125, 9, 4, 3);
    AppendTeamScore(bytes, 2, 88, 6, 7, 1);

    AppendScalar(bytes, uint32_t{2});
    AppendObjective(bytes, 11, 1, 1.0f, false, 123456789);
    AppendObjective(bytes, 12, 0, 0.25f, true, 987654321);
    return bytes;
}

void ExpectStateUnchanged(GameState& state,
                          const std::vector<uint8_t>& expectedWire)
{
    EXPECT_EQ(state.SerializeGameState(), expectedWire);
    EXPECT_EQ(state.GetPhase(), GamePhase::PostRound);
    EXPECT_EQ(state.GetMatchState(), MatchState::Finished);
    EXPECT_EQ(state.GetCurrentRound(), 7u);
    EXPECT_EQ(state.GetTeamScores().size(), 2u);
    EXPECT_EQ(state.GetObjectives().size(), 2u);
}

} // namespace

TEST(GameStateSerialization, EmitsExactLegacyCompatibleFieldLayout)
{
    const auto expected = BuildLegacyCompatibleFixture();
    ASSERT_EQ(expected.size(), kFixtureWireSize);
    ASSERT_EQ(kFixtureWireSize, 110u);

    GameState state(nullptr);
    state.DeserializeGameState(expected);
    const auto encoded = state.SerializeGameState();

    EXPECT_EQ(encoded, expected);
    EXPECT_EQ(ReadScalarAt<uint32_t>(encoded, kTeamCountOffset), 2u);
    EXPECT_EQ(ReadScalarAt<uint32_t>(encoded, kObjectiveCountOffset), 2u);

    const size_t firstObjectiveOffset =
        kObjectiveCountOffset + sizeof(uint32_t);
    EXPECT_EQ(ReadScalarAt<uint32_t>(encoded, firstObjectiveOffset), 11u);
    EXPECT_EQ(encoded[firstObjectiveOffset + 12], uint8_t{0});
    EXPECT_EQ(encoded[firstObjectiveOffset + 13], uint8_t{0});
    EXPECT_EQ(encoded[firstObjectiveOffset + 14], uint8_t{0});
    EXPECT_EQ(encoded[firstObjectiveOffset + 15], uint8_t{0});
    EXPECT_EQ(ReadScalarAt<int64_t>(encoded, firstObjectiveOffset + 16),
              int64_t{123456789});
}

TEST(GameStateSerialization, RoundTripRestoresEveryPublishedField)
{
    GameState state(nullptr);
    state.DeserializeGameState(BuildLegacyCompatibleFixture());

    EXPECT_EQ(state.GetPhase(), GamePhase::PostRound);
    EXPECT_EQ(state.GetMatchState(), MatchState::Finished);
    EXPECT_EQ(state.GetCurrentRound(), 7u);

    const auto& scores = state.GetTeamScores();
    ASSERT_EQ(scores.size(), 2u);
    EXPECT_EQ(scores[0].teamId, 1u);
    EXPECT_EQ(scores[0].score, 125u);
    EXPECT_EQ(scores[0].kills, 9u);
    EXPECT_EQ(scores[0].deaths, 4u);
    EXPECT_EQ(scores[0].objectivesCaptured, 3u);
    EXPECT_EQ(scores[1].teamId, 2u);
    EXPECT_EQ(scores[1].score, 88u);

    const auto& objectives = state.GetObjectives();
    ASSERT_EQ(objectives.size(), 2u);
    EXPECT_EQ(objectives[0].objectiveId, 11u);
    EXPECT_EQ(objectives[0].controllingTeam, 1u);
    EXPECT_FLOAT_EQ(objectives[0].captureProgress, 1.0f);
    EXPECT_FALSE(objectives[0].isNeutral);
    EXPECT_EQ(objectives[0].lastCaptureTime.time_since_epoch().count(),
              123456789);
    EXPECT_EQ(objectives[1].objectiveId, 12u);
    EXPECT_EQ(objectives[1].controllingTeam, 0u);
    EXPECT_FLOAT_EQ(objectives[1].captureProgress, 0.25f);
    EXPECT_TRUE(objectives[1].isNeutral);
    EXPECT_EQ(objectives[1].lastCaptureTime.time_since_epoch().count(),
              987654321);
}

TEST(GameStateSerialization, EveryTruncatedPrefixLeavesStateUnchanged)
{
    const auto fixture = BuildLegacyCompatibleFixture();
    GameState state(nullptr);
    state.DeserializeGameState(fixture);

    for (size_t length = 0; length < fixture.size(); ++length) {
        const std::vector<uint8_t> truncated(fixture.begin(),
                                             fixture.begin() + length);
        state.DeserializeGameState(truncated);
        ASSERT_EQ(state.SerializeGameState(), fixture);
    }
}

TEST(GameStateSerialization, OversizedTeamCountLeavesStateUnchanged)
{
    const auto fixture = BuildLegacyCompatibleFixture();
    GameState state(nullptr);
    state.DeserializeGameState(fixture);

    auto malformed = fixture;
    WriteScalarAt(malformed, kTeamCountOffset,
                  std::numeric_limits<uint32_t>::max());
    state.DeserializeGameState(malformed);

    ExpectStateUnchanged(state, fixture);
}

TEST(GameStateSerialization, OversizedObjectiveCountLeavesStateUnchanged)
{
    const auto fixture = BuildLegacyCompatibleFixture();
    GameState state(nullptr);
    state.DeserializeGameState(fixture);

    auto malformed = fixture;
    WriteScalarAt(malformed, kObjectiveCountOffset,
                  std::numeric_limits<uint32_t>::max());
    state.DeserializeGameState(malformed);

    ExpectStateUnchanged(state, fixture);
}

RS2V_TEST_MAIN()
