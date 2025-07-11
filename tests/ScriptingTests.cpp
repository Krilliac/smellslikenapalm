// tests/ScriptingTests.cpp
// Comprehensive unit tests for the embedded scripting subsystem
//
// Covers:
// 1. Script engine initialization and shutdown.
// 2. Loading, compiling, and executing scripts.
// 3. Binding game API functions and variables.
// 4. Error handling: syntax errors, runtime exceptions.
// 5. Security sandboxing: restricted APIs, execution timeouts.
// 6. Persistence: saving/loading script state.
// 7. Edge cases: empty scripts, infinite loops, concurrent script contexts.

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <string>
#include <chrono>
#include <thread>

#include "Scripting/ScriptEngine.h"
#include "Game/GameServer.h"
#include "Utils/Logger.h"

using ::testing::_;

// Mock GameServer to expose API to scripts
class MockGameServer : public GameServer {
public:
    MOCK_METHOD(void, BroadcastChatMessage, (const std::string&), (override));
    MOCK_METHOD(int, GetServerTick, (), (const, override));
};

// Fixture for scripting tests
class ScriptingTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine = std::make_unique<ScriptEngine>();
        mockServer = std::make_shared<MockGameServer>();
        engine->SetGameServer(mockServer.get());
        ASSERT_TRUE(engine->Initialize());
    }

    void TearDown() override {
        engine->Shutdown();
        engine.reset();
        mockServer.reset();
    }

    std::unique_ptr<ScriptEngine> engine;
    std::shared_ptr<MockGameServer> mockServer;
};

// 1. Initialization and shutdown
TEST_F(ScriptingTest, InitializeShutdown_Succeeds) {
    EXPECT_TRUE(engine->IsRunning());
    engine->Shutdown();
    EXPECT_FALSE(engine->IsRunning());
}

// 2. Load, compile, and execute a simple script
TEST_F(ScriptingTest, SimpleScript_ExecutesCorrectly) {
    const std::string script = R"(
        function OnServerStart()
            BroadcastChat("Server started")
        end
        OnServerStart()
    )";
    EXPECT_CALL(*mockServer, BroadcastChatMessage("Server started")).Times(1);
    ASSERT_TRUE(engine->LoadScript("startup", script));
    EXPECT_TRUE(engine->Execute("startup"));
}

// 3. Syntax error handling
TEST_F(ScriptingTest, ScriptSyntaxError_ReportsFailure) {
    const std::string badScript = "function() syntax error";
    EXPECT_FALSE(engine->LoadScript("bad", badScript));
    auto error = engine->GetLastError();
    EXPECT_NE(error.find("syntax error"), std::string::npos);
}

// 4. Runtime exception handling
TEST_F(ScriptingTest, ScriptRuntimeError_ThrowsExceptionAtExec) {
    const std::string script = R"(
        function Test()
            error("Runtime failure")
        end
        Test()
    )";
    ASSERT_TRUE(engine->LoadScript("runtime", script));
    EXPECT_FALSE(engine->Execute("runtime"));
    auto error = engine->GetLastError();
    EXPECT_NE(error.find("Runtime failure"), std::string::npos);
}

// 5. API sandboxing: restrict file IO
TEST_F(ScriptingTest, Sandbox_DisallowedFunctionCall) {
    const std::string script = R"(
        function Test()
            io.open("secret.txt", "r")
        end
        Test()
    )";
    ASSERT_TRUE(engine->LoadScript("sandbox", script));
    EXPECT_FALSE(engine->Execute("sandbox"));
    auto error = engine->GetLastError();
    EXPECT_NE(error.find("attempt to call a nil value"), std::string::npos);
}

// 6. Execution timeout for infinite loops
TEST_F(ScriptingTest, Timeout_InfiniteLoop_Terminates) {
    engine->SetTimeoutMs(100);
    const std::string script = R"(
        function Test()
            while true do end
        end
        Test()
    )";
    ASSERT_TRUE(engine->LoadScript("loop", script));
    auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(engine->Execute("loop"));
    auto end = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    EXPECT_LT(elapsed, 500);  // should abort quickly
    auto error = engine->GetLastError();
    EXPECT_NE(error.find("timeout"), std::string::npos);
}

// 7. State persistence between executes
TEST_F(ScriptingTest, StatePersistence_VariablesRetained) {
    const std::string script = R"(
        counter = counter or 0
        function Increment()
            counter = counter + 1
        end
        Increment()
    )";
    ASSERT_TRUE(engine->LoadScript("state", script));
    EXPECT_TRUE(engine->Execute("state"));
    int val1 = engine->GetGlobal<int>("counter");
    EXPECT_EQ(val1, 1);
    EXPECT_TRUE(engine->Execute("state"));
    int val2 = engine->GetGlobal<int>("counter");
    EXPECT_EQ(val2, 2);
}

// 8. Concurrent script contexts isolation
TEST_F(ScriptingTest, ConcurrentContexts_Isolated) {
    auto engine2 = std::make_unique<ScriptEngine>();
    engine2->SetGameServer(mockServer.get());
    ASSERT_TRUE(engine2->Initialize());

    const std::string script = R"(
        x = x or 0
        function Inc()
            x = x + 1
        end
        Inc()
    )";
    ASSERT_TRUE(engine->LoadScript("ctx1", script));
    ASSERT_TRUE(engine2->LoadScript("ctx2", script));
    EXPECT_TRUE(engine->Execute("ctx1"));
    EXPECT_TRUE(engine2->Execute("ctx2"));
    EXPECT_TRUE(engine->Execute("ctx1"));
    EXPECT_TRUE(engine2->Execute("ctx2"));
    EXPECT_EQ(engine->GetGlobal<int>("x"), 2);
    EXPECT_EQ(engine2->GetGlobal<int>("x"), 2);

    engine2->Shutdown();
}

// 9. Edge: empty script load
TEST_F(ScriptingTest, EmptyScript_LoadAndExecuteNoOp) {
    ASSERT_TRUE(engine->LoadScript("empty", ""));
    EXPECT_TRUE(engine->Execute("empty"));
}

// 10. Performance: many small scripts
TEST_F(ScriptingTest, Performance_MultipleScripts_CompileAndExec) {
    const int N = 1000;
    std::vector<std::string> names;
    for (int i = 0; i < N; ++i) {
        names.push_back("script" + std::to_string(i));
        std::string s = "function f() end";
        engine->LoadScript(names.back(), s);
    }
    auto start = std::chrono::high_resolution_clock::now();
    for (auto& name : names) {
        engine->Execute(name);
    }
    auto end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - start).count();
    EXPECT_LT(ms, 200.0);  // 1000 executes <200ms
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}