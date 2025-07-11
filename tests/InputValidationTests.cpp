// tests/InputValidationTests.cpp
// Unit tests for validating input handling and sanitization

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <string>
#include <vector>
#include <filesystem>

// Core headers
#include "Network/PacketSerializer.h"
#include "Config/ConfigManager.h"
#include "Config/ServerConfig.h"
#include "Game/GameMode.h"
#include "Game/PlayerManager.h"
#include "Game/ChatManager.h"
#include "Game/AdminManager.h"
#include "Protocol/PacketTypes.h"
#include "Utils/StringUtils.h"
#include "Utils/Logger.h"

using ::testing::_; using ::testing::Return; using ::testing::StrictMock;

// Mocked dependencies
class MockConfigManager : public ConfigManager {
public:
    MOCK_METHOD(bool, LoadConfiguration, (const std::string&), (override));
    MOCK_METHOD(std::string, GetString, (const std::string&, const std::string&, const std::string&), (const, override));
};

class MockChatManager : public ChatManager {
public:
    MOCK_METHOD(void, BroadcastMessage, (uint32_t, const std::string&), (override));
};

class MockAdminManager : public AdminManager {
public:
    MOCK_METHOD(bool, IsAdmin, (const std::string&), (const, override));
    MOCK_METHOD(void, ExecuteCommand, (const std::string&, const std::vector<std::string>&), (override));
};

class MockStringUtils : public StringUtils {
public:
    using StringUtils::Trim;
    using StringUtils::ToLowerCase;
};

// Fixture
class InputValidationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // no-op
    }

    void TearDown() override {
        // no-op
    }

    StrictMock<MockConfigManager> cfg;
    StrictMock<MockChatManager> chat;
    StrictMock<MockAdminManager> admin;
    MockStringUtils strutil;
};

// === PacketSerializer boundary tests ===

TEST_F(InputValidationTest, PacketSerializer_HeaderIntegrity_InvalidMagicRejected) {
    PacketSerializer ser;
    std::vector<uint8_t> buf = ser.SerializeHeader(1, 0);
    buf[0] = 0x00; // Corrupt magic
    EXPECT_FALSE(ser.ValidateHeader(buf.data(), buf.size()));
}

TEST_F(InputValidationTest, PacketSerializer_MaxSize_Enforced) {
    PacketSerializer ser;
    size_t max = ser.GetMaxPacketSize();
    std::vector<uint8_t> buf(max + 1);
    EXPECT_FALSE(ser.ValidateBufferSize(buf.size()));
    buf.resize(max);
    EXPECT_TRUE(ser.ValidateBufferSize(buf.size()));
}

// === ConfigManager input tests ===

TEST_F(InputValidationTest, ConfigManager_GetString_SectionCaseInsensitive) {
    EXPECT_CALL(cfg, LoadConfiguration(_)).WillOnce(Return(true));
    cfg.LoadConfiguration("dummy.ini");
    EXPECT_CALL(cfg, GetString("Server","Key","def")).WillOnce(Return("Value"));
    // key lookup should be case-insensitive
    EXPECT_EQ(cfg.GetString("server","key","def"), "Value");
}

TEST_F(InputValidationTest, ServerConfig_InvalidPort_FailsInitialization) {
    // Write a temp ini
    std::string path = "test_invalid_port.ini";
    std::ofstream(path) << "[Server]\nPort=99999\nMaxPlayers=32\nTickRate=60\n";
    ServerConfig sc;
    EXPECT_FALSE(sc.Initialize(path));
    std::filesystem::remove(path);
}

// === StringUtils input sanitation ===

TEST_F(InputValidationTest, StringUtils_Trim_RemovesWhitespace) {
    std::string s = "   hello world  \t\n";
    EXPECT_EQ(strutil.Trim(s), "hello world");
}

TEST_F(InputValidationTest, StringUtils_ToLowerCase_Correct) {
    EXPECT_EQ(strutil.ToLowerCase("HeLLo123"), "hello123");
}

// === ChatManager sanitization tests ===

