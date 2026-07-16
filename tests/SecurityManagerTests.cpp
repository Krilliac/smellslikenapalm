// Focused SecurityManager startup coverage.

#include "TestFramework.h"

#include "Config/ConfigManager.h"
#include "Config/SecurityConfig.h"
#include "Config/ServerConfig.h"
#include "Network/SocketFactory.h"
#include "Network/ClientConnection.h"
#include "Network/UDPSocket.h"
#include "Security/Authentication.h"
#include "Security/EACServerEmulator.h"
#include "Security/SecurityManager.h"

#include <chrono>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace {

class SecurityManagerTest : public ::rs2v::Test {
protected:
    void SetUp() override {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        m_root = std::filesystem::temp_directory_path() /
                 ("rs2v_security_manager_" + std::to_string(unique));
        std::filesystem::create_directories(m_root);
        m_banList = m_root / "bans.txt";
        std::ofstream(m_banList).close();
        ASSERT_TRUE(SocketFactory::Initialize());
    }

    void TearDown() override {
        SocketFactory::Shutdown();
        std::error_code ec;
        std::filesystem::remove_all(m_root, ec);
    }

    std::shared_ptr<SecurityConfig> MakeConfig(bool enabled,
                                               const std::string& mode,
                                               std::optional<int> listenPort = std::nullopt) const {
        auto values = std::make_shared<ConfigManager>();
        values->SetBool("Security.enable_anti_cheat", enabled);
        values->SetString("Security.anti_cheat_mode", mode);
        if (listenPort.has_value()) {
            values->SetInt("EAC.listen_port", *listenPort);
        }
        values->SetString("Security.ban_list_file", m_banList.generic_string());
        values->SetBool("Security.enable_ban_manager", true);
        ServerConfig serverConfig(values);
        return std::make_shared<SecurityConfig>(serverConfig);
    }

    uint16_t ReserveAvailableUdpPort(UDPSocket& reservation) const {
        constexpr uint32_t kFirstPort = 20000;
        constexpr uint32_t kPortCount = 40000;
        const auto seed = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const uint32_t start = static_cast<uint32_t>(seed % kPortCount);

        for (uint32_t offset = 0; offset < 4096; ++offset) {
            const auto candidate = static_cast<uint16_t>(
                kFirstPort + ((start + offset) % kPortCount));
            if (reservation.Bind(candidate)) {
                return candidate;
            }
        }
        return 0;
    }

    uint16_t FindAvailableUdpPort() const {
        UDPSocket reservation;
        const uint16_t port = ReserveAvailableUdpPort(reservation);
        reservation.Close();
        return port;
    }

    std::filesystem::path m_root;
    std::filesystem::path m_banList;
};

TEST_F(SecurityManagerTest, DisabledFlagSkipsEacPortInitialization) {
    UDPSocket eacPortReservation;
    const uint16_t eacPort = ReserveAvailableUdpPort(eacPortReservation);
    ASSERT_TRUE(eacPort != 0);

    SecurityManager security(MakeConfig(false, "emulate", eacPort));
    ASSERT_TRUE(security.Initialize());
    EXPECT_TRUE(security.IsInitialized());
    EXPECT_FALSE(security.IsEACInitialized());
    security.Update();
}

TEST_F(SecurityManagerTest, OffModeSkipsEacPortInitialization) {
    UDPSocket eacPortReservation;
    const uint16_t eacPort = ReserveAvailableUdpPort(eacPortReservation);
    ASSERT_TRUE(eacPort != 0);

    SecurityManager security(MakeConfig(true, "off", eacPort));
    ASSERT_TRUE(security.Initialize());
    EXPECT_TRUE(security.IsInitialized());
    EXPECT_FALSE(security.IsEACInitialized());
    security.Update();
}

TEST_F(SecurityManagerTest, OffModeTrimsWhitespaceAndIgnoresCase) {
    UDPSocket eacPortReservation;
    const uint16_t eacPort = ReserveAvailableUdpPort(eacPortReservation);
    ASSERT_TRUE(eacPort != 0);

    SecurityManager security(MakeConfig(true, " \tOfF \r\n", eacPort));
    ASSERT_TRUE(security.Initialize());
    EXPECT_FALSE(security.IsEACInitialized());
}

TEST_F(SecurityManagerTest, MissingEacPortUsesLegacyDefault) {
    const auto config = MakeConfig(false, "off");
    EXPECT_EQ(config->GetEACListenPort(),
              SecurityConfig::kDefaultEACListenPort);
    EXPECT_TRUE(SecurityConfig::IsValidEACListenPort(
        config->GetEACListenPort()));
}

TEST_F(SecurityManagerTest, ConfiguredEacPortIsBoundAndReleased) {
    const uint16_t eacPort = FindAvailableUdpPort();
    ASSERT_TRUE(eacPort != 0);

    const auto config = MakeConfig(true, "emulate", eacPort);
    EXPECT_EQ(config->GetEACListenPort(), static_cast<int>(eacPort));

    SecurityManager security(config);
    ASSERT_TRUE(security.Initialize());
    ASSERT_TRUE(security.IsEACInitialized());

    UDPSocket collision;
    EXPECT_FALSE(collision.Bind(eacPort));

    security.Shutdown();
    EXPECT_FALSE(security.IsEACInitialized());

    UDPSocket released;
    EXPECT_TRUE(released.Bind(eacPort));
}

