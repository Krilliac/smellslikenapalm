// Server/telemetry/PrometheusReporter.cpp
// Implementation of Prometheus HTTP endpoint metrics reporter
// for the RS2V server telemetry system

#include "MetricsReporter.h"
#include "TelemetryManager.h"
#include "Utils/Logger.h"
#include "Network/PlatformSocket.h"  // SocketHandle, RS2V_INVALID_SOCKET, ws2tcpip.h (inet_ntop)

#include <cstdint>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <regex>
#include <chrono>
#include <thread>
#include <cstring>
#include <cmath>

// Platform-specific networking extras not covered by PlatformSocket.h.
// PlatformSocket.h already pulls in the right system headers and defines
// SocketHandle / RS2V_INVALID_SOCKET; here we just add the Berkeley-socket
// status sentinels and helpers this file still references.
#ifdef _WIN32
    #pragma comment(lib, "ws2_32.lib")
    typedef int socklen_t;
#else
    #include <signal.h>
    #ifndef INVALID_SOCKET
    #define INVALID_SOCKET (-1)
    #endif
    #ifndef SOCKET_ERROR
    #define SOCKET_ERROR (-1)
    #endif
    #ifndef closesocket
    #define closesocket close
    #endif
#endif

namespace Telemetry {

PrometheusMetricsReporter::PrometheusMetricsReporter(const PrometheusReporterConfig& config)
    : m_config(config), m_healthy(true), m_serverRunning(false),
      m_reportsGenerated(0), m_reportsFailed(0), m_httpRequests(0),
      m_serverSocket(INVALID_SOCKET) {

    Logger::Trace("[PrometheusMetricsReporter::PrometheusMetricsReporter] Entry - config.port=%d, config.endpoint='%s', config.metricsPrefix='%s', config.enableTimestamps=%d",
                 config.port, config.endpoint.c_str(), config.metricsPrefix.c_str(), config.enableTimestamps);

    // Validate configuration
    if (m_config.port <= 0 || m_config.port > 65535) {
        Logger::Warn("[PrometheusMetricsReporter::PrometheusMetricsReporter] Invalid port %d, falling back to default 9090", m_config.port);
        m_config.port = 9090; // Default Prometheus port
    }
    if (m_config.endpoint.empty()) {
        Logger::Warn("[PrometheusMetricsReporter::PrometheusMetricsReporter] Empty endpoint, defaulting to '/metrics'");
        m_config.endpoint = "/metrics";
    }
    if (m_config.metricsPrefix.empty()) {
        Logger::Warn("[PrometheusMetricsReporter::PrometheusMetricsReporter] Empty metrics prefix, defaulting to 'rs2v_server'");
        m_config.metricsPrefix = "rs2v_server";
    }

    // Ensure endpoint starts with /
    if (m_config.endpoint[0] != '/') {
        Logger::Debug("[PrometheusMetricsReporter::PrometheusMetricsReporter] Endpoint '%s' does not start with '/', prepending '/'", m_config.endpoint.c_str());
        m_config.endpoint = "/" + m_config.endpoint;
    }

    Logger::Debug("[PrometheusMetricsReporter::PrometheusMetricsReporter] Validated config: port=%d, endpoint='%s', prefix='%s', excludeMetrics count=%zu, staticLabels count=%zu",
                 m_config.port, m_config.endpoint.c_str(), m_config.metricsPrefix.c_str(),
                 m_config.excludeMetrics.size(), m_config.staticLabels.size());

    Logger::Info("PrometheusMetricsReporter created with config: port=%d, endpoint=%s, prefix=%s",
                m_config.port, m_config.endpoint.c_str(), m_config.metricsPrefix.c_str());

#ifdef _WIN32
    // Initialize Winsock
    Logger::Debug("[PrometheusMetricsReporter::PrometheusMetricsReporter] Initializing Winsock on Windows platform");
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        Logger::Error("[PrometheusMetricsReporter::PrometheusMetricsReporter] WSAStartup failed");
        ReportError("Failed to initialize Winsock");
        m_healthy = false;
    } else {
        Logger::Debug("[PrometheusMetricsReporter::PrometheusMetricsReporter] Winsock initialized successfully");
    }
#else
    // Ignore SIGPIPE for robustness
    Logger::Debug("[PrometheusMetricsReporter::PrometheusMetricsReporter] Setting SIGPIPE to SIG_IGN for robustness");
    signal(SIGPIPE, SIG_IGN);
#endif
    Logger::Trace("[PrometheusMetricsReporter::PrometheusMetricsReporter] Exit - construction complete");
}

PrometheusMetricsReporter::~PrometheusMetricsReporter() {
    Logger::Trace("[PrometheusMetricsReporter::~PrometheusMetricsReporter] Destructor invoked, initiating shutdown");
    Shutdown();

#ifdef _WIN32
    Logger::Debug("[PrometheusMetricsReporter::~PrometheusMetricsReporter] Cleaning up Winsock");
    WSACleanup();
#endif
    Logger::Trace("[PrometheusMetricsReporter::~PrometheusMetricsReporter] Destructor completed");
}

