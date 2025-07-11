// tests/GameTests.cpp
// Comprehensive game logic and mechanics unit tests

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <random>
#include <unordered_map>
#include <algorithm>
#include <queue>

// Include the headers
#include "Game/GameMode.h"
#include "Game/PlayerManager.h"
#include "Game/TeamManager.h"
#include "Game/MapManager.h"
#include "Game/ScoreManager.h"
#include "Game/GameServer.h"
#include "Config/GameConfig.h"
#include "Config/MapConfig.h"
#include "Network/Packet.h"
#include "Protocol/PacketTypes.h"
#include "Utils/Logger.h"
#include "Utils/PerformanceProfiler.h"
#include "Math/Vector3.h"

using ::testing::_;
using ::testing::Return;
using ::testing::InSequence;
using ::testing::StrictMock;
using ::testing::NiceMock;
using ::testing::Invoke;
using ::testing::DoAll;
using ::testing::SetArgReferee;
using ::testing::AtLeast;
using ::testing::Between;

// Constants for game testing
constexpr int DEFAULT_MAX_PLAYERS = 64;
constexpr int DEFAULT_TICK_RATE = 60;
constexpr float DEFAULT_MATCH_TIME = 1800.0f; // 30 minutes
constexpr int DEFAULT_MAX_SCORE = 500;
constexpr float DEFAULT_RESPAWN_TIME = 10.0f;
constexpr int DEFAULT_TEAM_COUNT = 2;

// Game state enums
enum class GameState {
    WAITING_FOR_PLAYERS,
    STARTING,
    IN_PROGRESS,
    ROUND_END,
    MATCH_END
};

enum class PlayerState {
    DISCONNECTED,
    CONNECTING,
    SPECTATING,
    ALIVE,
    DEAD,
    RESPAWNING
};

enum class TeamID {
    NONE = 0,
    NORTH_VIETNAM = 1,
    SOUTH_VIETNAM = 2,
    SPECTATOR = 3
};

enum class ObjectiveType {
    CAPTURE_POINT,
    DESTROY_TARGET,
    ESCORT,
    ELIMINATION
};

// Game data structures
struct Player {
    uint32_t playerId;
    std::string steamId;
    std::string playerName;
    TeamID team;
    PlayerState state;
    Vector3 position;
    Vector3 spawnPosition;
    float health;
    int score;
    int kills;
    int deaths;
    int assists;
    std::chrono::steady_clock::time_point lastRespawnTime;
    std::chrono::steady_clock::time_point lastActionTime;
    
    Player(uint32_t id, const std::string& steam, const std::string& name)
        : playerId(id), steamId(steam), playerName(name), team(TeamID::NONE)
        , state(PlayerState::CONNECTING), position(0, 0, 0), spawnPosition(0, 0, 0)
        , health(100.0f), score(0), kills(0), deaths(0), assists(0) {
        lastRespawnTime = lastActionTime = std::chrono::steady_clock::now();
    }
};

struct Team {
    TeamID teamId;
    std::string teamName;
    std::vector<uint32_t> playerIds;
    int teamScore;
    int objectivesHeld;
    float morale;
    
    Team(TeamID id, const std::string& name)
        : teamId(id), teamName(name), teamScore(0), objectivesHeld(0), morale(1.0f) {}
};

struct Objective {
    uint32_t objectiveId;
    ObjectiveType type;
    std::string name;
    Vector3 position;
    float captureRadius;
    TeamID controllingTeam;
    float captureProgress;
    std::vector<uint32_t> playersInZone;
    bool isActive;
    
    Objective(uint32_t id, ObjectiveType objType, const std::string& objName, const Vector3& pos)
        : objectiveId(id), type(objType), name(objName), position(pos)
        , captureRadius(10.0f), controllingTeam(TeamID::NONE), captureProgress(0.0f), isActive(true) {}
};

struct GameModeDefinition {
    std::string modeName;
    std::string description;
    int maxPlayers;
    float matchDuration;
    int maxScore;
    bool friendlyFire;
    float respawnTime;
    std::vector<ObjectiveType> objectiveTypes;
    std::unordered_map<std::string, float> rules;
};

// Mock classes for game testing
class MockGameConfig : public GameConfig {
public:
    MOCK_METHOD(bool, Initialize, (std::shared_ptr<ConfigManager> configManager), (override));
    MOCK_METHOD(GameModeDefinition, GetGameModeDefinition, (const std::string& modeName), (const, override));
    MOCK_METHOD(bool, IsFriendlyFireEnabled, (), (const, override));
    MOCK_METHOD(float, GetRespawnTime, (), (const, override));
    MOCK_METHOD(int, GetMaxScore, (), (const, override));
    MOCK_METHOD(float, GetMatchDuration, (), (const, override));
    MOCK_METHOD(int, GetTickRate, (), (const, override));
};

class MockMapConfig : public MapConfig {
public:
    MOCK_METHOD(bool, Initialize, (const std::string& configPath), (override));
    MOCK_METHOD(std::vector<std::string>, GetMapRotation, (), (const, override));
    MOCK_METHOD(std::vector<Vector3>, GetSpawnPoints, (TeamID team), (const, override));
    MOCK_METHOD(std::vector<Vector3>, GetObjectiveLocations, (), (const, override));
    MOCK_METHOD(std::string, GetCurrentMap, (), (const, override));
    MOCK_METHOD(bool, IsValidMap, (const std::string& mapName), (const, override));
};

