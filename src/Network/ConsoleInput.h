// src/Network/ConsoleInput.h — local stdin command transport.
//
// Reads command lines from the server's standard input on a background thread
// and dispatches them through CommandManager at Console permission (full trust —
// anyone with a shell on the box already controls the process). Output is
// printed to stdout. On EOF (e.g. a headless/daemonised launch with no TTY) the
// reader thread exits cleanly and the server keeps running.

#pragma once

#include <atomic>
#include <thread>

class GameServer;

class ConsoleInput {
public:
    explicit ConsoleInput(GameServer* server);
    ~ConsoleInput();

    void Start();
    void Stop();

private:
    void ReadLoop();

    GameServer*       m_server;
    std::atomic<bool> m_running{false};
    std::thread       m_thread;
};