bool PrometheusMetricsReporter::Initialize(const std::string& outputDirectory) {
    Logger::Trace("[PrometheusMetricsReporter::Initialize] Entry - outputDirectory='%s' (unused for Prometheus)", outputDirectory.c_str());
    // outputDirectory is not used for Prometheus reporter but kept for interface compliance
    (void)outputDirectory;

    try {
        Logger::Debug("[PrometheusMetricsReporter::Initialize] Creating server socket on port %d", m_config.port);
        if (!CreateServerSocket()) {
            Logger::Error("[PrometheusMetricsReporter::Initialize] Failed to create HTTP server socket on port %d", m_config.port);
            ReportError("Failed to create HTTP server socket");
            m_healthy = false;
            Logger::Trace("[PrometheusMetricsReporter::Initialize] Exit - returning false (socket creation failed)");
            return false;
        }
        Logger::Debug("[PrometheusMetricsReporter::Initialize] Server socket created successfully");

        Logger::Debug("[PrometheusMetricsReporter::Initialize] Starting HTTP server thread");
        StartHTTPServer();

        m_healthy = true;
        Logger::Info("PrometheusMetricsReporter initialized successfully on port %d", m_config.port);
        Logger::Trace("[PrometheusMetricsReporter::Initialize] Exit - returning true (success)");
        return true;

    } catch (const std::exception& ex) {
        Logger::Error("[PrometheusMetricsReporter::Initialize] Exception during initialization: %s", ex.what());
        ReportError("Failed to initialize PrometheusMetricsReporter: " + std::string(ex.what()));
        m_healthy = false;
        Logger::Trace("[PrometheusMetricsReporter::Initialize] Exit - returning false (exception)");
        return false;
    }
}

void PrometheusMetricsReporter::Shutdown() {
    Logger::Trace("[PrometheusMetricsReporter::Shutdown] Entry");
    Logger::Debug("[PrometheusMetricsReporter::Shutdown] Stopping HTTP server, total requests served so far: %llu", (unsigned long long)m_httpRequests.load());
    StopHTTPServer();

    Logger::Info("PrometheusMetricsReporter shutdown complete. Served %llu HTTP requests total.",
                (unsigned long long)m_httpRequests.load());
    Logger::Debug("[PrometheusMetricsReporter::Shutdown] Final stats: reportsGenerated=%llu, reportsFailed=%llu",
                 (unsigned long long)m_reportsGenerated.load(), (unsigned long long)m_reportsFailed.load());
    Logger::Trace("[PrometheusMetricsReporter::Shutdown] Exit");
}

void PrometheusMetricsReporter::Report(const MetricsSnapshot& snapshot) {
    Logger::Trace("[PrometheusMetricsReporter::Report] Entry - snapshot with activeConnections=%llu, activeMatches=%llu",
                 (unsigned long long)snapshot.activeConnections, (unsigned long long)snapshot.activeMatches);
    if (!m_healthy.load()) {
        Logger::Warn("[PrometheusMetricsReporter::Report] Reporter is unhealthy, discarding snapshot");
        m_reportsFailed.fetch_add(1, std::memory_order_relaxed);
        Logger::Debug("[PrometheusMetricsReporter::Report] Reports failed count incremented to %llu", (unsigned long long)m_reportsFailed.load());
        Logger::Trace("[PrometheusMetricsReporter::Report] Exit - early return (unhealthy)");
        return;
    }

    try {
        // Store the latest snapshot for HTTP responses
        {
            std::lock_guard<std::mutex> lock(m_snapshotMutex);
            m_latestSnapshot = snapshot;
            Logger::Debug("[PrometheusMetricsReporter::Report] Latest snapshot updated for HTTP responses");
        }

        auto totalReports = m_reportsGenerated.fetch_add(1, std::memory_order_relaxed) + 1;
        Logger::Debug("[PrometheusMetricsReporter::Report] Snapshot stored successfully, total reports generated: %llu", (unsigned long long)totalReports);

    } catch (const std::exception& ex) {
        Logger::Error("[PrometheusMetricsReporter::Report] Exception updating metrics: %s", ex.what());
        ReportError("Error updating Prometheus metrics: " + std::string(ex.what()));
        m_reportsFailed.fetch_add(1, std::memory_order_relaxed);
    }
    Logger::Trace("[PrometheusMetricsReporter::Report] Exit");
}

std::vector<std::string> PrometheusMetricsReporter::GetLastErrors() const {
    Logger::Trace("[PrometheusMetricsReporter::GetLastErrors] Entry");
    std::lock_guard<std::mutex> lock(m_errorMutex);
    Logger::Debug("[PrometheusMetricsReporter::GetLastErrors] Returning %zu stored errors", m_errors.size());
    Logger::Trace("[PrometheusMetricsReporter::GetLastErrors] Exit - returning %zu errors", m_errors.size());
    return m_errors;
}

void PrometheusMetricsReporter::ClearErrors() {
    Logger::Trace("[PrometheusMetricsReporter::ClearErrors] Entry");
    std::lock_guard<std::mutex> lock(m_errorMutex);
    Logger::Debug("[PrometheusMetricsReporter::ClearErrors] Clearing %zu stored errors and resetting healthy flag", m_errors.size());
    m_errors.clear();
    m_healthy = true;
    Logger::Info("[PrometheusMetricsReporter::ClearErrors] Error history cleared, reporter marked as healthy");
    Logger::Trace("[PrometheusMetricsReporter::ClearErrors] Exit");
}