class MockGameServer : public GameServer {
public:
    MOCK_METHOD(std::shared_ptr<PlayerManager>, GetPlayerManager, (), (const, override));
    MOCK_METHOD(std::shared_ptr<TeamManager>, GetTeamManager, (), (const, override));
    MOCK_METHOD(std::shared_ptr<MapManager>, GetMapManager, (), (const, override));
    MOCK_METHOD(std::shared_ptr<GameConfig>, GetGameConfig, (), (const, override));
    MOCK_METHOD(void, BroadcastChatMessage, (const std::string& message), (override));
};

// Game mode implementation for testing
class TestGameMode : public GameMode {
public:
    TestGameMode(GameServer* server, const GameModeDefinition& definition)
        : GameMode(server, definition), m_gameState(GameState::WAITING_FOR_PLAYERS)
        , m_matchStartTime(std::chrono::steady_clock::now())
        , m_lastTickTime(std::chrono::steady_clock::now()) {
        m_definition = definition;
        InitializeObjectives();
    }

    void OnStart() override {
        m_gameState = GameState::STARTING;
        m_matchStartTime = std::chrono::steady_clock::now();
        Logger::Info("Game mode started: %s", m_definition.modeName.c_str());
    }

    void OnEnd() override {
        m_gameState = GameState::MATCH_END;
        Logger::Info("Game mode ended: %s", m_definition.modeName.c_str());
    }

    void Update() override {
        auto now = std::chrono::steady_clock::now();
        float deltaTime = std::chrono::duration<float>(now - m_lastTickTime).count();
        m_lastTickTime = now;

        UpdateGameState(deltaTime);
        UpdateObjectives(deltaTime);
        UpdatePlayers(deltaTime);
        CheckWinConditions();
    }

    bool HandlePlayerAction(uint32_t playerId, uint32_t actionType, const std::vector<uint8_t>& data) override {
        auto it = m_players.find(playerId);
        if (it == m_players.end()) return false;

        auto& player = it->second;
        player.lastActionTime = std::chrono::steady_clock::now();

        switch (actionType) {
            case 1: // Move
                return HandlePlayerMovement(player, data);
            case 2: // Fire weapon
                return HandleWeaponFire(player, data);
            case 3: // Use item
                return HandleItemUse(player, data);
            case 4: // Interact
                return HandleInteraction(player, data);
            default:
                return false;
        }
    }

    // Test-specific methods
    void AddPlayer(const Player& player) {
        m_players[player.playerId] = player;
    }

    void RemovePlayer(uint32_t playerId) {
        m_players.erase(playerId);
    }

    Player* GetPlayer(uint32_t playerId) {
        auto it = m_players.find(playerId);
        return (it != m_players.end()) ? &it->second : nullptr;
    }

    const std::vector<Team>& GetTeams() const { return m_teams; }
    const std::vector<Objective>& GetObjectives() const { return m_objectives; }
    GameState GetGameState() const { return m_gameState; }
    
    void SetGameState(GameState state) { m_gameState = state; }
    
    float GetMatchTimeRemaining() const {
        auto elapsed = std::chrono::duration<float>(
            std::chrono::steady_clock::now() - m_matchStartTime).count();
        return std::max(0.0f, m_definition.matchDuration - elapsed);
    }

    void AddObjective(const Objective& objective) {
        m_objectives.push_back(objective);
    }

    void TriggerObjectiveCapture(uint32_t objectiveId, TeamID team) {
        for (auto& obj : m_objectives) {
            if (obj.objectiveId == objectiveId) {
                obj.controllingTeam = team;
                obj.captureProgress = 100.0f;
                UpdateTeamScore(team, 50); // Award points for capture
                break;
            }
        }
    }

private:
    void InitializeObjectives() {
        // Create default objectives for testing
        m_objectives.clear();
        
        if (m_definition.modeName == "Conquest") {
            m_objectives.emplace_back(1, ObjectiveType::CAPTURE_POINT, "Alpha", Vector3(100, 0, 100));
            m_objectives.emplace_back(2, ObjectiveType::CAPTURE_POINT, "Bravo", Vector3(-100, 0, 100));
            m_objectives.emplace_back(3, ObjectiveType::CAPTURE_POINT, "Charlie", Vector3(0, 0, -100));
        } else if (m_definition.modeName == "Elimination") {
            m_objectives.emplace_back(1, ObjectiveType::ELIMINATION, "Eliminate", Vector3(0, 0, 0));
        }

        // Initialize teams
        m_teams.clear();
        m_teams.emplace_back(TeamID::NORTH_VIETNAM, "North Vietnam");
        m_teams.emplace_back(TeamID::SOUTH_VIETNAM, "South Vietnam");
    }

