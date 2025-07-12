// Server/telemetry/PrometheusReporter.cpp
// Implementation of Prometheus HTTP endpoint metrics reporter
// for the RS2V server telemetry system

#include "MetricsReporter.h"
#include "TelemetryManager.h"
#include "../Utils/Logger.h"

#include <sstream>
#include <iomanip>
#include <algorithm>
#include <regex>
#include <chrono>
#include <thread>

// Platform-specific networking includes
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef int socklen_t;
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <signal.h>
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR -1
    #define closesocket close
    typedef int SOCKET;
#endif

namespace Telemetry {

PrometheusMetricsReporter::PrometheusMetricsReporter(const PrometheusReporterConfig& config)
    : m_config(config), m_healthy(true), m_serverRunning(false), 
      m_reportsGenerated(0), m_reportsFailed(0), m_httpRequests(0),
      m_serverSocket(INVALID_SOCKET) {
    
    // Validate configuration
    if (m_config.port <= 0 || m_config.port > 65535) {
        m_config.port = 9090; // Default Prometheus port
    }
    if (m_config.endpoint.empty()) {
        m_config.endpoint = "/metrics";
    }
    if (m_config.metricsPrefix.empty()) {
        m_config.metricsPrefix = "rs2v_server";
    }
    
    // Ensure endpoint starts with /
    if (m_config.endpoint[0] != '/') {
        m_config.endpoint = "/" + m_config.endpoint;
    }
    
    Logger::Info("PrometheusMetricsReporter created with config: port=%d, endpoint=%s, prefix=%s",
                m_config.port, m_config.endpoint.c_str(), m_config.metricsPrefix.c_str());

#ifdef _WIN32
    // Initialize Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        ReportError("Failed to initialize Winsock");
        m_healthy = false;
    }
#else
    // Ignore SIGPIPE for robustness
    signal(SIGPIPE, SIG_IGN);
#endif
}

PrometheusMetricsReporter::~PrometheusMetricsReporter() {
    Shutdown();
    
#ifdef _WIN32
    WSACleanup();
#endif
}

bool PrometheusMetricsReporter::Initialize(const std::string& outputDirectory) {
    // outputDirectory is not used for Prometheus reporter but kept for interface compliance
    (void)outputDirectory;
    
    try {
        if (!CreateServerSocket()) {
            ReportError("Failed to create HTTP server socket");
            m_healthy = false;
            return false;
        }
        
        StartHTTPServer();
        
        m_healthy = true;
        Logger::Info("PrometheusMetricsReporter initialized successfully on port %d", m_config.port);
        return true;
        
    } catch (const std::exception& ex) {
        ReportError("Failed to initialize PrometheusMetricsReporter: " + std::string(ex.what()));
        m_healthy = false;
        return false;
    }
}

void PrometheusMetricsReporter::Shutdown() {
    StopHTTPServer();
    
    Logger::Info("PrometheusMetricsReporter shutdown complete. Served %llu HTTP requests total.",
                m_httpRequests.load());
}