std::string PrometheusMetricsReporter::GetMetricsEndpoint() const {
    Logger::Trace("[PrometheusMetricsReporter::GetMetricsEndpoint] Entry");
    std::string endpoint = "http://localhost:" + std::to_string(m_config.port) + m_config.endpoint;
    Logger::Debug("[PrometheusMetricsReporter::GetMetricsEndpoint] Constructed endpoint URL: %s", endpoint.c_str());
    Logger::Trace("[PrometheusMetricsReporter::GetMetricsEndpoint] Exit - returning '%s'", endpoint.c_str());
    return endpoint;
}

void PrometheusMetricsReporter::StartHTTPServer() {
    Logger::Trace("[PrometheusMetricsReporter::StartHTTPServer] Entry");
    if (m_serverRunning.exchange(true)) {
        Logger::Warn("Prometheus HTTP server already running");
        Logger::Debug("[PrometheusMetricsReporter::StartHTTPServer] m_serverRunning was already true, skipping start");
        Logger::Trace("[PrometheusMetricsReporter::StartHTTPServer] Exit - early return (already running)");
        return;
    }

    Logger::Info("Starting Prometheus HTTP server on port %d", m_config.port);
    Logger::Debug("[PrometheusMetricsReporter::StartHTTPServer] Launching HTTP server thread for endpoint '%s'", m_config.endpoint.c_str());

    m_serverThread = std::thread(&PrometheusMetricsReporter::HTTPServerLoop, this);
    Logger::Debug("[PrometheusMetricsReporter::StartHTTPServer] HTTP server thread launched successfully");
    Logger::Trace("[PrometheusMetricsReporter::StartHTTPServer] Exit");
}

void PrometheusMetricsReporter::StopHTTPServer() {
    Logger::Trace("[PrometheusMetricsReporter::StopHTTPServer] Entry");
    if (!m_serverRunning.exchange(false)) {
        Logger::Debug("[PrometheusMetricsReporter::StopHTTPServer] Server was not running, nothing to stop");
        Logger::Trace("[PrometheusMetricsReporter::StopHTTPServer] Exit - early return (was not running)");
        return;
    }

    Logger::Info("Stopping Prometheus HTTP server...");

    // Close server socket to break accept loop
    if (m_serverSocket != INVALID_SOCKET) {
        Logger::Debug("[PrometheusMetricsReporter::StopHTTPServer] Closing server socket to break accept loop");
        closesocket(m_serverSocket);
        m_serverSocket = INVALID_SOCKET;
        Logger::Debug("[PrometheusMetricsReporter::StopHTTPServer] Server socket closed");
    } else {
        Logger::Debug("[PrometheusMetricsReporter::StopHTTPServer] Server socket was already INVALID_SOCKET");
    }

    if (m_serverThread.joinable()) {
        Logger::Debug("[PrometheusMetricsReporter::StopHTTPServer] Waiting for HTTP server thread to join");
        m_serverThread.join();
        Logger::Debug("[PrometheusMetricsReporter::StopHTTPServer] HTTP server thread joined successfully");
    } else {
        Logger::Debug("[PrometheusMetricsReporter::StopHTTPServer] Server thread is not joinable");
    }

    Logger::Info("Prometheus HTTP server stopped");
    Logger::Trace("[PrometheusMetricsReporter::StopHTTPServer] Exit");
}

bool PrometheusMetricsReporter::CreateServerSocket() {
    Logger::Trace("[PrometheusMetricsReporter::CreateServerSocket] Entry");
    Logger::Debug("[PrometheusMetricsReporter::CreateServerSocket] Creating AF_INET SOCK_STREAM socket");

    m_serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_serverSocket == INVALID_SOCKET) {
        Logger::Error("[PrometheusMetricsReporter::CreateServerSocket] socket() returned INVALID_SOCKET");
        ReportError("Failed to create socket");
        Logger::Trace("[PrometheusMetricsReporter::CreateServerSocket] Exit - returning false (socket creation failed)");
        return false;
    }
    Logger::Debug("[PrometheusMetricsReporter::CreateServerSocket] Socket created successfully, fd=%d", (int)m_serverSocket);

    // Set socket options
    int optval = 1;
    if (setsockopt(m_serverSocket, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&optval), sizeof(optval)) == SOCKET_ERROR) {
        Logger::Warn("Failed to set SO_REUSEADDR on Prometheus socket");
        Logger::Debug("[PrometheusMetricsReporter::CreateServerSocket] setsockopt SO_REUSEADDR failed, continuing anyway");
    } else {
        Logger::Debug("[PrometheusMetricsReporter::CreateServerSocket] SO_REUSEADDR set successfully");
    }

#ifndef _WIN32
    // Set non-blocking mode for accept timeout
    Logger::Debug("[PrometheusMetricsReporter::CreateServerSocket] Setting socket to non-blocking mode (Unix)");
    int flags = fcntl(m_serverSocket, F_GETFL, 0);
    if (flags != -1) {
        fcntl(m_serverSocket, F_SETFL, flags | O_NONBLOCK);
        Logger::Debug("[PrometheusMetricsReporter::CreateServerSocket] Socket set to non-blocking mode");
    } else {
        Logger::Warn("[PrometheusMetricsReporter::CreateServerSocket] fcntl F_GETFL failed, unable to set non-blocking mode");
    }
