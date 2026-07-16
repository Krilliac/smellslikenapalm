// src/Network/ConsoleInput.h — local stdin command transport.
//
// Reads command lines from the server's standard input on a background thread
// and dispatches them through CommandManager at Console permission (full trust —
// anyone with a shell on the box already controls the process). Output is
// printed to stdout. On EOF (e.g. a headless/daemonised launch with no TTY) the
// reader thread exits cleanly and the server keeps running.

#pragma once

#include <memory>
#include <thread>

class GameServer;

class ConsoleInput {
public:
    explicit ConsoleInput(GameServer* server);
    ~ConsoleInput();

    void Start();
    void Stop();

private:
    struct State;
    static void ReadLoop(const std::shared_ptr<State>& state);

    // The Windows stdin read may remain blocked during shutdown. The worker
    // therefore captures this shared state, never `this`; Stop() can detach
    // without leaving a dangling ConsoleInput or GameServer pointer behind.
    std::shared_ptr<State> m_state;
    std::thread            m_thread;
};
