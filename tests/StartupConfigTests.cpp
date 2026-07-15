// tests/StartupConfigTests.cpp
// Focused regression coverage for command-line primary-config selection.

#include "TestFramework.h"

#include "Config/ConfigManager.h"
#include "Config/ConfigValidator.h"
#include "Config/GameConfig.h"
#include "Config/ServerConfig.h"
#include "Config/ServerNamePolicy.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

class ReentrantConfigListener final
    : public IConfigurationListener,
      public std::enable_shared_from_this<ReentrantConfigListener> {
public:
    explicit ReentrantConfigListener(ConfigManager& config) : m_config(config) {}

    void OnConfigurationChanged() override {
        observedPort.store(m_config.GetInt("Network.port", 0));
        observedName = m_config.GetString("General.server_name", "missing");
        sawNetworkKey.store(m_config.HasKey("Network.port"));
        const auto keys = m_config.GetSectionKeys("Network");
        sawNetworkSection.store(!keys.empty());
        calls.fetch_add(1);

        // Removing ourselves exercises listener-registry reentrancy. The
        // notification path must hold neither the state nor listener lock.
        m_config.RemoveConfigurationListener(shared_from_this());
    }

    std::atomic<int> calls{0};
    std::atomic<int> observedPort{0};
    std::atomic<bool> sawNetworkKey{false};
    std::atomic<bool> sawNetworkSection{false};
    std::string observedName;

private:
    ConfigManager& m_config;
};

class StartupConfigTest : public ::rs2v::Test {
protected:
    void SetUp() override {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        m_root = std::filesystem::temp_directory_path() /
                 ("rs2v_startup_config_" + std::to_string(unique));
        std::filesystem::create_directories(m_root);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(m_root, ec);
    }

    static void WriteConfig(const std::filesystem::path& path, int port) {
        std::ofstream out(path);
        out << "[General]\n"
            << "server_name=Alternate Instance\n"
            << "[Network]\n"
            << "port=" << port << "\n"
            << "[Configuration]\n"
            << "live_reload=false\n";
    }

    static void WritePolicyConfig(const std::filesystem::path& path,
                                  const std::string& serverName,
                                  int maxPlayers,
                                  int port = 17778) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "[General]\n"
            << "server_name=";
        out.write(serverName.data(),
                  static_cast<std::streamsize>(serverName.size()));
        out << "\nmax_players=" << maxPlayers << "\n"
            << "[Network]\n"
            << "port=" << port << "\n"
            << "[Configuration]\n"
            << "live_reload=false\n";
    }

    static std::string ReadFile(const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream contents;
        contents << in.rdbuf();
        return contents.str();
    }

    static void WriteLargeConfig(const std::filesystem::path& path,
                                 const std::string& serverName,
                                 int port) {
        std::ofstream out(path);
        out << "[General]\n";
        // Keep the required server_name at the end. The pre-fix loader cleared
        // live state before parsing, making the missing-value window reliably
        // observable by a concurrent reader while these entries were parsed.
        for (int i = 0; i < 25000; ++i) {
            out << "padding_" << i << "=value_" << i << "\n";
        }
        out << "server_name=" << serverName << "\n"
            << "[Network]\n"
            << "port=" << port << "\n"
            << "[Configuration]\n"
            << "live_reload=false\n";
    }

    std::filesystem::path m_root;
};

TEST_F(StartupConfigTest, InitializeLoadsTheExplicitPrimaryFile) {
    const std::filesystem::path selected = m_root / "alternate-instance.ini";
    WriteConfig(selected, 17778);

    ConfigManager config;
    ASSERT_TRUE(config.Initialize(selected.string()));
    EXPECT_EQ(config.GetInt("Network.port", 0), 17778);
}