#endif

    // Bind socket
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(static_cast<uint16_t>(m_config.port));

    Logger::Debug("[PrometheusMetricsReporter::CreateServerSocket] Binding socket to 0.0.0.0:%d", m_config.port);
    if (bind(m_serverSocket, reinterpret_cast<struct sockaddr*>(&serverAddr),
             sizeof(serverAddr)) == SOCKET_ERROR) {
        Logger::Error("[PrometheusMetricsReporter::CreateServerSocket] bind() failed on port %d", m_config.port);
        ReportError("Failed to bind socket to port " + std::to_string(m_config.port));
        closesocket(m_serverSocket);
        m_serverSocket = INVALID_SOCKET;
        Logger::Trace("[PrometheusMetricsReporter::CreateServerSocket] Exit - returning false (bind failed)");
        return false;
    }
    Logger::Debug("[PrometheusMetricsReporter::CreateServerSocket] Socket bound to port %d successfully", m_config.port);

    // Listen for connections
    Logger::Debug("[PrometheusMetricsReporter::CreateServerSocket] Setting socket to listen with backlog=5");
    if (listen(m_serverSocket, 5) == SOCKET_ERROR) {
        Logger::Error("[PrometheusMetricsReporter::CreateServerSocket] listen() failed on socket");
        ReportError("Failed to listen on socket");
        closesocket(m_serverSocket);
        m_serverSocket = INVALID_SOCKET;
        Logger::Trace("[PrometheusMetricsReporter::CreateServerSocket] Exit - returning false (listen failed)");
        return false;
    }

    Logger::Info("[PrometheusMetricsReporter::CreateServerSocket] Server socket ready and listening on port %d", m_config.port);
    Logger::Trace("[PrometheusMetricsReporter::CreateServerSocket] Exit - returning true (success)");
    return true;
}