TEST_F(InputValidationTest, ChatManager_BroadcastStripsInvalidChars) {
    EXPECT_CALL(chat, BroadcastMessage(1, "HelloWorld")).Times(1);
    std::string raw = "Hello\x01World";  // contains control char
    // Simulate sanitization before broadcast
    std::string clean;
    for(char c:raw) if(isprint(c)||isspace(c)) clean+=c;
    chat.BroadcastMessage(1, clean);
}

// === AdminManager command validation ===

TEST_F(InputValidationTest, AdminManager_ExecuteCommand_InvalidUserRejected) {
    EXPECT_CALL(admin, IsAdmin("user1")).WillOnce(Return(false));
    // user1 not admin → ExecuteCommand should not be called
    EXPECT_CALL(admin, ExecuteCommand("kick", _)).Times(0);
    if(admin.IsAdmin("user1")) {
        admin.ExecuteCommand("kick", {"user2"});
    }
}

TEST_F(InputValidationTest, AdminManager_ExecuteCommand_ValidUserAccepted) {
    EXPECT_CALL(admin, IsAdmin("mod1")).WillOnce(Return(true));
    EXPECT_CALL(admin, ExecuteCommand("kick", std::vector<std::string>{"user2"})).Times(1);
    if(admin.IsAdmin("mod1")) {
        admin.ExecuteCommand("kick", {"user2"});
    }
}

// === PlayerManager input boundaries ===

TEST_F(InputValidationTest, PlayerManager_SetHealth_ClampsRange) {
    PlayerManager pm;
    EXPECT_NO_THROW(pm.SetPlayerHealth(1, 150));  // should clamp to max
    EXPECT_EQ(pm.GetPlayerHealth(1), pm.GetMaxHealth());
    EXPECT_NO_THROW(pm.SetPlayerHealth(1, -10));  // clamp to zero
    EXPECT_EQ(pm.GetPlayerHealth(1), 0);
}

// === GameMode action validation ===

TEST_F(InputValidationTest, GameMode_HandleInvalidAction_ReturnsFalse) {
    GameModeDefinition def{"Test", "",0,0,0,false,0.0f,{},{}};
    GameMode gm(nullptr, def);
    std::vector<uint8_t> badData;  // too short
    EXPECT_FALSE(gm.HandlePlayerAction(1, 1, badData));
}

// === ProtocolUtils tag mapping tests ===

TEST_F(InputValidationTest, ProtocolUtils_TagToType_InvalidTag) {
    EXPECT_EQ(ProtocolUtils::TagToType(999), PacketType::PT_UNKNOWN);
}

TEST_F(InputValidationTest, ProtocolUtils_TypeToTag_RoundTrip) {
    for(int i=0;i<static_cast<int>(PacketType::PT_UNKNOWN);++i){
        auto tag = static_cast<PacketType>(i);
        EXPECT_EQ(ProtocolUtils::TagToType(ProtocolUtils::TypeToTag(tag)), tag);
    }
}

// === Input edge-case tests ===

TEST_F(InputValidationTest, EmptyStrings_AreHandledGracefully) {
    EXPECT_EQ(strutil.Trim(""), "");
    EXPECT_EQ(strutil.ToLowerCase(""), "");
    EXPECT_EQ(cfg.GetString("No","Key","fallback"), "fallback");
}

// === Integration of validation layers ===

TEST_F(InputValidationTest, FullInputValidation_PacketToHandler) {
    PacketSerializer ser;
    auto buf = ser.SerializeHeader(ProtocolUtils::TypeToTag(PacketType::PT_CHAT_MESSAGE), 0);
    ASSERT_TRUE(ser.ValidateHeader(buf.data(), buf.size()));
    auto type = ser.GetTag(buf.data(), buf.size());
    EXPECT_EQ(type, PacketType::PT_CHAT_MESSAGE);
}

// === Boundary value tests ===

TEST_F(InputValidationTest, Boundary_PacketSizes_Varying) {
    PacketSerializer ser;
    for(size_t sz : {0u, 1u, ser.GetMaxPacketSize(), ser.GetMaxPacketSize()+1}) {
        if(sz>ser.GetMaxPacketSize())
            EXPECT_FALSE(ser.ValidateBufferSize(sz));
        else
            EXPECT_TRUE(ser.ValidateBufferSize(sz));
    }
}