TEST_F(StartupConfigTest, ServerNameAcceptsWirePolicyBoundaries) {
    const std::vector<std::string> validNames{
        "X",
        std::string(ServerNamePolicy::kMaxEncodedBytes, 'X'),
        "Visible Name With Internal Spaces",
    };

    std::size_t index = 0;
    for (const std::string& validName : validNames) {
        const std::filesystem::path selected =
            m_root / ("valid-server-name-" + std::to_string(index++) + ".ini");
        WritePolicyConfig(selected, validName, 64);

        ConfigManager config;
        ASSERT_TRUE(config.Initialize(selected.string()));
        EXPECT_EQ(config.GetString("General.server_name", "missing"),
                  validName);
    }
}

TEST_F(StartupConfigTest, ConfigValidatorUsesSharedServerNameWirePolicy) {
    ConfigValidator validator;
    ASSERT_TRUE(validator.Initialize());

    std::map<std::string, std::string> config{
        {"General.server_name",
         std::string(ServerNamePolicy::kMaxEncodedBytes, 'X')},
    };
    EXPECT_TRUE(validator.ValidateConfiguration(config).isValid);

    config["General.server_name"] =
        std::string(ServerNamePolicy::kMaxEncodedBytes + 1u, 'X');
    EXPECT_FALSE(validator.ValidateConfiguration(config).isValid);

    config["General.server_name"] =
        std::string("Bad") + static_cast<char>(0x80u) + "Name";
    EXPECT_FALSE(validator.ValidateConfiguration(config).isValid);
}

TEST_F(StartupConfigTest,
       ConfigValidatorFileParserMatchesConfigManagerCommentSemantics) {
    struct ParserCase {
        const char* label;
        std::string fileValue;
        std::string expectedValue;
    };
    const std::vector<ParserCase> cases{
        {"leading literal hash", "#5", "#5"},
        {"embedded literal hash", "Clan#5", "Clan#5"},
        {"leading literal semicolon", ";5", ";5"},
        {"embedded literal semicolon", "Clan;5", "Clan;5"},
        // The invalid byte proves the whitespace-preceded marker was removed
        // as a comment before the shared server-name wire policy ran.
        {"whitespace hash comment",
         std::string("Clan # ignored") + static_cast<char>(0x80u), "Clan"},
        {"whitespace semicolon comment",
         std::string("Clan ; ignored") + static_cast<char>(0x80u), "Clan"},
    };

    ConfigValidator validator;
    ASSERT_TRUE(validator.Initialize());
    std::size_t index = 0;
    for (const ParserCase& parserCase : cases) {
        const std::filesystem::path selected =
            m_root / ("server-name-comment-parity-" +
                      std::to_string(index++) + ".ini");
        WritePolicyConfig(selected, parserCase.fileValue, 64);

        ConfigManager config;
        ASSERT_TRUE(config.Initialize(selected.string())) << parserCase.label;
        EXPECT_EQ(config.GetString("General.server_name", "missing"),
                  parserCase.expectedValue) << parserCase.label;
        EXPECT_TRUE(validator.ValidateConfigurationFile(selected.string()))
            << parserCase.label;
    }
}

TEST_F(StartupConfigTest, InvalidServerNameReloadIsRejectedAtomically) {
    struct InvalidName {
        const char* label;
        std::string serverName;
    };
    const std::vector<InvalidName> invalidNames{
        {"empty", ""},
        {"oversized",
         std::string(ServerNamePolicy::kMaxEncodedBytes + 1u, 'X')},
        {"embedded-nul", std::string("Visible\0Hidden", 14u)},
        {"c0-control",
         std::string("Bad") + static_cast<char>(0x1Fu) + "Name"},
        {"del", std::string("Bad") + static_cast<char>(0x7Fu) + "Name"},
        {"non-ascii",
         std::string("Bad") + static_cast<char>(0x80u) + "Name"},
        {"spaces-only", std::string(8u, ' ')},
    };

    std::size_t index = 0;
    for (const InvalidName& invalid : invalidNames) {
        const std::filesystem::path selected =
            m_root / ("invalid-general-policy-" +
                      std::to_string(index++) + ".ini");
        WritePolicyConfig(selected, "Stable Server", 64);

        ConfigManager config;
        ASSERT_TRUE(config.Initialize(selected.string())) << invalid.label;
        WritePolicyConfig(selected, invalid.serverName, 65, 18889);

        EXPECT_FALSE(config.ReloadConfiguration()) << invalid.label;
        EXPECT_EQ(config.GetString("General.server_name", "missing"),
                  "Stable Server") << invalid.label;
        EXPECT_EQ(config.GetInt("General.max_players", 0), 64)
            << invalid.label;
        EXPECT_EQ(config.GetInt("Network.port", 0), 17778)
            << invalid.label;
    }
}