void PrometheusMetricsReporter::HTTPServerLoop() {
    Logger::Trace("[PrometheusMetricsReporter::HTTPServerLoop] Entry");
    Logger::Info("Prometheus HTTP server loop started");

    uint64_t acceptAttempts = 0;
    uint64_t clientsAccepted = 0;

    while (m_serverRunning.load()) {
        try {
            struct sockaddr_in clientAddr;
            socklen_t clientAddrLen = sizeof(clientAddr);
            acceptAttempts++;

#ifdef _WIN32
            // Windows blocking accept with timeout
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(m_serverSocket, &readfds);

            struct timeval timeout;
            timeout.tv_sec = 1;
            timeout.tv_usec = 0;

            Logger::Trace("[PrometheusMetricsReporter::HTTPServerLoop] Windows select() waiting for connection (attempt %llu)", (unsigned long long)acceptAttempts);
            int selectResult = select(0, &readfds, nullptr, nullptr, &timeout);
            if (selectResult == 0) {
                Logger::Trace("[PrometheusMetricsReporter::HTTPServerLoop] Select timeout, checking if server should stop");
                continue; // Timeout, check if we should stop
            }
            if (selectResult == SOCKET_ERROR) {
                if (m_serverRunning.load()) {
                    Logger::Error("[PrometheusMetricsReporter::HTTPServerLoop] select() returned SOCKET_ERROR");
                    ReportError("Select error in HTTP server loop");
                }
                break;
            }
            Logger::Trace("[PrometheusMetricsReporter::HTTPServerLoop] Select returned %d, connection ready", selectResult);
#endif

            Logger::Trace("[PrometheusMetricsReporter::HTTPServerLoop] Calling accept() (attempt %llu)", (unsigned long long)acceptAttempts);
            SocketHandle clientSocket = accept(m_serverSocket,
                                       reinterpret_cast<struct sockaddr*>(&clientAddr),
                                       &clientAddrLen);

            if (clientSocket == INVALID_SOCKET) {
#ifdef _WIN32
                int error = WSAGetLastError();
                if (error != WSAEWOULDBLOCK && m_serverRunning.load()) {
                    Logger::Error("[PrometheusMetricsReporter::HTTPServerLoop] Accept failed with Windows error: %d", error);
                    ReportError("Accept failed with error: " + std::to_string(error));
                } else {
                    Logger::Trace("[PrometheusMetricsReporter::HTTPServerLoop] Accept returned WSAEWOULDBLOCK, sleeping 100ms");
                }
#else
                if (errno != EAGAIN && errno != EWOULDBLOCK && m_serverRunning.load()) {
                    Logger::Error("[PrometheusMetricsReporter::HTTPServerLoop] Accept failed: %s (errno=%d)", strerror(errno), errno);
                    ReportError("Accept failed: " + std::string(strerror(errno)));
                } else {
                    Logger::Trace("[PrometheusMetricsReporter::HTTPServerLoop] Accept returned EAGAIN/EWOULDBLOCK, sleeping 100ms");
                }
#endif
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            clientsAccepted++;
            // Thread-safe address formatting (inet_ntop replaces the non-reentrant,
            // deprecated inet_ntoa). Buffer is large enough for IPv4 dotted-quad.
            char clientIpStr[INET_ADDRSTRLEN] = {0};
            if (inet_ntop(AF_INET, &clientAddr.sin_addr, clientIpStr, sizeof(clientIpStr)) == nullptr) {
                std::strncpy(clientIpStr, "unknown", sizeof(clientIpStr) - 1);
            }
            Logger::Debug("[PrometheusMetricsReporter::HTTPServerLoop] Accepted client connection #%llu from %s:%d, socket fd=%d",
                         (unsigned long long)clientsAccepted,
                         clientIpStr, ntohs(clientAddr.sin_port), (int)clientSocket);

            // Handle client connection in separate thread for non-blocking operation
            Logger::Trace("[PrometheusMetricsReporter::HTTPServerLoop] Spawning detached thread for client %d", (int)clientSocket);
            std::thread clientThread(&PrometheusMetricsReporter::HandleClientConnection,
                                   this, clientSocket);
            clientThread.detach();

        } catch (const std::exception& ex) {
            if (m_serverRunning.load()) {
                Logger::Error("[PrometheusMetricsReporter::HTTPServerLoop] Exception in server loop: %s", ex.what());
                ReportError("Exception in HTTP server loop: " + std::string(ex.what()));
            }
        } catch (...) {
            // A non-std throw must not escape this thread function into
            // std::terminate. Record it and keep accepting connections.
            if (m_serverRunning.load()) {
                Logger::Error("[PrometheusMetricsReporter::HTTPServerLoop] Non-std exception in server loop");
                ReportError("Non-std exception in HTTP server loop");
            }
        }
    }

    Logger::Info("Prometheus HTTP server loop stopped");
    Logger::Debug("[PrometheusMetricsReporter::HTTPServerLoop] Loop stats: acceptAttempts=%llu, clientsAccepted=%llu", (unsigned long long)acceptAttempts, (unsigned long long)clientsAccepted);
    Logger::Trace("[PrometheusMetricsReporter::HTTPServerLoop] Exit");
}

void PrometheusMetricsReporter::HandleClientConnection(SocketHandle clientSocket) {
    Logger::Trace("[PrometheusMetricsReporter::HandleClientConnection] Entry - clientSocket=%d", (int)clientSocket);
    try {
        // Set socket timeout
        struct timeval timeout;
        timeout.tv_sec = 5;  // 5 second timeout
        timeout.tv_usec = 0;
        Logger::Trace("[PrometheusMetricsReporter::HandleClientConnection] Setting socket timeouts (5s) for recv and send");
        setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO,
                  reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO,
                  reinterpret_cast<const char*>(&timeout), sizeof(timeout));

        // Read HTTP request
        char buffer[4096];
        int bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);

        if (bytesReceived <= 0) {
            Logger::Debug("[PrometheusMetricsReporter::HandleClientConnection] recv returned %d, closing client socket %d", bytesReceived, (int)clientSocket);
            closesocket(clientSocket);
            Logger::Trace("[PrometheusMetricsReporter::HandleClientConnection] Exit - early return (no data received)");
            return;
        }

        buffer[bytesReceived] = '\0';
        std::string request(buffer);
        Logger::Trace("[PrometheusMetricsReporter::HandleClientConnection] Received %d bytes from client", bytesReceived);

        // Parse HTTP request line
        std::istringstream requestStream(request);
        std::string method, path, protocol;
        requestStream >> method >> path >> protocol;
        Logger::Debug("[PrometheusMetricsReporter::HandleClientConnection] HTTP request: method='%s', path='%s', protocol='%s'", method.c_str(), path.c_str(), protocol.c_str());

        auto totalRequests = m_httpRequests.fetch_add(1, std::memory_order_relaxed) + 1;
        Logger::Debug("[PrometheusMetricsReporter::HandleClientConnection] Total HTTP requests served: %llu", (unsigned long long)totalRequests);

        std::string response;

        if (method == "GET" && path == m_config.endpoint) {
            // Generate metrics response
            Logger::Debug("[PrometheusMetricsReporter::HandleClientConnection] Matched metrics endpoint '%s', generating Prometheus metrics response", m_config.endpoint.c_str());
            std::string metricsData = FormatPrometheusMetrics();
            Logger::Trace("[PrometheusMetricsReporter::HandleClientConnection] Generated metrics data: %zu bytes", metricsData.length());

            response = "HTTP/1.1 200 OK\r\n";
            response += "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n";
            response += "Content-Length: " + std::to_string(metricsData.length()) + "\r\n";
            response += "Connection: close\r\n";
            response += "\r\n";
            response += metricsData;

            Logger::Info("[PrometheusMetricsReporter::HandleClientConnection] Served metrics response (200 OK, %zu bytes) to client on socket %d", metricsData.length(), (int)clientSocket);

        } else if (method == "GET" && path == "/") {
            // Root path - provide basic info
            Logger::Debug("[PrometheusMetricsReporter::HandleClientConnection] Matched root path '/', returning server info");
            std::string info = "RS2V Server Prometheus Metrics\n";
            info += "Metrics endpoint: " + m_config.endpoint + "\n";
            info += "Server version: 1.0.0\n";

            response = "HTTP/1.1 200 OK\r\n";
            response += "Content-Type: text/plain\r\n";
            response += "Content-Length: " + std::to_string(info.length()) + "\r\n";
            response += "Connection: close\r\n";
            response += "\r\n";
            response += info;

            Logger::Debug("[PrometheusMetricsReporter::HandleClientConnection] Served info response (200 OK) to client on socket %d", (int)clientSocket);

        } else {
            // 404 Not Found
            Logger::Debug("[PrometheusMetricsReporter::HandleClientConnection] No route matched for %s %s, returning 404", method.c_str(), path.c_str());
            std::string notFound = "404 Not Found\nTry: " + m_config.endpoint + "\n";

            response = "HTTP/1.1 404 Not Found\r\n";
            response += "Content-Type: text/plain\r\n";
            response += "Content-Length: " + std::to_string(notFound.length()) + "\r\n";
            response += "Connection: close\r\n";
            response += "\r\n";
            response += notFound;

            Logger::Warn("[PrometheusMetricsReporter::HandleClientConnection] Returned 404 for %s %s to client on socket %d", method.c_str(), path.c_str(), (int)clientSocket);
        }

        // Send response
        Logger::Trace("[PrometheusMetricsReporter::HandleClientConnection] Sending %zu byte response to client socket %d", response.length(), (int)clientSocket);
        int bytesSent = send(clientSocket, response.c_str(), static_cast<int>(response.length()), 0);
        Logger::Trace("[PrometheusMetricsReporter::HandleClientConnection] send() returned %d", bytesSent);

    } catch (const std::exception& ex) {
        Logger::Error("Error handling Prometheus HTTP client: %s", ex.what());
        Logger::Error("[PrometheusMetricsReporter::HandleClientConnection] Exception details on socket %d: %s", (int)clientSocket, ex.what());
    } catch (...) {
        // Runs on a detached thread: a non-std throw here would reach
        // std::terminate with no one to observe it. Swallow + log, then still
        // close the socket below.
        Logger::Error("[PrometheusMetricsReporter::HandleClientConnection] Non-std exception on socket %d", (int)clientSocket);
    }

    Logger::Trace("[PrometheusMetricsReporter::HandleClientConnection] Closing client socket %d", (int)clientSocket);
    closesocket(clientSocket);
    Logger::Trace("[PrometheusMetricsReporter::HandleClientConnection] Exit");
}

