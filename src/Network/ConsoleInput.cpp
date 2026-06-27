// src/Network/ConsoleInput.cpp — stdin command reader.

#include "Network/ConsoleInput.h"

#include "Game/GameServer.h"
#include "Game/CommandManager.h"
#include "Utils/Logger.h"
#include "Utils/StringUtils.h"
#include "Utils/CrashHandler.h"

#include <iostream>
#include <string>

#ifndef _WIN32
#include <sys/select.h>
#include <unistd.h>
#endif

ConsoleInput::ConsoleInput(GameServer* server)
    : m_server(server)
{
    Logger::Trace("[ConsoleInput::ConsoleInput] Entry");
}

ConsoleInput::~ConsoleInput()
{
    Stop();
}

void ConsoleInput::Start()
{
    if (m_running.exchange(true)) return;
    Logger::Info("[ConsoleInput] Console command input ready (type 'help')");
    m_thread = std::thread([this] { ReadLoop(); });
}

void ConsoleInput::Stop()
{
    // Always reach the join: the reader thread may have already self-exited on
    // EOF (clearing m_running itself), but it is still joinable. Destroying a
    // joinable std::thread calls std::terminate, so a guard that skipped the
    // join here would abort the process at shutdown.
    m_running.store(false);
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

void ConsoleInput::ReadLoop()
{
    while (m_running.load()) {
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
        if (!m_server) break;

        CommandManager* cmdMgr = m_server->GetCommandManager();
        if (!cmdMgr) continue;

        CommandContext ctx;
        ctx.source  = CommandSource::Console;
        ctx.level   = CommandLevel::Console; // local console is fully trusted
        ctx.invoker = "console";
        ctx.server  = m_server;
        ctx.out     = [](std::string_view s) { std::cout << s << "\n"; };
        // Guarded so a throwing command can't take down the reader thread (an
        // uncaught exception in a std::thread calls std::terminate).
        rs2v::Guard("console command", [&] { cmdMgr->Execute(ctx, line); });
        std::cout.flush();
    }
    m_running.store(false);
}
