// tests/SteamQueryTests.cpp
// Comprehensive unit tests for SteamQuery subsystem
//
// Covers:
// 1. Steamworks initialization and shutdown.
// 2. Querying Steam user stats: achievements, stats retrieval.
// 3. Leaderboard find/create and score upload/download.
// 4. Async callback handling and timeout.
// 5. Error handling: Steam API not available, invalid handles.
// 6. Edge cases: zero-length names, nonexistent leaderboards, stale callbacks.

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <thread>
#include <chrono>
#include "Steam/SteamQuery.h"
#include "Utils/Logger.h"

using ::testing::_;
using ::testing::Return;
using ::testing::Invoke;

// Mock SteamAPI wrapper
class MockSteamAPI : public ISteamAPI {
public:
    MOCK_METHOD(bool, Initialize, (), (override));
    MOCK_METHOD(void, Shutdown, (), (override));
    MOCK_METHOD(bool, GetUserStat, (const std::string& name, int32_t& data), (override));
    MOCK_METHOD(bool, SetUserStat, (const std::string& name, int32_t data), (override));
    MOCK_METHOD(bool, StoreStats, (), (override));
    MOCK_METHOD(SteamLeaderboard_t, FindOrCreateLeaderboard, (const std::string& name), (override));
    MOCK_METHOD(bool, UploadLeaderboardScore, (SteamLeaderboard_t lb, int32_t score), (override));
    MOCK_METHOD(bool, DownloadLeaderboardEntries, (SteamLeaderboard_t lb, std::vector<int32_t>& scores), (override));
    MOCK_METHOD(bool, IsAPICallCompleted, (SteamAPICall_t call), (override));
    MOCK_METHOD(void, ReleaseAPICall, (SteamAPICall_t call), (override));
};

// Fixture
class SteamQueryTest : public ::testing::Test {
protected:
    void SetUp() override {
        mockAPI = std::make_shared<MockSteamAPI>();
        sq = std::make_unique<SteamQuery>(mockAPI);
    }

    void TearDown() override {
        sq.reset();
        mockAPI.reset();
    }

    std::shared_ptr<MockSteamAPI> mockAPI;
    std::unique_ptr<SteamQuery> sq;
};

// 1. Initialization and shutdown
TEST_F(SteamQueryTest, InitializeShutdown_Succeeds) {
    EXPECT_CALL(*mockAPI, Initialize()).WillOnce(Return(true));
    EXPECT_TRUE(sq->Initialize());
    EXPECT_CALL(*mockAPI, Shutdown()).Times(1);
    sq->Shutdown();
}

// 2. Get and set user stats success
TEST_F(SteamQueryTest, GetSetUserStat_Succeeds) {
    int32_t value = 0;
    EXPECT_CALL(*mockAPI, GetUserStat("Kills", _))
        .WillOnce(DoAll(::testing::SetArgReferee<1>(150), Return(true)));
    EXPECT_TRUE(sq->GetStat("Kills", value));
    EXPECT_EQ(value, 150);

    EXPECT_CALL(*mockAPI, SetUserStat("Kills", 200)).WillOnce(Return(true));
    EXPECT_CALL(*mockAPI, StoreStats()).WillOnce(Return(true));
    EXPECT_TRUE(sq->SetStat("Kills", 200));
}

// 3. Get user stat failure
TEST_F(SteamQueryTest, GetUserStat_NotFound_Fails) {
    int32_t value = 0;
    EXPECT_CALL(*mockAPI, GetUserStat("Unknown", _)).WillOnce(Return(false));
    EXPECT_FALSE(sq->GetStat("Unknown", value));
}

// 4. Leaderboard find/create success
TEST_F(SteamQueryTest, FindOrCreateLeaderboard_Succeeds) {
    SteamLeaderboard_t lb = 12345;
    EXPECT_CALL(*mockAPI, FindOrCreateLeaderboard("TopScores"))
        .WillOnce(Return(lb));
    auto result = sq->GetOrCreateLeaderboard("TopScores");
    EXPECT_EQ(result, lb);
}

// 5. Upload leaderboard score
TEST_F(SteamQueryTest, UploadLeaderboardScore_Succeeds) {
    SteamLeaderboard_t lb = 111;
    EXPECT_CALL(*mockAPI, UploadLeaderboardScore(lb, 9999)).WillOnce(Return(true));
    EXPECT_TRUE(sq->UploadScore(lb, 9999));
}

// 6. Download leaderboard entries
TEST_F(SteamQueryTest, DownloadLeaderboardEntries_Succeeds) {
    SteamLeaderboard_t lb = 222;
    std::vector<int32_t> scores = {100, 200, 300};
    EXPECT_CALL(*mockAPI, DownloadLeaderboardEntries(lb, _))
        .WillOnce(DoAll(Invoke([&](SteamLeaderboard_t, std::vector<int32_t>& out){
            out = scores;
            return true;
        })));
    auto result = sq->DownloadScores(lb);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), scores);
}

// 7. Async callback handling with timeout
TEST_F(SteamQueryTest, APICall_Timeout) {
    SteamAPICall_t call = 999;
    EXPECT_CALL(*mockAPI, IsAPICallCompleted(call))
        .WillOnce(Return(false));
    auto start = std::chrono::steady_clock::now();
    bool completed = sq->WaitForAPICall(call, 100); // 100ms timeout
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    EXPECT_FALSE(completed);
    EXPECT_GE(elapsed, 100);
}

// 8. Release API call
TEST_F(SteamQueryTest, ReleaseAPICall_Called) {
    SteamAPICall_t call = 555;
    EXPECT_CALL(*mockAPI, IsAPICallCompleted(call))
        .WillOnce(Return(true));
    EXPECT_CALL(*mockAPI, ReleaseAPICall(call)).Times(1);
    sq->WaitForAPICall(call, 1000);
}

// 9. Edge: zero-length leaderboard name
TEST_F(SteamQueryTest, EmptyLeaderboardName_ReturnsInvalid) {
    auto lb = sq->GetOrCreateLeaderboard("");
    EXPECT_EQ(lb, k_InvalidLeaderboard);
}

// 10. Edge: invalid Steam API not available
TEST_F(SteamQueryTest, APIUnavailable_FailsAll) {
    EXPECT_CALL(*mockAPI, Initialize()).WillOnce(Return(false));
    EXPECT_FALSE(sq->Initialize());
    int32_t val;
    EXPECT_FALSE(sq->GetStat("Kills", val));
    EXPECT_EQ(sq->GetOrCreateLeaderboard("A"), k_InvalidLeaderboard);
    EXPECT_FALSE(sq->UploadScore(1,1));
    EXPECT_FALSE(sq->DownloadScores(1).has_value());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}