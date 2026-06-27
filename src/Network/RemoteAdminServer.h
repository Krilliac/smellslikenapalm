// src/Network/RemoteAdminServer.h — SOAP/HTTP remote command transport.
//
// Exposes the unified command system over a tiny SOAP-over-HTTP endpoint so
// off-box tooling — including AI automation — can drive the server remotely.
// A request carries a shared password and a command line; the server
// authenticates, dispatches through CommandManager at the configured permission
// level, and returns the collected output in a SOAP response.
//
// Security posture (the client is untrusted — see CLAUDE.md trust boundary):
//   * Disabled unless BOTH a port and a non-empty password are configured.
//   * Every request body is size-bounded and parsed defensively; malformed or
//     oversized input fails closed (connection dropped / SOAP fault), never
//     crashes or over-reads.
//   * Password mismatch returns a fault and is logged; no command runs.
//   * The listener binds all interfaces (TCPSocket limitation) — operators MUST
//     firewall the port to trusted hosts. Documented in docs/ADMIN_COMMANDS.md.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

class GameServer;
class TCPSocket;

struct RemoteAdminConfig {
    uint16_t    port = 0;          // 0 = disabled
    std::string password;          // required; empty = disabled
    int         defaultLevel = 3;  // CommandLevel applied to authenticated requests
};

class RemoteAdminServer {
public:
    RemoteAdminServer(GameServer* server, RemoteAdminConfig config);
    ~RemoteAdminServer();

    bool Start();
    void Stop();

    // Build a SOAP response body for an executed command. Exposed for testing
    // so the XML shaping is verified without opening a socket.
    static std::string BuildSoapResponse(bool ok, const std::string& output);

    // Extract the text content of the first <tag>...</tag> in `xml`, with the
    // minimal XML entities decoded. Returns empty string if absent. Exposed for
    // testing the (defensive) parser directly.
    static std::string ExtractTag(const std::string& xml, const std::string& tag);

private:
    void AcceptLoop();
    void HandleConnection(TCPSocket& client);
    static std::string XmlEscape(const std::string& s);
    static std::string XmlUnescape(const std::string& s);

    GameServer*                 m_server;
    RemoteAdminConfig           m_config;
    std::unique_ptr<TCPSocket>  m_listenSocket;
    std::atomic<bool>           m_running{false};
    std::thread                 m_thread;
};
