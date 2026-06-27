// src/Network/RemoteAdminServer.cpp — SOAP/HTTP remote command transport.

#include "Network/RemoteAdminServer.h"

#include "Network/TCPSocket.h"
#include "Game/GameServer.h"
#include "Game/CommandManager.h"
#include "Utils/Logger.h"
#include "Utils/StringUtils.h"
#include "Utils/CrashHandler.h"

#include <chrono>
#include <thread>

namespace {
// Hard cap on a single request. Remote input is hostile until proven otherwise;
// a length field (Content-Length) is never trusted to size an allocation.
constexpr size_t MAX_REQUEST_BYTES = 64 * 1024;
constexpr size_t RECV_CHUNK = 4096;

// Length-independent equality so a wrong password cannot be narrowed by timing.
bool ConstantTimeEquals(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return diff == 0;
}
} // namespace

RemoteAdminServer::RemoteAdminServer(GameServer* server, RemoteAdminConfig config)
    : m_server(server), m_config(std::move(config))
{
    Logger::Trace("[RemoteAdminServer] ctor port=%u", m_config.port);
}

RemoteAdminServer::~RemoteAdminServer()
{
    Stop();
}

bool RemoteAdminServer::Start()
{
    if (m_config.port == 0 || m_config.password.empty()) {
        Logger::Warn("[RemoteAdminServer] Not starting: port/password not configured");
        return false;
    }
    m_listenSocket = std::make_unique<TCPSocket>();
    SocketConfig cfg;
    cfg.nonBlocking = true;            // so AcceptLoop can poll and honour Stop()
    cfg.recvTimeout = std::chrono::milliseconds(2000);
    if (!m_listenSocket->Listen(m_config.port, 16, cfg)) {
        Logger::Error("[RemoteAdminServer] Failed to listen on port %u", m_config.port);
        m_listenSocket.reset();
        return false;
    }
    m_running.store(true);
    m_thread = std::thread([this] { AcceptLoop(); });
    Logger::Info("[RemoteAdminServer] SOAP remote command endpoint listening on port %u "
                 "(firewall this port to trusted hosts!)", m_config.port);
    return true;
}

void RemoteAdminServer::Stop()
{
    if (!m_running.exchange(false)) return;
    if (m_thread.joinable()) m_thread.join();
    if (m_listenSocket) {
        m_listenSocket->Close();
        m_listenSocket.reset();
    }
    Logger::Info("[RemoteAdminServer] Stopped");
}