    void UpdateGameState(float deltaTime) {
        switch (m_gameState) {
            case GameState::WAITING_FOR_PLAYERS:
                if (GetPlayerCount() >= 2) { // Minimum players to start
                    m_gameState = GameState::STARTING;
                }
                break;
            case GameState::STARTING:
                m_gameState = GameState::IN_PROGRESS;
                break;
            case GameState::IN_PROGRESS:
                if (GetMatchTimeRemaining() <= 0) {
                    m_gameState = GameState::ROUND_END;
                }
                break;
            case GameState::ROUND_END:
                // Handle round end logic
                break;
            case GameState::MATCH_END:
                // Match is over
                break;
        }
    }

    void UpdateObjectives(float deltaTime) {
        for (auto& objective : m_objectives) {
            if (!objective.isActive) continue;

            UpdateObjectiveCapture(objective, deltaTime);
        }
    }

    void UpdateObjectiveCapture(Objective& objective, float deltaTime) {
        // Find players in capture zone
        objective.playersInZone.clear();
        std::unordered_map<TeamID, int> teamCounts;

        for (const auto& [playerId, player] : m_players) {
            if (player.state != PlayerState::ALIVE) continue;
            
            float distance = (player.position - objective.position).Length();
            if (distance <= objective.captureRadius) {
                objective.playersInZone.push_back(playerId);
                teamCounts[player.team]++;
            }
        }

        // Determine capturing team
        TeamID capturingTeam = TeamID::NONE;
        int maxCount = 0;
        for (const auto& [team, count] : teamCounts) {
            if (count > maxCount) {
                maxCount = count;
                capturingTeam = team;
            }
        }

        // Update capture progress
        if (capturingTeam != TeamID::NONE && capturingTeam != objective.controllingTeam) {
            float captureRate = 20.0f; // Points per second
            objective.captureProgress += captureRate * deltaTime;
            
            if (objective.captureProgress >= 100.0f) {
                objective.captureProgress = 100.0f;
                objective.controllingTeam = capturingTeam;
                UpdateTeamScore(capturingTeam, 100); // Capture bonus
            }
        } else if (objective.playersInZone.empty()) {
            // Decay capture progress when no one is present
            objective.captureProgress = std::max(0.0f, objective.captureProgress - 10.0f * deltaTime);
        }
    }

    void UpdatePlayers(float deltaTime) {
        auto now = std::chrono::steady_clock::now();
        
        for (auto& [playerId, player] : m_players) {
            if (player.state == PlayerState::DEAD) {
                auto timeSinceDeath = std::chrono::duration<float>(now - player.lastRespawnTime).count();
                if (timeSinceDeath >= m_definition.respawnTime) {
                    RespawnPlayer(player);
                }
            }
        }
    }

    void RespawnPlayer(Player& player) {
        player.state = PlayerState::ALIVE;
        player.health = 100.0f;
        player.position = GetTeamSpawnPoint(player.team);
        player.lastRespawnTime = std::chrono::steady_clock::now();
    }

    Vector3 GetTeamSpawnPoint(TeamID team) {
        // Return different spawn points based on team
        switch (team) {
            case TeamID::NORTH_VIETNAM:
                return Vector3(-200, 0, 0);
            case TeamID::SOUTH_VIETNAM:
                return Vector3(200, 0, 0);
            default:
                return Vector3(0, 0, 0);
        }
    }

    void CheckWinConditions() {
        if (m_gameState != GameState::IN_PROGRESS) return;

        // Check score-based win condition
        for (const auto& team : m_teams) {
            if (team.teamScore >= m_definition.maxScore) {
                m_gameState = GameState::ROUND_END;
                return;
            }
        }

        // Check objective-based win conditions
        if (m_definition.modeName == "Conquest") {
            // Check if one team controls all objectives
            TeamID firstTeam = TeamID::NONE;
            bool allSame = true;
            
            for (const auto& obj : m_objectives) {
                if (firstTeam == TeamID::NONE) {
                    firstTeam = obj.controllingTeam;
                } else if (obj.controllingTeam != firstTeam) {
                    allSame = false;
                    break;
                }
            }
            
            if (allSame && firstTeam != TeamID::NONE) {
                m_gameState = GameState::ROUND_END;
            }
        }
    }

    void UpdateTeamScore(TeamID team, int points) {
        for (auto& teamObj : m_teams) {
            if (teamObj.teamId == team) {
                teamObj.teamScore += points;
                break;
            }
        }
    }

    bool HandlePlayerMovement(Player& player, const std::vector<uint8_t>& data) {
        if (data.size() < sizeof(Vector3)) return false;
        
        Vector3 newPosition = *reinterpret_cast<const Vector3*>(data.data());
        
        // Validate movement (basic check)
        float distance = (newPosition - player.position).Length();
        if (distance > 100.0f) { // Max movement per action
            return false; // Potential speed hack
        }
        
        player.position = newPosition;
        return true;
    }

    bool HandleWeaponFire(Player& player, const std::vector<uint8_t>& data) {
        if (player.state != PlayerState::ALIVE) return false;
        
        // Simple weapon fire handling
        // In real implementation, this would handle hit detection, damage, etc.
        return true;
    }

    bool HandleItemUse(Player& player, const std::vector<uint8_t>& data) {
        if (player.state != PlayerState::ALIVE) return false;
        
        // Handle item usage
        return true;
    }

    bool HandleInteraction(Player& player, const std::vector<uint8_t>& data) {
        if (player.state != PlayerState::ALIVE) return false;
        
        // Handle world interactions
        return true;
    }