std::string PrometheusMetricsReporter::FormatPrometheusMetrics() const {
    Logger::Trace("[PrometheusMetricsReporter::FormatPrometheusMetrics] Entry");
    std::ostringstream metrics;

    MetricsSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(m_snapshotMutex);
        snapshot = m_latestSnapshot;
        Logger::Trace("[PrometheusMetricsReporter::FormatPrometheusMetrics] Copied latest snapshot under lock");
    }

    // Add timestamp comment
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    metrics << "# Generated at " << timestamp << "\n";
    metrics << "# RS2V Server Metrics\n\n";
    Logger::Trace("[PrometheusMetricsReporter::FormatPrometheusMetrics] Formatting metrics at timestamp %lld", (long long)timestamp);

    // System metrics
    Logger::Debug("[PrometheusMetricsReporter::FormatPrometheusMetrics] Formatting system metrics: cpu=%.2f%%, memUsed=%llu, memTotal=%llu",
                 snapshot.cpuUsagePercent, (unsigned long long)snapshot.memoryUsedBytes, (unsigned long long)snapshot.memoryTotalBytes);
    if (snapshot.cpuUsagePercent >= 0.0) {
        metrics << FormatMetricLine("cpu_usage_percent", snapshot.cpuUsagePercent);
        Logger::Trace("[PrometheusMetricsReporter::FormatPrometheusMetrics] Added metric: cpu_usage_percent=%.2f", snapshot.cpuUsagePercent);
    } else {
        Logger::Trace("[PrometheusMetricsReporter::FormatPrometheusMetrics] Skipping cpu_usage_percent (value < 0)");
    }

    if (snapshot.memoryUsedBytes > 0) {
        metrics << FormatMetricLine("memory_used_bytes", static_cast<double>(snapshot.memoryUsedBytes));
        metrics << FormatMetricLine("memory_total_bytes", static_cast<double>(snapshot.memoryTotalBytes));

        if (snapshot.memoryTotalBytes > 0) {
            double memoryUsagePercent = (static_cast<double>(snapshot.memoryUsedBytes) /
                                       snapshot.memoryTotalBytes) * 100.0;
            metrics << FormatMetricLine("memory_usage_percent", memoryUsagePercent);
            Logger::Trace("[PrometheusMetricsReporter::FormatPrometheusMetrics] Added metric: memory_usage_percent=%.2f", memoryUsagePercent);
        } else {
            Logger::Trace("[PrometheusMetricsReporter::FormatPrometheusMetrics] Skipping memory_usage_percent (memoryTotalBytes is 0)");
        }
    } else {
        Logger::Trace("[PrometheusMetricsReporter::FormatPrometheusMetrics] Skipping memory metrics (memoryUsedBytes is 0)");
    }

    metrics << FormatMetricLine("network_bytes_sent_total", static_cast<double>(snapshot.networkBytesSent));
    metrics << FormatMetricLine("network_bytes_received_total", static_cast<double>(snapshot.networkBytesReceived));
    metrics << FormatMetricLine("disk_read_bytes_total", static_cast<double>(snapshot.diskReadBytes));
    metrics << FormatMetricLine("disk_write_bytes_total", static_cast<double>(snapshot.diskWriteBytes));

    // Network application metrics
    Logger::Debug("[PrometheusMetricsReporter::FormatPrometheusMetrics] Formatting network application metrics: connections=%llu, players=%llu, packets=%llu",
                 (unsigned long long)snapshot.activeConnections, (unsigned long long)snapshot.authenticatedPlayers, (unsigned long long)snapshot.totalPacketsProcessed);
    metrics << FormatMetricLine("active_connections", static_cast<double>(snapshot.activeConnections));
    metrics << FormatMetricLine("authenticated_players", static_cast<double>(snapshot.authenticatedPlayers));
    metrics << FormatMetricLine("packets_processed_total", static_cast<double>(snapshot.totalPacketsProcessed));
    metrics << FormatMetricLine("packets_dropped_total", static_cast<double>(snapshot.totalPacketsDropped));
    metrics << FormatMetricLine("average_latency_milliseconds", snapshot.averageLatencyMs);
    metrics << FormatMetricLine("packet_loss_rate", snapshot.packetLossRate);

    // Gameplay metrics
    Logger::Debug("[PrometheusMetricsReporter::FormatPrometheusMetrics] Formatting gameplay metrics: matches=%llu, kills=%llu, deaths=%llu",
                 (unsigned long long)snapshot.activeMatches, (unsigned long long)snapshot.totalKills, (unsigned long long)snapshot.totalDeaths);
    metrics << FormatMetricLine("current_tick", static_cast<double>(snapshot.currentTick));
    metrics << FormatMetricLine("active_matches", static_cast<double>(snapshot.activeMatches));
    metrics << FormatMetricLine("kills_total", static_cast<double>(snapshot.totalKills));
    metrics << FormatMetricLine("deaths_total", static_cast<double>(snapshot.totalDeaths));
    metrics << FormatMetricLine("objectives_captured_total", static_cast<double>(snapshot.objectivesCaptured));
    metrics << FormatMetricLine("chat_messages_sent_total", static_cast<double>(snapshot.chatMessagesSent));

    // Performance metrics
    Logger::Debug("[PrometheusMetricsReporter::FormatPrometheusMetrics] Formatting performance metrics: frameTime=%.2fms, physicsTime=%.2fms",
                 snapshot.frameTimeMs, snapshot.physicsTimeMs);
    metrics << FormatMetricLine("frame_time_milliseconds", snapshot.frameTimeMs);
    metrics << FormatMetricLine("physics_time_milliseconds", snapshot.physicsTimeMs);
    metrics << FormatMetricLine("network_time_milliseconds", snapshot.networkTimeMs);
    metrics << FormatMetricLine("game_logic_time_milliseconds", snapshot.gameLogicTimeMs);

    // Security metrics
    Logger::Debug("[PrometheusMetricsReporter::FormatPrometheusMetrics] Formatting security metrics: violations=%llu, malformed=%llu, speedHacks=%llu",
                 (unsigned long long)snapshot.securityViolations, (unsigned long long)snapshot.malformedPackets, (unsigned long long)snapshot.speedHackDetections);
    metrics << FormatMetricLine("security_violations_total", static_cast<double>(snapshot.securityViolations));
    metrics << FormatMetricLine("malformed_packets_total", static_cast<double>(snapshot.malformedPackets));
    metrics << FormatMetricLine("speed_hack_detections_total", static_cast<double>(snapshot.speedHackDetections));
    metrics << FormatMetricLine("players_kicked_total", static_cast<double>(snapshot.kickedPlayers));
    metrics << FormatMetricLine("players_banned_total", static_cast<double>(snapshot.bannedPlayers));

    // Computed metrics
    if (snapshot.totalKills + snapshot.totalDeaths > 0) {
        double killDeathRatio = snapshot.totalDeaths > 0 ?
            static_cast<double>(snapshot.totalKills) / snapshot.totalDeaths :
            static_cast<double>(snapshot.totalKills);
        metrics << FormatMetricLine("kill_death_ratio", killDeathRatio);
        Logger::Trace("[PrometheusMetricsReporter::FormatPrometheusMetrics] Added computed metric: kill_death_ratio=%.4f", killDeathRatio);
    } else {
        Logger::Trace("[PrometheusMetricsReporter::FormatPrometheusMetrics] Skipping kill_death_ratio (no kills or deaths)");
    }

    if (snapshot.totalPacketsProcessed > 0) {
        double packetLossPercent = (static_cast<double>(snapshot.totalPacketsDropped) /
                                  snapshot.totalPacketsProcessed) * 100.0;
        metrics << FormatMetricLine("packet_loss_percent", packetLossPercent);
        Logger::Trace("[PrometheusMetricsReporter::FormatPrometheusMetrics] Added computed metric: packet_loss_percent=%.4f", packetLossPercent);
    } else {
        Logger::Trace("[PrometheusMetricsReporter::FormatPrometheusMetrics] Skipping packet_loss_percent (no packets processed)");
    }

    // Reporter-specific metrics
    Logger::Debug("[PrometheusMetricsReporter::FormatPrometheusMetrics] Formatting reporter-specific metrics: httpRequests=%llu, reportsGenerated=%llu, reportsFailed=%llu",
                 (unsigned long long)m_httpRequests.load(), (unsigned long long)m_reportsGenerated.load(), (unsigned long long)m_reportsFailed.load());
    metrics << FormatMetricLine("prometheus_http_requests_total",
                               static_cast<double>(m_httpRequests.load()));
    metrics << FormatMetricLine("prometheus_reports_generated_total",
                               static_cast<double>(m_reportsGenerated.load()));
    metrics << FormatMetricLine("prometheus_reports_failed_total",
                               static_cast<double>(m_reportsFailed.load()));

    std::string result = metrics.str();
    Logger::Debug("[PrometheusMetricsReporter::FormatPrometheusMetrics] Total formatted metrics output: %zu bytes", result.length());
    Logger::Trace("[PrometheusMetricsReporter::FormatPrometheusMetrics] Exit - returning %zu bytes of Prometheus metrics", result.length());
    return result;
}