void RemoteAdminServer::AcceptLoop()
{
    while (m_running.load()) {
        auto client = m_listenSocket ? m_listenSocket->Accept() : nullptr;
        if (!client) {
            // No pending connection (non-blocking) — brief sleep and re-check.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        // Guarded so a malformed remote request that throws is diagnosed and the
        // accept loop survives (an uncaught exception here would terminate the
        // worker thread, and remote input is untrusted).
        rs2v::Guard("remote admin request", [&] { HandleConnection(*client); });
        client->Close();
    }
}

void RemoteAdminServer::HandleConnection(TCPSocket& client)
{
    client.SetBlocking(true);
    client.SetRecvTimeout(std::chrono::milliseconds(2000));

    // Read the request, bounded. Stop once we have the full body (per
    // Content-Length) or the peer stops sending — never beyond the hard cap.
    std::string request;
    request.reserve(RECV_CHUNK);
    size_t expectedBody = 0;
    bool haveHeaders = false;
    size_t headerEnd = std::string::npos;

    char buf[RECV_CHUNK];
    while (request.size() < MAX_REQUEST_BYTES) {
        ssize_t n = client.Receive(buf, sizeof(buf));
        if (n <= 0) break; // closed, timeout, or error
        request.append(buf, static_cast<size_t>(n));

        if (!haveHeaders) {
            headerEnd = request.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                haveHeaders = true;
                // Parse Content-Length (case-insensitive), clamped to the cap.
                std::string lower = StringUtils::ToLower(request.substr(0, headerEnd));
                size_t pos = lower.find("content-length:");
                if (pos != std::string::npos) {
                    size_t valStart = pos + std::string("content-length:").size();
                    size_t valEnd = lower.find("\r\n", valStart);
                    std::string val = StringUtils::Trim(request.substr(valStart, valEnd - valStart));
                    if (auto parsed = StringUtils::ToInt(val); parsed && *parsed > 0) {
                        expectedBody = static_cast<size_t>(*parsed);
                        if (expectedBody > MAX_REQUEST_BYTES) expectedBody = MAX_REQUEST_BYTES;
                    }
                }
            }
        }
        if (haveHeaders) {
            size_t bodyHave = request.size() - (headerEnd + 4);
            if (bodyHave >= expectedBody) break; // full request received
        }
    }

    if (request.empty()) {
        Logger::Debug("[RemoteAdminServer] Empty/failed request");
        return;
    }

    const std::string password = ExtractTag(request, "password");
    const std::string command  = StringUtils::Trim(ExtractTag(request, "command"));

    auto sendHttp = [&client](int code, const std::string& reason, const std::string& body) {
        std::string resp = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n"
                           "Content-Type: text/xml; charset=utf-8\r\n"
                           "Content-Length: " + std::to_string(body.size()) + "\r\n"
                           "Connection: close\r\n\r\n" + body;
        size_t sent = 0;
        while (sent < resp.size()) {
            ssize_t w = client.Send(resp.data() + sent, resp.size() - sent);
            if (w <= 0) break;
            sent += static_cast<size_t>(w);
        }
    };

    // Authenticate before doing any work.
    if (!ConstantTimeEquals(password, m_config.password)) {
        Logger::Warn("[RemoteAdminServer] Rejected request: bad/missing password");
        sendHttp(401, "Unauthorized", BuildSoapResponse(false, "authentication failed"));
        return;
    }

    if (command.empty()) {
        sendHttp(400, "Bad Request", BuildSoapResponse(false, "missing <command>"));
        return;
    }

    CommandManager* cmdMgr = m_server ? m_server->GetCommandManager() : nullptr;
    if (!cmdMgr) {
        sendHttp(503, "Service Unavailable", BuildSoapResponse(false, "server not ready"));
        return;
    }

    std::string output;
    CommandContext ctx;
    ctx.source  = CommandSource::Remote;
    ctx.level   = CommandManager::LevelFromInt(m_config.defaultLevel);
    ctx.invoker = "remote";
    ctx.server  = m_server;
    ctx.machine = true; // tooling/AI consumer — prefer terse output
    ctx.out = [&output](std::string_view line) { output.append(line); output.push_back('\n'); };

    bool ok = cmdMgr->Execute(ctx, command);
    sendHttp(200, "OK", BuildSoapResponse(ok, output));
}

std::string RemoteAdminServer::BuildSoapResponse(bool ok, const std::string& output)
{
    std::string body =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<soap:Envelope xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\">\n"
        "  <soap:Body>\n"
        "    <ExecuteCommandResponse>\n"
        "      <ok>" + std::string(ok ? "true" : "false") + "</ok>\n"
        "      <output>" + XmlEscape(output) + "</output>\n"
        "    </ExecuteCommandResponse>\n"
        "  </soap:Body>\n"
        "</soap:Envelope>\n";
    return body;
}

std::string RemoteAdminServer::ExtractTag(const std::string& xml, const std::string& tag)
{
    // Defensive, namespace-agnostic extraction: find "<tag" then its '>' then the
    // matching "</tag>". Returns "" if any part is missing or malformed.
    const std::string open = "<" + tag;
    size_t s = xml.find(open);
    if (s == std::string::npos) return "";
    size_t gt = xml.find('>', s);
    if (gt == std::string::npos) return "";
    // Reject "<tagXxx" false matches: the char after the tag name must be '>',
    // whitespace, or '/' (self-closing, which carries no text).
    char after = xml[s + open.size()];
    if (after != '>' && after != ' ' && after != '\t' && after != '/' && after != '\r' && after != '\n')
        return "";
    const std::string close = "</" + tag + ">";
    size_t e = xml.find(close, gt + 1);
    if (e == std::string::npos) return "";
    return XmlUnescape(xml.substr(gt + 1, e - (gt + 1)));
}

std::string RemoteAdminServer::XmlEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out.push_back(c); break;
        }
    }
    return out;
}

std::string RemoteAdminServer::XmlUnescape(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '&') {
            if (s.compare(i, 5, "&amp;") == 0)  { out.push_back('&');  i += 5; continue; }
            if (s.compare(i, 4, "&lt;") == 0)   { out.push_back('<');  i += 4; continue; }
            if (s.compare(i, 4, "&gt;") == 0)   { out.push_back('>');  i += 4; continue; }
            if (s.compare(i, 6, "&quot;") == 0) { out.push_back('"');  i += 6; continue; }
            if (s.compare(i, 6, "&apos;") == 0) { out.push_back('\''); i += 6; continue; }
        }
        out.push_back(s[i]);
        ++i;
    }
    return out;
}