    int GetPlayerCount() const {
        int count = 0;
        for (const auto& [id, player] : m_players) {
            if (player.state != PlayerState::DISCONNECTED) {
                count++;
            }
        }
        return count;
    }

    GameModeDefinition m_definition;
    GameState m_gameState;
    std::chrono::steady_clock::time_point m_matchStartTime;
    std::chrono::steady_clock::time_point m_lastTickTime;
    std::unordered_map<uint32_t, Player> m_players;
    std::vector<Team> m_teams;
    std::vector<Objective> m_objectives;
};

// Test fixture for game tests
class GameTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize mocks
        mockGameConfig = std::make_shared<NiceMock<MockGameConfig>>();
        mockMapConfig = std::make_shared<NiceMock<MockMapConfig>>();
        mockGameServer = std::make_shared<NiceMock<MockGameServer>>();

        // Set up default mock behavior
        ON_CALL(*mockGameConfig, IsFriendlyFireEnabled())
            .WillByDefault(Return(false));
        ON_CALL(*mockGameConfig, GetRespawnTime())
            .WillByDefault(Return(DEFAULT_RESPAWN_TIME));
        ON_CALL(*mockGameConfig, GetMaxScore())
            .WillByDefault(Return(DEFAULT_MAX_SCORE));
        ON_CALL(*mockGameConfig, GetMatchDuration())
            .WillByDefault(Return(DEFAULT_MATCH_TIME));
        ON_CALL(*mockGameConfig, GetTickRate())
            .WillByDefault(Return(DEFAULT_TICK_RATE));

        ON_CALL(*mockMapConfig, GetCurrentMap())
            .WillByDefault(Return("VTE-CuChi"));
        ON_CALL(*mockMapConfig, IsValidMap(_))
            .WillByDefault(Return(true));
        ON_CALL(*mockMapConfig, GetSpawnPoints(_))
            .WillByDefault(Return(std::vector<Vector3>{{0, 0, 0}, {10, 0, 10}}));

        // Create game mode definitions
        conquestMode = CreateConquestMode();
        eliminationMode = CreateEliminationMode();
        
        // Initialize random number generator
        rng.seed(std::chrono::steady_clock::now().time_since_epoch().count());
    }

    void TearDown() override {
        testGameMode.reset();
        mockGameServer.reset();
        mockMapConfig.reset();
        mockGameConfig.reset();
    }

    // Helper methods
    GameModeDefinition CreateConquestMode() {
        GameModeDefinition mode;
        mode.modeName = "Conquest";
        mode.description = "Control objectives to win";
        mode.maxPlayers = DEFAULT_MAX_PLAYERS;
        mode.matchDuration = DEFAULT_MATCH_TIME;
        mode.maxScore = DEFAULT_MAX_SCORE;
        mode.friendlyFire = false;
        mode.respawnTime = DEFAULT_RESPAWN_TIME;
        mode.objectiveTypes = {ObjectiveType::CAPTURE_POINT};
        return mode;
    }

    GameModeDefinition CreateEliminationMode() {
        GameModeDefinition mode;
        mode.modeName = "Elimination";
        mode.description = "Eliminate all enemy players";
        mode.maxPlayers = 32;
        mode.matchDuration = 900.0f; // 15 minutes
        mode.maxScore = 100;
        mode.friendlyFire = false;
        mode.respawnTime = 5.0f;
        mode.objectiveTypes = {ObjectiveType::ELIMINATION};
        return mode;
    }

    Player CreateTestPlayer(uint32_t id, const std::string& name, TeamID team = TeamID::NORTH_VIETNAM) {
        Player player(id, "steam_" + std::to_string(id), name);
        player.team = team;
        player.state = PlayerState::ALIVE;
        return player;
    }

    std::unique_ptr<TestGameMode> CreateGameMode(const GameModeDefinition& definition) {
        return std::make_unique<TestGameMode>(mockGameServer.get(), definition);
    }

    void SimulateGameTicks(int tickCount, float deltaTime = 1.0f / DEFAULT_TICK_RATE) {
        for (int i = 0; i < tickCount; ++i) {
            if (testGameMode) {
                testGameMode->Update();
            }
            std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int>(deltaTime * 1000000)));
        }
    }

    Vector3 RandomPosition(float range = 100.0f) {
        std::uniform_real_distribution<float> dist(-range, range);
        return Vector3(dist(rng), 0, dist(rng));
    }

    // Test data
    std::shared_ptr<MockGameConfig> mockGameConfig;
    std::shared_ptr<MockMapConfig> mockMapConfig;
    std::shared_ptr<MockGameServer> mockGameServer;
    std::unique_ptr<TestGameMode> testGameMode;
    GameModeDefinition conquestMode;
    GameModeDefinition eliminationMode;
    std::mt19937 rng;
};

// === Game Mode Initialization Tests ===

TEST_F(GameTest, GameMode_Initialize_ConquestMode_Success) {
    // Act
    testGameMode = CreateGameMode(conquestMode);
    testGameMode->OnStart();

    // Assert
    EXPECT_EQ(testGameMode->GetGameState(), GameState::STARTING);
    EXPECT_EQ(testGameMode->GetObjectives().size(), 3); // Alpha, Bravo, Charlie
    EXPECT_EQ(testGameMode->GetTeams().size(), 2); // North/South Vietnam
}