void PrometheusMetricsReporter::Report(const MetricsSnapshot& snapshot) {
    if (!m_healthy.load()) {
        m_reportsFailed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    
    try {
        // Store the latest snapshot for HTTP responses
        {
            std::lock_guard<std::mutex> lock(m_snapshotMutex);
            m_latestSnapshot = snapshot;
        }
        
        m_reportsGenerated.fetch_add(1, std::memory_order_relaxed);
        
    } catch (const std::exception& ex) {
        ReportError("Error updating Prometheus metrics: " + std::string(ex.what()));
        m_reportsFailed.fetch_add(1, std::memory_order_relaxed);
    }
}

std::vector<std::string> PrometheusMetricsReporter::GetLastErrors() const {
    std::lock_guard<std::mutex> lock(m_errorMutex);
    return m_errors;
}

void PrometheusMetricsReporter::ClearErrors() {
    std::lock_guard<std::mutex> lock(m_errorMutex);
    m_errors.clear();
    m_healthy = true;
}

std::string PrometheusMetricsReporter::GetMetricsEndpoint() const {
    return "http://localhost:" + std::to_string(m_config.port) + m_config.endpoint;
}

void PrometheusMetricsReporter::StartHTTPServer() {
    if (m_serverRunning.exchange(true)) {
        Logger::Warn("Prometheus HTTP server already running");
        return;
    }
    
    Logger::Info("Starting Prometheus HTTP server on port %d", m_config.port);
    
    m_serverThread = std::thread(&PrometheusMetricsReporter::HTTPServerLoop, this);
}

void PrometheusMetricsReporter::StopHTTPServer() {
    if (!m_serverRunning.exchange(false)) {
        return;
    }
    
    Logger::Info("Stopping Prometheus HTTP server...");
    
    // Close server socket to break accept loop
    if (m_serverSocket != INVALID_SOCKET) {
        closesocket(m_serverSocket);
        m_serverSocket = INVALID_SOCKET;
    }
    
    if (m_serverThread.joinable()) {
        m_serverThread.join();
    }
    
    Logger::Info("Prometheus HTTP server stopped");
}

bool PrometheusMetricsReporter::CreateServerSocket() {
    m_serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_serverSocket == INVALID_SOCKET) {
        ReportError("Failed to create socket");
        return false;
    }
    
    // Set socket options
    int optval = 1;
    if (setsockopt(m_serverSocket, SOL_SOCKET, SO_REUSEADDR, 
                   reinterpret_cast<const char*>(&optval), sizeof(optval)) == SOCKET_ERROR) {
        Logger::Warn("Failed to set SO_REUSEADDR on Prometheus socket");
    }
    
#ifndef _WIN32
    // Set non-blocking mode for accept timeout
    int flags = fcntl(m_serverSocket, F_GETFL, 0);
    if (flags != -1) {
        fcntl(m_serverSocket, F_SETFL, flags | O_NONBLOCK);
    }
#endif
    
    // Bind socket
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(static_cast<uint16_t>(m_config.port));
    
    if (bind(m_serverSocket, reinterpret_cast<struct sockaddr*>(&serverAddr), 
             sizeof(serverAddr)) == SOCKET_ERROR) {
        ReportError("Failed to bind socket to port " + std::to_string(m_config.port));
        closesocket(m_serverSocket);
        m_serverSocket = INVALID_SOCKET;
        return false;
    }
    
    // Listen for connections
    if (listen(m_serverSocket, 5) == SOCKET_ERROR) {
        ReportError("Failed to listen on socket");
        closesocket(m_serverSocket);
        m_serverSocket = INVALID_SOCKET;
        return false;
    }
    
    return true;
}