TEST_F(SecurityManagerTest, InvalidConfiguredEacPortsFailBeforeInitialization) {
    const int invalidPorts[] = {
        -1,
        0,
        65536,
        std::numeric_limits<int>::max()
    };

    for (const int invalidPort : invalidPorts) {
        EXPECT_FALSE(SecurityConfig::IsValidEACListenPort(invalidPort));
        SecurityManager security(MakeConfig(true, "emulate", invalidPort));
        EXPECT_FALSE(security.Initialize());
        EXPECT_FALSE(security.IsInitialized());
        EXPECT_FALSE(security.IsEACInitialized());
    }
}

TEST_F(SecurityManagerTest, EacEmulatorRejectsZeroInsteadOfBindingEphemeralPort) {
    EACServerEmulator emulator;
    EXPECT_FALSE(emulator.Initialize(0));
    emulator.Shutdown();
}

TEST_F(SecurityManagerTest, InitializeAndShutdownAreIdempotentAndReusable) {
    SecurityManager security(MakeConfig(true, "off"));

    ASSERT_TRUE(security.Initialize());
    ASSERT_TRUE(security.Initialize());
    EXPECT_TRUE(security.IsInitialized());
    EXPECT_FALSE(security.IsEACInitialized());

    security.Shutdown();
    security.Shutdown();
    EXPECT_FALSE(security.IsInitialized());
    EXPECT_FALSE(security.IsEACInitialized());

    ASSERT_TRUE(security.Initialize());
    EXPECT_TRUE(security.IsInitialized());
    EXPECT_FALSE(security.IsEACInitialized());
    security.Update();
}

TEST_F(SecurityManagerTest, NullConnectionIsIgnoredSafely) {
    SecurityManager security(MakeConfig(true, "off"));
    ASSERT_TRUE(security.Initialize());

    security.OnClientConnect(nullptr);

    EXPECT_TRUE(security.IsInitialized());
    EXPECT_FALSE(security.ValidatePacket(999u, {}));
}

TEST_F(SecurityManagerTest, UE3ClientNeverCreatesLegacyPacketAuthSession) {
    const auto config = MakeConfig(true, "off");
    Authentication authentication(config);
    EXPECT_FALSE(authentication.SendChallenge(nullptr));

    auto connection = std::make_shared<ClientConnection>(
        77u, "127.0.0.1", uint16_t{7777}, nullptr, nullptr);
    connection->SetUE3Client(true);

    ASSERT_TRUE(authentication.SendChallenge(connection));
    // No legacy session was created, so a response cannot be accepted. Retail
    // authenticates through the UE3 control channel instead.
    EXPECT_EQ(authentication.ValidateResponse(connection, "not-applicable"),
              AuthResult::Error);
}

TEST_F(SecurityManagerTest, AuthenticationRejectsNullAndFailedChallengeSessions) {
    Authentication configured(MakeConfig(true, "off"));
    EXPECT_EQ(configured.ValidateResponse(nullptr, "response"),
              AuthResult::Error);

    auto noSocketConnection = std::make_shared<ClientConnection>(
        78u, "127.0.0.1", uint16_t{7778}, nullptr, nullptr);
    EXPECT_FALSE(configured.SendChallenge(noSocketConnection));
    EXPECT_EQ(configured.ValidateResponse(noSocketConnection, "response"),
              AuthResult::Error);
}

TEST_F(SecurityManagerTest, MissingAuthenticationConfigFailsClosed) {
    auto socket = std::make_shared<UDPSocket>();
    ASSERT_TRUE(socket->Bind(0));
    auto connection = std::make_shared<ClientConnection>(
        79u, "127.0.0.1", uint16_t{7779}, socket, nullptr);

    Authentication missingConfig(nullptr);
    ASSERT_TRUE(missingConfig.SendChallenge(connection));
    EXPECT_EQ(missingConfig.ValidateResponse(connection, "response"),
              AuthResult::ServiceUnavailable);
}

TEST_F(SecurityManagerTest, PacketValidationIsSafeAcrossConcurrentDisconnect) {
    SecurityManager security(MakeConfig(true, "off"));
    ASSERT_TRUE(security.Initialize());

    auto connection = std::make_shared<ClientConnection>(
        80u, "127.0.0.1", uint16_t{7780}, nullptr, nullptr);
    connection->SetUE3Client(true);
    security.OnClientConnect(connection);
    EXPECT_TRUE(security.ValidatePacket(80u, {}));

    std::atomic<bool> start{false};
    std::thread validator([&]() {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (int i = 0; i < 128; ++i) {
            (void)security.ValidatePacket(80u, {});
        }
    });
    start.store(true, std::memory_order_release);
    security.OnClientDisconnect(80u);
    validator.join();

    EXPECT_FALSE(security.ValidatePacket(80u, {}));
}

} // namespace

RS2V_TEST_MAIN()