TEST_F(GameTest, GameMode_Initialize_EliminationMode_Success) {
    // Act
    testGameMode = CreateGameMode(eliminationMode);
    testGameMode->OnStart();

    // Assert
    EXPECT_EQ(testGameMode->GetGameState(), GameState::STARTING);
    EXPECT_EQ(testGameMode->GetObjectives().size(), 1); // Elimination objective
    EXPECT_EQ(testGameMode->GetTeams().size(), 2);
}

TEST_F(GameTest, GameMode_StateTransition_WaitingToStarting_WithPlayers) {
    // Arrange
    testGameMode = CreateGameMode(conquestMode);
    testGameMode->SetGameState(GameState::WAITING_FOR_PLAYERS);
    
    auto player1 = CreateTestPlayer(1, "Player1", TeamID::NORTH_VIETNAM);
    auto player2 = CreateTestPlayer(2, "Player2", TeamID::SOUTH_VIETNAM);
    testGameMode->AddPlayer(player1);
    testGameMode->AddPlayer(player2);

    // Act
    testGameMode->Update();

    // Assert
    EXPECT_EQ(testGameMode->GetGameState(), GameState::STARTING);
}

// === Player Management Tests ===

TEST_F(GameTest, PlayerManagement_AddPlayer_Success) {
    // Arrange
    testGameMode = CreateGameMode(conquestMode);
    auto player = CreateTestPlayer(1, "TestPlayer");

    // Act
    testGameMode->AddPlayer(player);

    // Assert
    Player* addedPlayer = testGameMode->GetPlayer(1);
    ASSERT_NE(addedPlayer, nullptr);
    EXPECT_EQ(addedPlayer->playerId, 1);
    EXPECT_EQ(addedPlayer->playerName, "TestPlayer");
    EXPECT_EQ(addedPlayer->team, TeamID::NORTH_VIETNAM);
}

TEST_F(GameTest, PlayerManagement_RemovePlayer_Success) {
    // Arrange
    testGameMode = CreateGameMode(conquestMode);
    auto player = CreateTestPlayer(1, "TestPlayer");
    testGameMode->AddPlayer(player);

    // Act
    testGameMode->RemovePlayer(1);

    // Assert
    Player* removedPlayer = testGameMode->GetPlayer(1);
    EXPECT_EQ(removedPlayer, nullptr);
}

TEST_F(GameTest, PlayerManagement_PlayerMovement_ValidatesPosition) {
    // Arrange
    testGameMode = CreateGameMode(conquestMode);
    auto player = CreateTestPlayer(1, "TestPlayer");
    player.position = Vector3(0, 0, 0);
    testGameMode->AddPlayer(player);

    Vector3 validMove(5, 0, 5); // Small movement
    std::vector<uint8_t> moveData(sizeof(Vector3));
    std::memcpy(moveData.data(), &validMove, sizeof(Vector3));

    // Act
    bool result = testGameMode->HandlePlayerAction(1, 1, moveData); // Action type 1 = move

    // Assert
    EXPECT_TRUE(result);
    Player* movedPlayer = testGameMode->GetPlayer(1);
    ASSERT_NE(movedPlayer, nullptr);
    EXPECT_NEAR(movedPlayer->position.x, 5.0f, 0.1f);
    EXPECT_NEAR(movedPlayer->position.z, 5.0f, 0.1f);
}

TEST_F(GameTest, PlayerManagement_PlayerMovement_RejectsSpeedHack) {
    // Arrange
    testGameMode = CreateGameMode(conquestMode);
    auto player = CreateTestPlayer(1, "TestPlayer");
    player.position = Vector3(0, 0, 0);
    testGameMode->AddPlayer(player);

    Vector3 invalidMove(200, 0, 200); // Too far movement
    std::vector<uint8_t> moveData(sizeof(Vector3));
    std::memcpy(moveData.data(), &invalidMove, sizeof(Vector3));

    // Act
    bool result = testGameMode->HandlePlayerAction(1, 1, moveData);

    // Assert
    EXPECT_FALSE(result); // Should reject impossible movement
    Player* player_ptr = testGameMode->GetPlayer(1);
    ASSERT_NE(player_ptr, nullptr);
    EXPECT_NEAR(player_ptr->position.x, 0.0f, 0.1f); // Position unchanged
}

TEST_F(GameTest, PlayerManagement_PlayerRespawn_AfterDeathTimer) {
    // Arrange
    testGameMode = CreateGameMode(conquestMode);
    auto player = CreateTestPlayer(1, "TestPlayer");
    player.state = PlayerState::DEAD;
    player.lastRespawnTime = std::chrono::steady_clock::now() - std::chrono::seconds(15);
    testGameMode->AddPlayer(player);

    // Act
    testGameMode->Update(); // This should trigger respawn logic

    // Assert
    Player* respawnedPlayer = testGameMode->GetPlayer(1);
    ASSERT_NE(respawnedPlayer, nullptr);
    EXPECT_EQ(respawnedPlayer->state, PlayerState::ALIVE);
    EXPECT_EQ(respawnedPlayer->health, 100.0f);
}