TEST_F(StartupConfigTest, MissingExplicitFileDoesNotFallBackToDefault) {
    const std::filesystem::path missing = m_root / "missing-instance.ini";

    ConfigManager config;
    EXPECT_FALSE(config.Initialize(missing.string()));
    EXPECT_FALSE(config.HasKey("Network.port"));
}

TEST_F(StartupConfigTest, ReadyPlayerGateDefaultsOnAndAcceptsExplicitOff) {
    const std::filesystem::path defaultPath = m_root / "ready-gate-default.ini";
    WriteConfig(defaultPath, 17778);

    auto defaultManager = std::make_shared<ConfigManager>();
    ASSERT_TRUE(defaultManager->Initialize(defaultPath.string()));
    ServerConfig defaultServer(defaultManager);
    GameConfig defaultGame(defaultServer);
    EXPECT_TRUE(defaultGame.WaitForReadyPlayer());

    const std::filesystem::path headlessPath = m_root / "ready-gate-off.ini";
    WriteConfig(headlessPath, 17779);
    {
        std::ofstream out(headlessPath, std::ios::app);
        out << "[Gameplay]\n"
            << "wait_for_ready_player=false\n";
    }

    auto headlessManager = std::make_shared<ConfigManager>();
    ASSERT_TRUE(headlessManager->Initialize(headlessPath.string()));
    ServerConfig headlessServer(headlessManager);
    GameConfig headlessGame(headlessServer);
    EXPECT_FALSE(headlessGame.WaitForReadyPlayer());
}

TEST_F(StartupConfigTest, MalformedReadyPlayerGateIsRejected) {
    const std::filesystem::path selected = m_root / "ready-gate-invalid.ini";
    WriteConfig(selected, 17778);
    {
        std::ofstream out(selected, std::ios::app);
        out << "[Gameplay]\n"
            << "wait_for_ready_player=eventually\n";
    }

    ConfigManager config;
    EXPECT_FALSE(config.Initialize(selected.string()));
}

TEST_F(StartupConfigTest, AntiCheatModeValidationIsCaseInsensitive) {
    const std::filesystem::path selected = m_root / "mixed-case-mode.ini";
    WriteConfig(selected, 17778);
    {
        std::ofstream out(selected, std::ios::app);
        out << "[Security]\n"
            << "enable_anti_cheat=true\n"
            << "anti_cheat_mode= OfF \n";
    }

    ConfigManager config;
    ASSERT_TRUE(config.Initialize(selected.string()));
}

TEST_F(StartupConfigTest, EacListenPortAcceptsRangeBoundaries) {
    const int validPorts[] = {1, 65535};
    for (const int listenPort : validPorts) {
        const std::filesystem::path selected =
            m_root / ("valid-eac-port-" + std::to_string(listenPort) + ".ini");
        WriteConfig(selected, 17778);
        {
            std::ofstream out(selected, std::ios::app);
            out << "[EAC]\n"
                << "listen_port=" << listenPort << "\n";
        }

        ConfigManager config;
        ASSERT_TRUE(config.Initialize(selected.string()));
        EXPECT_EQ(config.GetInt("EAC.listen_port", 0), listenPort);
    }
}