std::string PrometheusMetricsReporter::FormatMetricLine(
    const std::string& name,
    double value,
    const std::unordered_map<std::string, std::string>& labels) const {

    Logger::Trace("[PrometheusMetricsReporter::FormatMetricLine] Entry - name='%s', value=%.6f, labels count=%zu", name.c_str(), value, labels.size());

    // Check if metric should be excluded
    if (std::find(m_config.excludeMetrics.begin(), m_config.excludeMetrics.end(), name)
        != m_config.excludeMetrics.end()) {
        Logger::Debug("[PrometheusMetricsReporter::FormatMetricLine] Metric '%s' is in exclude list, returning empty string", name.c_str());
        Logger::Trace("[PrometheusMetricsReporter::FormatMetricLine] Exit - returning empty string (excluded)");
        return "";
    }

    std::ostringstream line;

    // Metric name with prefix
    line << m_config.metricsPrefix << "_" << name;

    // Labels (including static labels)
    std::vector<std::pair<std::string, std::string>> allLabels;

    // Add static labels first
    for (const auto& [key, val] : m_config.staticLabels) {
        allLabels.emplace_back(key, val);
    }

    // Add specific labels
    for (const auto& [key, val] : labels) {
        allLabels.emplace_back(key, val);
    }

    if (!allLabels.empty()) {
        Logger::Trace("[PrometheusMetricsReporter::FormatMetricLine] Appending %zu labels to metric '%s'", allLabels.size(), name.c_str());
        line << "{";
        bool first = true;
        for (const auto& [key, val] : allLabels) {
            if (!first) line << ",";
            line << key << "=\"" << EscapePrometheusString(val) << "\"";
            first = false;
        }
        line << "}";
    }

    // Value
    line << " ";
    if (std::isfinite(value)) {
        line << std::fixed << std::setprecision(6) << value;
        Logger::Trace("[PrometheusMetricsReporter::FormatMetricLine] Metric '%s' formatted with finite value: %.6f", name.c_str(), value);
    } else if (std::isnan(value)) {
        line << "NaN";
        Logger::Warn("[PrometheusMetricsReporter::FormatMetricLine] Metric '%s' has NaN value", name.c_str());
    } else if (std::isinf(value)) {
        line << (value > 0 ? "+Inf" : "-Inf");
        Logger::Warn("[PrometheusMetricsReporter::FormatMetricLine] Metric '%s' has infinite value: %s", name.c_str(), value > 0 ? "+Inf" : "-Inf");
    } else {
        line << "0";
        Logger::Warn("[PrometheusMetricsReporter::FormatMetricLine] Metric '%s' has unknown non-finite value, defaulting to 0", name.c_str());
    }

    // Timestamp (if enabled)
    if (m_config.enableTimestamps) {
        auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        line << " " << timestamp;
        Logger::Trace("[PrometheusMetricsReporter::FormatMetricLine] Appended timestamp %lld to metric '%s'", (long long)timestamp, name.c_str());
    }

    line << "\n";

    Logger::Trace("[PrometheusMetricsReporter::FormatMetricLine] Exit - returning formatted line for metric '%s'", name.c_str());
    return line.str();
}