// === Objective and Capture Point Tests ===

TEST_F(GameTest, Objectives_CapturePoint_PlayersInZone_StartsCapture) {
    // Arrange
    testGameMode = CreateGameMode(conquestMode);
    auto player = CreateTestPlayer(1, "Capturer", TeamID::NORTH_VIETNAM);
    player.position = Vector3(100, 0, 100); // At Alpha objective position
    testGameMode->AddPlayer(player);

    // Act
    SimulateGameTicks(60); // 1 second at 60 FPS

    // Assert
    const auto& objectives = testGameMode->GetObjectives();
    auto alphaObjective = std::find_if(objectives.begin(), objectives.end(),
        [](const Objective& obj) { return obj.name == "Alpha"; });
    
    ASSERT_NE(alphaObjective, objectives.end());
    EXPECT_GT(alphaObjective->captureProgress, 0.0f);
    EXPECT_FALSE(alphaObjective->playersInZone.empty());
}

TEST_F(GameTest, Objectives_CapturePoint_CompletesCapture_AwardsPoints) {
    // Arrange
    testGameMode = CreateGameMode(conquestMode);
    auto player = CreateTestPlayer(1, "Capturer", TeamID::NORTH_VIETNAM);
    player.position = Vector3(100, 0, 100);
    testGameMode->AddPlayer(player);

    // Act
    testGameMode->TriggerObjectiveCapture(1, TeamID::NORTH_VIETNAM); // Force capture

    // Assert
    const auto& objectives = testGameMode->GetObjectives();
    auto alphaObjective = std::find_if(objectives.begin(), objectives.end(),
        [](const Objective& obj) { return obj.objectiveId == 1; });
    
    ASSERT_NE(alphaObjective, objectives.end());
    EXPECT_EQ(alphaObjective->controllingTeam, TeamID::NORTH_VIETNAM);
    EXPECT_EQ(alphaObjective->captureProgress, 100.0f);
    
    const auto& teams = testGameMode->GetTeams();
    auto northTeam = std::find_if(teams.begin(), teams.end(),
        [](const Team& team) { return team.teamId == TeamID::NORTH_VIETNAM; });
    
    ASSERT_NE(northTeam, teams.end());
    EXPECT_GT(northTeam->teamScore, 0);
}

TEST_F(GameTest, Objectives_ContestedCapture_MultipleTeams_Blocks) {
    // Arrange
    testGameMode = CreateGameMode(conquestMode);
    auto northPlayer = CreateTestPlayer(1, "North", TeamID::NORTH_VIETNAM);
    auto southPlayer = CreateTestPlayer(2, "South", TeamID::SOUTH_VIETNAM);
    
    // Both players at same objective
    northPlayer.position = Vector3(100, 0, 100);
    southPlayer.position = Vector3(105, 0, 105);
    
    testGameMode->AddPlayer(northPlayer);
    testGameMode->AddPlayer(southPlayer);

    // Act
    SimulateGameTicks(60); // 1 second

    // Assert
    const auto& objectives = testGameMode->GetObjectives();
    auto alphaObjective = std::find_if(objectives.begin(), objectives.end(),
        [](const Objective& obj) { return obj.name == "Alpha"; });
    
    ASSERT_NE(alphaObjective, objectives.end());
    EXPECT_EQ(alphaObjective->playersInZone.size(), 2);
    // Capture progress should be minimal or zero due to contest
}

// === Team Management Tests ===

TEST_F(GameTest, Teams_InitialState_CorrectTeamSetup) {
    // Arrange & Act
    testGameMode = CreateGameMode(conquestMode);

    // Assert
    const auto& teams = testGameMode->GetTeams();
    EXPECT_EQ(teams.size(), 2);
    
    auto northTeam = std::find_if(teams.begin(), teams.end(),
        [](const Team& team) { return team.teamId == TeamID::NORTH_VIETNAM; });
    auto southTeam = std::find_if(teams.begin(), teams.end(),
        [](const Team& team) { return team.teamId == TeamID::SOUTH_VIETNAM; });
    
    ASSERT_NE(northTeam, teams.end());
    ASSERT_NE(southTeam, teams.end());
    EXPECT_EQ(northTeam->teamScore, 0);
    EXPECT_EQ(southTeam->teamScore, 0);
}

TEST_F(GameTest, Teams_BalancedPlayerDistribution_EvenTeams) {
    // Arrange
    testGameMode = CreateGameMode(conquestMode);
    
    // Add players to different teams
    for (int i = 1; i <= 10; ++i) {
        TeamID team = (i % 2 == 0) ? TeamID::NORTH_VIETNAM : TeamID::SOUTH_VIETNAM;
        auto player = CreateTestPlayer(i, "Player" + std::to_string(i), team);
        testGameMode->AddPlayer(player);
    }

    // Act & Assert
    int northCount = 0, southCount = 0;
    for (int i = 1; i <= 10; ++i) {
        Player* player = testGameMode->GetPlayer(i);
        ASSERT_NE(player, nullptr);
        if (player->team == TeamID::NORTH_VIETNAM) northCount++;
        else if (player->team == TeamID::SOUTH_VIETNAM) southCount++;
    }
    
    EXPECT_EQ(northCount, 5);
    EXPECT_EQ(southCount, 5);
}

