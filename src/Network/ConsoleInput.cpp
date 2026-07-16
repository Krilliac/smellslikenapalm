// src/Network/ConsoleInput.cpp — stdin command reader.

#include "Network/ConsoleInput.h"

#include "Game/GameServer.h"
#include "Game/CommandManager.h"
#include "Utils/Logger.h"
#include "Utils/StringUtils.h"
#include "Utils/CrashHandler.h"

#include <iostream>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <string>
#include <utility>

#ifndef _WIN32
#include <sys/select.h>
#include <unistd.h>
#endif

struct ConsoleInput::State {
    explicit State(GameServer* initialServer) : server(initialServer) {}

    std::atomic<bool> running{false};
    std::mutex serverMutex;
    GameServer* server = nullptr; // guarded by serverMutex
};

ConsoleInput::ConsoleInput(GameServer* server)
    : m_state(std::make_shared<State>(server))
{
    Logger::Trace("[ConsoleInput::ConsoleInput] Entry");
}

ConsoleInput::~ConsoleInput()
{
    Stop();
}

void ConsoleInput::Start()
{
    const std::shared_ptr<State> state = m_state;
    if (!state || state->running.exchange(true)) return;
    Logger::Info("[ConsoleInput] Console command input ready (type 'help')");
    m_thread = std::thread([state] { ReadLoop(state); });
}

void ConsoleInput::Stop()
{
    // Always reach the join: the reader thread may have already self-exited on
    // EOF (clearing m_running itself), but it is still joinable. Destroying a
    // joinable std::thread calls std::terminate, so a guard that skipped the
    // join here would abort the process at shutdown.
    const std::shared_ptr<State> state = m_state;
    if (state) {
        state->running.store(false);
        // Serialize with the worker's GetCommandManager()/Enqueue sequence. Once
        // this lock is released the detached Windows reader can no longer reach
        // GameServer, even if getline eventually returns after server teardown.
        std::lock_guard<std::mutex> lock(state->serverMutex);
        state->server = nullptr;
    }
    if (m_thread.joinable()) {
#ifndef _WIN32
        // The POSIX read loop wakes from select() within its timeout, so a clean
        // join returns promptly.
        m_thread.join();
#else
        // On Windows std::getline blocks with no portable interrupt; let the
        // reader thread die with the process rather than hang shutdown.
        m_thread.detach();
#endif
    }
    Logger::Info("[ConsoleInput] Console command input stopped");
}

void ConsoleInput::ReadLoop(const std::shared_ptr<State>& state)
{
    while (state && state->running.load()) {
#ifndef _WIN32
        // Wait briefly for input so the loop can re-check m_running and exit on
        // Stop() instead of blocking forever inside getline.
        fd_set readset;
        FD_ZERO(&readset);
        FD_SET(STDIN_FILENO, &readset);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000; // 200ms
        int r = select(STDIN_FILENO + 1, &readset, nullptr, nullptr, &tv);
        if (r <= 0) continue; // timeout (re-check m_running) or error
#endif
        std::string line;
        if (!std::getline(std::cin, line)) {
            // EOF (no TTY / piped input closed): stop reading, leave server up.
            Logger::Info("[ConsoleInput] stdin closed; console input disabled");
            break;
        }

        line = StringUtils::Trim(line);
        if (line.empty()) continue;

        CommandContext ctx;
        ctx.source  = CommandSource::Console;
        ctx.level   = CommandLevel::Console; // local console is fully trusted
        ctx.invoker = "console";
        std::future<QueuedCommandResult> resultFuture;
        {
            std::lock_guard<std::mutex> lock(state->serverMutex);
            if (!state->running.load() || !state->server) break;
            CommandManager* cmdMgr = state->server->GetCommandManager();
            if (!cmdMgr) continue;
            resultFuture = cmdMgr->Enqueue(std::move(ctx), std::move(line));
        }

        // Poll rather than blocking indefinitely so Stop() can always join on
        // POSIX and safely detach on Windows even if the game loop is stalled.
        while (state->running.load() &&
               resultFuture.wait_for(std::chrono::milliseconds(100)) !=
                   std::future_status::ready) {
        }
        if (resultFuture.wait_for(std::chrono::milliseconds(0)) ==
            std::future_status::ready) {
            rs2v::Guard("console command result", [&] {
                QueuedCommandResult result = resultFuture.get();
                if (!result.output.empty()) std::cout << result.output;
            });
        }
        std::cout.flush();
    }
    if (state) state->running.store(false);
}