std::string PrometheusMetricsReporter::EscapePrometheusString(const std::string& str) const {
    Logger::Trace("[PrometheusMetricsReporter::EscapePrometheusString] Entry - input length=%zu", str.length());
    std::string escaped;
    escaped.reserve(str.length() * 2); // Reserve extra space for escaping

    int escapedChars = 0;
    for (char c : str) {
        switch (c) {
            case '\\': escaped += "\\\\"; escapedChars++; break;
            case '"':  escaped += "\\\""; escapedChars++; break;
            case '\n': escaped += "\\n"; escapedChars++; break;
            case '\t': escaped += "\\t"; escapedChars++; break;
            case '\r': escaped += "\\r"; escapedChars++; break;
            default:   escaped += c; break;
        }
    }

    if (escapedChars > 0) {
        Logger::Trace("[PrometheusMetricsReporter::EscapePrometheusString] Escaped %d special characters, output length=%zu", escapedChars, escaped.length());
    }
    Logger::Trace("[PrometheusMetricsReporter::EscapePrometheusString] Exit - returning escaped string (length=%zu)", escaped.length());
    return escaped;
}

void PrometheusMetricsReporter::ReportError(const std::string& error) {
    std::lock_guard<std::mutex> lock(m_errorMutex);
    
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    
    std::ostringstream oss;
    oss << "[" << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S") << "] " << error;
    
    m_errors.push_back(oss.str());
    
    // Limit error history
    if (m_errors.size() > MAX_ERRORS) {
        m_errors.erase(m_errors.begin());
    }
    
    Logger::Error("PrometheusMetricsReporter: %s", error.c_str());
    m_healthy = false;
}

// Factory function implementation
namespace ReporterFactory {

std::unique_ptr<MetricsReporter> CreatePrometheusReporter(
    int port,
    const std::string& prefix) {
    
    PrometheusReporterConfig config;
    config.port = port;
    config.metricsPrefix = prefix;
    config.endpoint = "/metrics";
    config.enableTimestamps = false;
    
    // Add some standard static labels
    config.staticLabels["instance"] = "rs2v_server";
    config.staticLabels["version"] = "1.0.0";
    
    return std::make_unique<PrometheusMetricsReporter>(config);
}

} // namespace ReporterFactory

} // namespace Telemetry