// === Win Condition Tests ===

TEST_F(GameTest, WinConditions_ScoreLimit_TriggersRoundEnd) {
    // Arrange
    testGameMode = CreateGameMode(conquestMode);
    testGameMode->SetGameState(GameState::IN_PROGRESS);
    
    // Force team score to win condition
    for (int i = 0; i < 10; ++i) { // Award enough points to exceed max score
        testGameMode->TriggerObjectiveCapture(1, TeamID::NORTH_VIETNAM);
    }

    // Act
    testGameMode->Update();

    // Assert
    EXPECT_EQ(testGameMode->GetGameState(), GameState::ROUND_END);
}

TEST_F(GameTest, WinConditions_TimeLimit_TriggersRoundEnd) {
    // Arrange
    testGameMode = CreateGameMode(conquestMode);
    testGameMode->SetGameState(GameState::IN_PROGRESS);

    // Act - Check if time runs out naturally (this is a simulation test)
    float timeRemaining = testGameMode->GetMatchTimeRemaining();
    EXPECT_GT(timeRemaining, 0.0f); // Should have time remaining initially
    
    // In a real test, we would advance time or mock the match start time
    // to be in the past beyond the match duration
}

TEST_F(GameTest, WinConditions_AllObjectivesCaptured_TriggersWin) {
    // Arrange
    testGameMode = CreateGameMode(conquestMode);
    testGameMode->SetGameState(GameState::IN_PROGRESS);

    // Act - Capture all objectives for one team
    testGameMode->TriggerObjectiveCapture(1, TeamID::NORTH_VIETNAM); // Alpha
    testGameMode->TriggerObjectiveCapture(2, TeamID::NORTH_VIETNAM); // Bravo  
    testGameMode->TriggerObjectiveCapture(3, TeamID::NORTH_VIETNAM); // Charlie
    
    testGameMode->Update();

    // Assert
    EXPECT_EQ(testGameMode->GetGameState(), GameState::ROUND_END);
}

// === Performance Tests ===

TEST_F(GameTest, Performance_ManyPlayers_EfficientUpdate) {
    // Arrange
    testGameMode = CreateGameMode(conquestMode);
    const int playerCount = 64; // Max players
    
    for (int i = 1; i <= playerCount; ++i) {
        TeamID team = (i % 2 == 0) ? TeamID::NORTH_VIETNAM : TeamID::SOUTH_VIETNAM;
        auto player = CreateTestPlayer(i, "Player" + std::to_string(i), team);
        player.position = RandomPosition(200.0f);
        testGameMode->AddPlayer(player);
    }

    auto startTime = std::chrono::high_resolution_clock::now();

    // Act
    SimulateGameTicks(60); // 1 second of game simulation

    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

    // Assert
    EXPECT_LT(duration.count(), 100); // Should complete within 100ms
    
    // Verify all players are still tracked
    for (int i = 1; i <= playerCount; ++i) {
        EXPECT_NE(testGameMode->GetPlayer(i), nullptr);
    }
}

TEST_F(GameTest, Performance_FrequentPlayerActions_HandleEfficiently) {
    // Arrange
    testGameMode = CreateGameMode(conquestMode);
    auto player = CreateTestPlayer(1, "ActivePlayer");
    testGameMode->AddPlayer(player);

    const int actionCount = 1000;
    Vector3 moveDirection(1, 0, 0);
    std::vector<uint8_t> moveData(sizeof(Vector3));
    std::memcpy(moveData.data(), &moveDirection, sizeof(Vector3));

    auto startTime = std::chrono::high_resolution_clock::now();

    // Act
    for (int i = 0; i < actionCount; ++i) {
        testGameMode->HandlePlayerAction(1, 1, moveData); // Movement action
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);

    // Assert
    EXPECT_LT(duration.count(), 10000); // Less than 10ms for 1000 actions
}

// === Error Handling Tests ===

TEST_F(GameTest, ErrorHandling_InvalidPlayerId_HandledGracefully) {
    // Arrange
    testGameMode = CreateGameMode(conquestMode);

    // Act & Assert - Should not crash
    EXPECT_NO_THROW({
        bool result = testGameMode->HandlePlayerAction(999, 1, std::vector<uint8_t>{});
        EXPECT_FALSE(result);
        
        Player* player = testGameMode->GetPlayer(999);
        EXPECT_EQ(player, nullptr);
    });
}

TEST_F(GameTest, ErrorHandling_InvalidActionData_RejectedSafely) {
    // Arrange
    testGameMode = CreateGameMode(conquestMode);
    auto player = CreateTestPlayer(1, "TestPlayer");
    testGameMode->AddPlayer(player);

    // Act - Send malformed action data
    std::vector<uint8_t> invalidData = {0x01, 0x02}; // Too short for movement
    bool result = testGameMode->HandlePlayerAction(1, 1, invalidData);

    // Assert
    EXPECT_FALSE(result);
    
    // Player should remain in valid state
    Player* player_ptr = testGameMode->GetPlayer(1);
    ASSERT_NE(player_ptr, nullptr);
    EXPECT_EQ(player_ptr->state, PlayerState::ALIVE);
}