TEST_F(StartupConfigTest, InvalidEacListenPortIsRejectedStrictly) {
    const std::string invalidValues[] = {
        "-1",
        "0",
        "65536",
        "not-a-port",
        "7957junk",
        ""
    };

    std::size_t i = 0;
    for (const std::string& invalidValue : invalidValues) {
        const std::filesystem::path selected =
            m_root / ("invalid-eac-port-" + std::to_string(i) + ".ini");
        WriteConfig(selected, 17778);
        {
            std::ofstream out(selected, std::ios::app);
            out << "[EAC]\n"
                << "listen_port=" << invalidValue << "\n";
        }

        ConfigManager config;
        EXPECT_FALSE(config.Initialize(selected.string()));
        ++i;
    }
}

TEST_F(StartupConfigTest, DestroyingReadOnlyManagerDoesNotRewritePrimaryFile) {
    const std::filesystem::path selected = m_root / "read-only.ini";
    WriteConfig(selected, 17778);
    const std::string original = ReadFile(selected);

    {
        ConfigManager config;
        ASSERT_TRUE(config.Initialize(selected.string()));
        EXPECT_EQ(config.GetInt("Network.port", 0), 17778);
    }

    EXPECT_EQ(ReadFile(selected), original);
}

TEST_F(StartupConfigTest, UnsavedMutationIsNotPersistedOnDestruction) {
    const std::filesystem::path selected = m_root / "unsaved.ini";
    WriteConfig(selected, 17778);
    const std::string original = ReadFile(selected);

    {
        ConfigManager config;
        ASSERT_TRUE(config.Initialize(selected.string()));
        config.SetInt("Network.port", 18888);
        EXPECT_EQ(config.GetInt("Network.port", 0), 18888);
    }

    EXPECT_EQ(ReadFile(selected), original);
}

TEST_F(StartupConfigTest, ExplicitSavePersistsIntentionalMutation) {
    const std::filesystem::path selected = m_root / "explicit-save.ini";
    WriteConfig(selected, 17778);

    {
        ConfigManager config;
        ASSERT_TRUE(config.Initialize(selected.string()));
        config.SetInt("Network.port", 18888);
        ASSERT_TRUE(config.SaveAllConfigurations());
    }

    EXPECT_TRUE(ReadFile(selected).find("port=18888") != std::string::npos);
}

TEST_F(StartupConfigTest, AutoSavePersistsIntentionalMutation) {
    const std::filesystem::path selected = m_root / "auto-save.ini";
    WriteConfig(selected, 17778);

    {
        ConfigManager config;
        ASSERT_TRUE(config.Initialize(selected.string()));
        config.SetAutoSave(true);
        config.SetInt("Network.port", 18889);
    }

    EXPECT_TRUE(ReadFile(selected).find("port=18889") != std::string::npos);
}

TEST_F(StartupConfigTest, AutoSavePersistsKeyRemoval) {
    const std::filesystem::path selected = m_root / "auto-save-remove.ini";
    WriteConfig(selected, 17778);

    {
        ConfigManager config;
        ASSERT_TRUE(config.Initialize(selected.string()));
        config.SetAutoSave(true);
        config.RemoveKey("Network.port");
    }

    EXPECT_TRUE(ReadFile(selected).find("port=") == std::string::npos);
}

TEST_F(StartupConfigTest, FileWatcherCanStopAndRestartCleanly) {
    const std::filesystem::path selected = m_root / "watcher-restart.ini";
    WriteConfig(selected, 17778); // live reload is disabled; start it explicitly.

    ConfigManager config;
    ASSERT_TRUE(config.Initialize(selected.string()));
    EXPECT_FALSE(config.IsFileWatcherRunning());

    ASSERT_TRUE(config.StartFileWatcher());
    EXPECT_TRUE(config.IsFileWatcherRunning());
    config.StopFileWatcher();
    EXPECT_FALSE(config.IsFileWatcherRunning());

    ASSERT_TRUE(config.StartFileWatcher());
    EXPECT_TRUE(config.IsFileWatcherRunning());
    config.StopFileWatcher();
    EXPECT_FALSE(config.IsFileWatcherRunning());
}