void PrometheusMetricsReporter::HTTPServerLoop() {
    Logger::Info("Prometheus HTTP server loop started");
    
    while (m_serverRunning.load()) {
        try {
            struct sockaddr_in clientAddr;
            socklen_t clientAddrLen = sizeof(clientAddr);
            
#ifdef _WIN32
            // Windows blocking accept with timeout
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(m_serverSocket, &readfds);
            
            struct timeval timeout;
            timeout.tv_sec = 1;
            timeout.tv_usec = 0;
            
            int selectResult = select(0, &readfds, nullptr, nullptr, &timeout);
            if (selectResult == 0) {
                continue; // Timeout, check if we should stop
            }
            if (selectResult == SOCKET_ERROR) {
                if (m_serverRunning.load()) {
                    ReportError("Select error in HTTP server loop");
                }
                break;
            }
#endif
            
            SOCKET clientSocket = accept(m_serverSocket, 
                                       reinterpret_cast<struct sockaddr*>(&clientAddr), 
                                       &clientAddrLen);
            
            if (clientSocket == INVALID_SOCKET) {
#ifdef _WIN32
                int error = WSAGetLastError();
                if (error != WSAEWOULDBLOCK && m_serverRunning.load()) {
                    ReportError("Accept failed with error: " + std::to_string(error));
                }
#else
                if (errno != EAGAIN && errno != EWOULDBLOCK && m_serverRunning.load()) {
                    ReportError("Accept failed: " + std::string(strerror(errno)));
                }
#endif
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            
            // Handle client connection in separate thread for non-blocking operation
            std::thread clientThread(&PrometheusMetricsReporter::HandleClientConnection, 
                                   this, clientSocket);
            clientThread.detach();
            
        } catch (const std::exception& ex) {
            if (m_serverRunning.load()) {
                ReportError("Exception in HTTP server loop: " + std::string(ex.what()));
            }
        }
    }
    
    Logger::Info("Prometheus HTTP server loop stopped");
}

void PrometheusMetricsReporter::HandleClientConnection(int clientSocket) {
    try {
        // Set socket timeout
        struct timeval timeout;
        timeout.tv_sec = 5;  // 5 second timeout
        timeout.tv_usec = 0;
        setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, 
                  reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, 
                  reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        
        // Read HTTP request
        char buffer[4096];
        int bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
        
        if (bytesReceived <= 0) {
            closesocket(clientSocket);
            return;
        }
        
        buffer[bytesReceived] = '\0';
        std::string request(buffer);
        
        // Parse HTTP request line
        std::istringstream requestStream(request);
        std::string method, path, protocol;
        requestStream >> method >> path >> protocol;
        
        m_httpRequests.fetch_add(1, std::memory_order_relaxed);
        
        std::string response;
        
        if (method == "GET" && path == m_config.endpoint) {
            // Generate metrics response
            std::string metricsData = FormatPrometheusMetrics();
            
            response = "HTTP/1.1 200 OK\r\n";
            response += "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n";
            response += "Content-Length: " + std::to_string(metricsData.length()) + "\r\n";
            response += "Connection: close\r\n";
            response += "\r\n";
            response += metricsData;
            
        } else if (method == "GET" && path == "/") {
            // Root path - provide basic info
            std::string info = "RS2V Server Prometheus Metrics\n";
            info += "Metrics endpoint: " + m_config.endpoint + "\n";
            info += "Server version: 1.0.0\n";
            
            response = "HTTP/1.1 200 OK\r\n";
            response += "Content-Type: text/plain\r\n";
            response += "Content-Length: " + std::to_string(info.length()) + "\r\n";
            response += "Connection: close\r\n";
            response += "\r\n";
            response += info;
            
        } else {
            // 404 Not Found
            std::string notFound = "404 Not Found\nTry: " + m_config.endpoint + "\n";
            
            response = "HTTP/1.1 404 Not Found\r\n";
            response += "Content-Type: text/plain\r\n";
            response += "Content-Length: " + std::to_string(notFound.length()) + "\r\n";
            response += "Connection: close\r\n";
            response += "\r\n";
            response += notFound;
        }
        
        // Send response
        send(clientSocket, response.c_str(), static_cast<int>(response.length()), 0);
        
    } catch (const std::exception& ex) {
        Logger::Error("Error handling Prometheus HTTP client: %s", ex.what());
    }
    
    closesocket(clientSocket);
}

std::string PrometheusMetricsReporter::FormatPrometheusMetrics() const {
    std::ostringstream metrics;
    
    MetricsSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(m_snapshotMutex);
        snapshot = m_latestSnapshot;
    }
    
    // Add timestamp comment
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    
    metrics << "# Generated at " << timestamp << "\n";
    metrics << "# RS2V Server Metrics\n\n";
    
    // System metrics
    if (snapshot.cpuUsagePercent >= 0.0) {
        metrics << FormatMetricLine("cpu_usage_percent", snapshot.cpuUsagePercent);
    }
    
    if (snapshot.memoryUsedBytes > 0) {
        metrics << FormatMetricLine("memory_used_bytes", static_cast<double>(snapshot.memoryUsedBytes));
        metrics << FormatMetricLine("memory_total_bytes", static_cast<double>(snapshot.memoryTotalBytes));
        
        if (snapshot.memoryTotalBytes > 0) {
            double memoryUsagePercent = (static_cast<double>(snapshot.memoryUsedBytes) / 
                                       snapshot.memoryTotalBytes) * 100.0;
            metrics << FormatMetricLine("memory_usage_percent", memoryUsagePercent);
        }
    }
    
    metrics << FormatMetricLine("network_bytes_sent_total", static_cast<double>(snapshot.networkBytesSent));
    metrics << FormatMetricLine("network_bytes_received_total", static_cast<double>(snapshot.networkBytesReceived));
    metrics << FormatMetricLine("disk_read_bytes_total", static_cast<double>(snapshot.diskReadBytes));
    metrics << FormatMetricLine("disk_write_bytes_total", static_cast<double>(snapshot.diskWriteBytes));
    
    // Network application metrics
    metrics << FormatMetricLine("active_connections", static_cast<double>(snapshot.activeConnections));
    metrics << FormatMetricLine("authenticated_players", static_cast<double>(snapshot.authenticatedPlayers));
    metrics << FormatMetricLine("packets_processed_total", static_cast<double>(snapshot.totalPacketsProcessed));
    metrics << FormatMetricLine("packets_dropped_total", static_cast<double>(snapshot.totalPacketsDropped));
    metrics << FormatMetricLine("average_latency_milliseconds", snapshot.averageLatencyMs);
    metrics << FormatMetricLine("packet_loss_rate", snapshot.packetLossRate);
    
    // Gameplay metrics
    metrics << FormatMetricLine("current_tick", static_cast<double>(snapshot.currentTick));
    metrics << FormatMetricLine("active_matches", static_cast<double>(snapshot.activeMatches));
    metrics << FormatMetricLine("kills_total", static_cast<double>(snapshot.totalKills));
    metrics << FormatMetricLine("deaths_total", static_cast<double>(snapshot.totalDeaths));
    metrics << FormatMetricLine("objectives_captured_total", static_cast<double>(snapshot.objectivesCaptured));
    metrics << FormatMetricLine("chat_messages_sent_total", static_cast<double>(snapshot.chatMessagesSent));
    
    // Performance metrics
    metrics << FormatMetricLine("frame_time_milliseconds", snapshot.frameTimeMs);
    metrics << FormatMetricLine("physics_time_milliseconds", snapshot.physicsTimeMs);
    metrics << FormatMetricLine("network_time_milliseconds", snapshot.networkTimeMs);
    metrics << FormatMetricLine("game_logic_time_milliseconds", snapshot.gameLogicTimeMs);
    
    // Security metrics
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
    }
    
    if (snapshot.totalPacketsProcessed > 0) {
        double packetLossPercent = (static_cast<double>(snapshot.totalPacketsDropped) / 
                                  snapshot.totalPacketsProcessed) * 100.0;
        metrics << FormatMetricLine("packet_loss_percent", packetLossPercent);
    }
    
    // Reporter-specific metrics
    metrics << FormatMetricLine("prometheus_http_requests_total", 
                               static_cast<double>(m_httpRequests.load()));
    metrics << FormatMetricLine("prometheus_reports_generated_total", 
                               static_cast<double>(m_reportsGenerated.load()));
    metrics << FormatMetricLine("prometheus_reports_failed_total", 
                               static_cast<double>(m_reportsFailed.load()));
    
    return metrics.str();
}

std::string PrometheusMetricsReporter::FormatMetricLine(
    const std::string& name, 
    double value, 
    const std::unordered_map<std::string, std::string>& labels) const {
    
    // Check if metric should be excluded
    if (std::find(m_config.excludeMetrics.begin(), m_config.excludeMetrics.end(), name) 
        != m_config.excludeMetrics.end()) {
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
    } else if (std::isnan(value)) {
        line << "NaN";
    } else if (std::isinf(value)) {
        line << (value > 0 ? "+Inf" : "-Inf");
    } else {
        line << "0";
    }
    
    // Timestamp (if enabled)
    if (m_config.enableTimestamps) {
        auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        line << " " << timestamp;
    }
    
    line << "\n";
    
    return line.str();
}

std::string PrometheusMetricsReporter::EscapePrometheusString(const std::string& str) const {
    std::string escaped;
    escaped.reserve(str.length() * 2); // Reserve extra space for escaping
    
    for (char c : str) {
        switch (c) {
            case '\\': escaped += "\\\\"; break;
            case '"':  escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\t': escaped += "\\t"; break;
            case '\r': escaped += "\\r"; break;
            default:   escaped += c; break;
        }
    }
    
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