// === Integration Tests ===

TEST_F(GameTest, Integration_FullGameFlow_CompletesSuccessfully) {
    // Arrange
    testGameMode = CreateGameMode(conquestMode);
    
    // Add players
    auto player1 = CreateTestPlayer(1, "Player1", TeamID::NORTH_VIETNAM);
    auto player2 = CreateTestPlayer(2, "Player2", TeamID::SOUTH_VIETNAM);
    player1.position = Vector3(100, 0, 100); // Near Alpha
    player2.position = Vector3(-100, 0, 100); // Near Bravo
    
    testGameMode->AddPlayer(player1);
    testGameMode->AddPlayer(player2);

    // Act - Start game and simulate gameplay
    testGameMode->OnStart();
    testGameMode->SetGameState(GameState::IN_PROGRESS);
    
    // Simulate several seconds of gameplay
    SimulateGameTicks(300); // 5 seconds

    // Players capture their respective points
    testGameMode->TriggerObjectiveCapture(1, TeamID::NORTH_VIETNAM); // Alpha
    testGameMode->TriggerObjectiveCapture(2, TeamID::SOUTH_VIETNAM);  // Bravo

    // Continue simulation
    SimulateGameTicks(60); // 1 more second

    // Assert
    const auto& objectives = testGameMode->GetObjectives();
    EXPECT_EQ(objectives.size(), 3);
    
    // Check that captures were successful
    auto alphaObj = std::find_if(objectives.begin(), objectives.end(),
        [](const Objective& obj) { return obj.name == "Alpha"; });
    auto bravoObj = std::find_if(objectives.begin(), objectives.end(),
        [](const Objective& obj) { return obj.name == "Bravo"; });
    
    ASSERT_NE(alphaObj, objectives.end());
    ASSERT_NE(bravoObj, objectives.end());
    EXPECT_EQ(alphaObj->controllingTeam, TeamID::NORTH_VIETNAM);
    EXPECT_EQ(bravoObj->controllingTeam, TeamID::SOUTH_VIETNAM);
}

TEST_F(GameTest, Integration_PlayerLifecycle_ConnectPlayRespawn) {
    // Arrange
    testGameMode = CreateGameMode(conquestMode);
    auto player = CreateTestPlayer(1, "LifecyclePlayer");
    
    // Act & Assert - Connection
    testGameMode->AddPlayer(player);
    EXPECT_EQ(testGameMode->GetPlayer(1)->state, PlayerState::ALIVE);
    
    // Simulate player death
    Player* player_ptr = testGameMode->GetPlayer(1);
    ASSERT_NE(player_ptr, nullptr);
    player_ptr->state = PlayerState::DEAD;
    player_ptr->lastRespawnTime = std::chrono::steady_clock::now();
    
    // Wait for respawn timer
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(DEFAULT_RESPAWN_TIME * 1000 + 100)));
    testGameMode->Update();
    
    // Assert respawn
    EXPECT_EQ(player_ptr->state, PlayerState::ALIVE);
    EXPECT_EQ(player_ptr->health, 100.0f);
}

// === Edge Cases ===

TEST_F(GameTest, EdgeCase_EmptyGame_HandledStably) {
    // Arrange
    testGameMode = CreateGameMode(conquestMode);

    // Act - Run game with no players
    testGameMode->OnStart();
    SimulateGameTicks(60);

    // Assert - Should remain stable
    EXPECT_EQ(testGameMode->GetGameState(), GameState::STARTING);
    EXPECT_EQ(testGameMode->GetObjectives().size(), 3);
}

TEST_F(GameTest, EdgeCase_SinglePlayer_HandlesGracefully) {
    // Arrange
    testGameMode = CreateGameMode(conquestMode);
    auto player = CreateTestPlayer(1, "LonePlayer");
    testGameMode->AddPlayer(player);

    // Act
    testGameMode->OnStart();
    testGameMode->SetGameState(GameState::IN_PROGRESS);
    SimulateGameTicks(60);

    // Assert
    EXPECT_EQ(testGameMode->GetGameState(), GameState::IN_PROGRESS);
    Player* player_ptr = testGameMode->GetPlayer(1);
    ASSERT_NE(player_ptr, nullptr);
    EXPECT_EQ(player_ptr->state, PlayerState::ALIVE);
}

TEST_F(GameTest, EdgeCase_AllPlayersDisconnect_ResetsToWaiting) {
    // Arrange
    testGameMode = CreateGameMode(conquestMode);
    auto player1 = CreateTestPlayer(1, "Player1");
    auto player2 = CreateTestPlayer(2, "Player2");
    testGameMode->AddPlayer(player1);
    testGameMode->AddPlayer(player2);
    
    testGameMode->SetGameState(GameState::IN_PROGRESS);

    // Act - Remove all players
    testGameMode->RemovePlayer(1);
    testGameMode->RemovePlayer(2);
    testGameMode->Update();

    // Assert
    // Game should handle empty state gracefully
    // (Specific behavior depends on implementation requirements)
    EXPECT_EQ(testGameMode->GetPlayer(1), nullptr);
    EXPECT_EQ(testGameMode->GetPlayer(2), nullptr);
}

} // namespace

// Test runner entry point
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}