TEST_F(StartupConfigTest, FailedReloadPreservesTheLastValidState) {
    const std::filesystem::path selected = m_root / "failed-reload.ini";
    WriteConfig(selected, 17778);

    ConfigManager config;
    ASSERT_TRUE(config.Initialize(selected.string()));

    {
        std::ofstream out(selected, std::ios::trunc);
        out << "[General]\n"
            << "server_name=\n"
            << "[Network]\n"
            << "port=80\n"
            << "[Configuration]\n"
            << "live_reload=false\n";
    }

    EXPECT_FALSE(config.ReloadConfiguration());
    EXPECT_EQ(config.GetString("General.server_name", "missing"),
              std::string("Alternate Instance"));
    EXPECT_EQ(config.GetInt("Network.port", 0), 17778);
}

TEST_F(StartupConfigTest, ReloadListenerCanReadAndRemoveItselfReentrantly) {
    const std::filesystem::path selected = m_root / "reentrant-listener.ini";
    WriteConfig(selected, 17778);

    ConfigManager config;
    ASSERT_TRUE(config.Initialize(selected.string()));
    auto listener = std::make_shared<ReentrantConfigListener>(config);
    config.AddConfigurationListener(listener);

    WriteConfig(selected, 18890);
    ASSERT_TRUE(config.ReloadConfiguration());
    EXPECT_EQ(listener->calls.load(), 1);
    EXPECT_EQ(listener->observedPort.load(), 18890);
    EXPECT_EQ(listener->observedName, std::string("Alternate Instance"));
    EXPECT_TRUE(listener->sawNetworkKey.load());
    EXPECT_TRUE(listener->sawNetworkSection.load());

    WriteConfig(selected, 18891);
    ASSERT_TRUE(config.ReloadConfiguration());
    EXPECT_EQ(listener->calls.load(), 1);
}

TEST_F(StartupConfigTest, ConcurrentReadersNeverObservePartiallyParsedState) {
    const std::filesystem::path first = m_root / "large-first.ini";
    const std::filesystem::path second = m_root / "large-second.ini";
    WriteLargeConfig(first, "Generation A", 17778);
    WriteLargeConfig(second, "Generation B", 17779);

    ConfigManager config;
    ASSERT_TRUE(config.Initialize(first.string()));

    std::atomic<bool> finished{false};
    std::atomic<int> invalidReads{0};
    std::atomic<std::uint64_t> samples{0};
    std::thread reader([&] {
        while (!finished.load()) {
            const std::string name =
                config.GetString("General.server_name", "__missing__");
            if (name != "Generation A" && name != "Generation B") {
                invalidReads.fetch_add(1);
            }
            samples.fetch_add(1);
        }
    });

    while (samples.load() < 1000) {
        std::this_thread::yield();
    }

    bool allLoadsSucceeded = true;
    for (int i = 0; i < 4; ++i) {
        allLoadsSucceeded &= config.LoadConfiguration(
            (i % 2 == 0 ? second : first).string());
    }

    finished.store(true);
    reader.join();
    EXPECT_TRUE(allLoadsSucceeded);
    EXPECT_EQ(invalidReads.load(), 0);
    EXPECT_GT(samples.load(), static_cast<std::uint64_t>(1000));
}

TEST_F(StartupConfigTest, StopFileWatcherWakesPromptly) {
    const std::filesystem::path selected = m_root / "watcher-prompt-stop.ini";
    WriteConfig(selected, 17778);

    ConfigManager config;
    ASSERT_TRUE(config.Initialize(selected.string()));
    ASSERT_TRUE(config.StartFileWatcher());

    // Let the watcher enter its timed wait; Stop must signal that wait instead
    // of waiting for the old two-second polling sleep to expire.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto start = std::chrono::steady_clock::now();
    config.StopFileWatcher();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    EXPECT_LT(elapsed.count(), static_cast<long long>(1000));
    EXPECT_FALSE(config.IsFileWatcherRunning());
}

} // namespace

RS2V_TEST_MAIN()
