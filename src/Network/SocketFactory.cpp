// src/Network/SocketFactory.cpp
#include "Network/SocketFactory.h"
#include "Utils/Logger.h"

#ifdef _WIN32
  #include <ws2tcpip.h>
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <fcntl.h>
  #include <unistd.h>
#endif

bool SocketFactory::Initialize() {
    Logger::Trace("[SocketFactory::Initialize] Entry");
#ifdef _WIN32
    Logger::Debug("[SocketFactory::Initialize] Platform is Windows, calling WSAStartup");
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        Logger::Error("SocketFactory: WSAStartup failed");
        Logger::Trace("[SocketFactory::Initialize] Exit: returning false (WSAStartup failed)");
        return false;
    }
    Logger::Info("[SocketFactory::Initialize] WSAStartup succeeded: version=%d.%d",
                 LOBYTE(wsa.wVersion), HIBYTE(wsa.wVersion));
#else
    Logger::Debug("[SocketFactory::Initialize] Platform is POSIX, no WSA initialization needed");
#endif
    Logger::Info("[SocketFactory::Initialize] Socket factory initialized successfully");
    Logger::Trace("[SocketFactory::Initialize] Exit: returning true");
    return true;
}

void SocketFactory::Shutdown() {
    Logger::Trace("[SocketFactory::Shutdown] Entry");
#ifdef _WIN32
    Logger::Debug("[SocketFactory::Shutdown] Platform is Windows, calling WSACleanup");
    WSACleanup();
    Logger::Info("[SocketFactory::Shutdown] WSACleanup completed");
#else
    Logger::Debug("[SocketFactory::Shutdown] Platform is POSIX, no WSA cleanup needed");
#endif
    Logger::Trace("[SocketFactory::Shutdown] Exit");
}

SocketHandle SocketFactory::CreateUdpSocket(uint16_t localPort, const SocketConfig& cfg) {
    Logger::Trace("[SocketFactory::CreateUdpSocket] Entry: localPort=%u, nonBlocking=%s, recvBufSize=%d, sendBufSize=%d",
                  localPort, cfg.nonBlocking ? "true" : "false", cfg.recvBufferSize, cfg.sendBufferSize);
    SocketHandle sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        Logger::Error("SocketFactory: UDP socket creation failed");
        Logger::Debug("[SocketFactory::CreateUdpSocket] socket() returned %d", sock);
        Logger::Trace("[SocketFactory::CreateUdpSocket] Exit: returning -1 (socket creation failed)");
        return -1;
    }
    Logger::Debug("[SocketFactory::CreateUdpSocket] UDP socket created: fd=%d", sock);
    if (!ConfigureSocket(sock, cfg)) {
        Logger::Error("[SocketFactory::CreateUdpSocket] ConfigureSocket failed for fd=%d", sock);
        close(sock);
        Logger::Debug("[SocketFactory::CreateUdpSocket] Socket fd=%d closed after configuration failure", sock);
        Logger::Trace("[SocketFactory::CreateUdpSocket] Exit: returning -1 (configure failed)");
        return -1;
    }
    Logger::Debug("[SocketFactory::CreateUdpSocket] Socket fd=%d configured successfully", sock);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(localPort);
    Logger::Debug("[SocketFactory::CreateUdpSocket] Binding to INADDR_ANY:%u", localPort);
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        Logger::Error("SocketFactory: UDP bind to port %u failed", localPort);
        close(sock);
        Logger::Debug("[SocketFactory::CreateUdpSocket] Socket fd=%d closed after bind failure", sock);
        Logger::Trace("[SocketFactory::CreateUdpSocket] Exit: returning -1 (bind failed)");
        return -1;
    }
    Logger::Info("[SocketFactory::CreateUdpSocket] UDP socket bound successfully: fd=%d, port=%u", sock, localPort);
    Logger::Trace("[SocketFactory::CreateUdpSocket] Exit: returning fd=%d", sock);
    return sock;
}

SocketHandle SocketFactory::CreateTcpListenSocket(uint16_t localPort, int backlog, const SocketConfig& cfg) {
    Logger::Trace("[SocketFactory::CreateTcpListenSocket] Entry: localPort=%u, backlog=%d, nonBlocking=%s",
                  localPort, backlog, cfg.nonBlocking ? "true" : "false");
    SocketHandle sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        Logger::Error("SocketFactory: TCP socket creation failed");
        Logger::Debug("[SocketFactory::CreateTcpListenSocket] socket() returned %d", sock);
        Logger::Trace("[SocketFactory::CreateTcpListenSocket] Exit: returning -1 (socket creation failed)");
        return -1;
    }
    Logger::Debug("[SocketFactory::CreateTcpListenSocket] TCP socket created: fd=%d", sock);
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    Logger::Debug("[SocketFactory::CreateTcpListenSocket] SO_REUSEADDR set on fd=%d", sock);

    if (!ConfigureSocket(sock, cfg)) {
        Logger::Error("[SocketFactory::CreateTcpListenSocket] ConfigureSocket failed for fd=%d", sock);
        close(sock);
        Logger::Debug("[SocketFactory::CreateTcpListenSocket] Socket fd=%d closed after configuration failure", sock);
        Logger::Trace("[SocketFactory::CreateTcpListenSocket] Exit: returning -1 (configure failed)");
        return -1;
    }
    Logger::Debug("[SocketFactory::CreateTcpListenSocket] Socket fd=%d configured successfully", sock);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(localPort);
    Logger::Debug("[SocketFactory::CreateTcpListenSocket] Binding to INADDR_ANY:%u", localPort);
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        Logger::Error("SocketFactory: TCP bind to port %u failed", localPort);
        close(sock);
        Logger::Debug("[SocketFactory::CreateTcpListenSocket] Socket fd=%d closed after bind failure", sock);
        Logger::Trace("[SocketFactory::CreateTcpListenSocket] Exit: returning -1 (bind failed)");
        return -1;
    }
    Logger::Debug("[SocketFactory::CreateTcpListenSocket] Bound successfully, calling listen with backlog=%d", backlog);
    if (listen(sock, backlog) < 0) {
        Logger::Error("SocketFactory: listen() failed");
        close(sock);
        Logger::Debug("[SocketFactory::CreateTcpListenSocket] Socket fd=%d closed after listen failure", sock);
        Logger::Trace("[SocketFactory::CreateTcpListenSocket] Exit: returning -1 (listen failed)");
        return -1;
    }
    Logger::Info("[SocketFactory::CreateTcpListenSocket] TCP listen socket ready: fd=%d, port=%u, backlog=%d",
                 sock, localPort, backlog);
    Logger::Trace("[SocketFactory::CreateTcpListenSocket] Exit: returning fd=%d", sock);
    return sock;
}

SocketHandle SocketFactory::CreateTcpClientSocket(const std::string& remoteIp, uint16_t remotePort, const SocketConfig& cfg) {
    Logger::Trace("[SocketFactory::CreateTcpClientSocket] Entry: remoteIp='%s', remotePort=%u, nonBlocking=%s",
                  remoteIp.c_str(), remotePort, cfg.nonBlocking ? "true" : "false");
    SocketHandle sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        Logger::Error("SocketFactory: TCP client socket creation failed");
        Logger::Debug("[SocketFactory::CreateTcpClientSocket] socket() returned %d", sock);
        Logger::Trace("[SocketFactory::CreateTcpClientSocket] Exit: returning -1 (socket creation failed)");
        return -1;
    }
    Logger::Debug("[SocketFactory::CreateTcpClientSocket] TCP client socket created: fd=%d", sock);
    if (!ConfigureSocket(sock, cfg)) {
        Logger::Error("[SocketFactory::CreateTcpClientSocket] ConfigureSocket failed for fd=%d", sock);
        close(sock);
        Logger::Debug("[SocketFactory::CreateTcpClientSocket] Socket fd=%d closed after configuration failure", sock);
        Logger::Trace("[SocketFactory::CreateTcpClientSocket] Exit: returning -1 (configure failed)");
        return -1;
    }
    Logger::Debug("[SocketFactory::CreateTcpClientSocket] Socket fd=%d configured, connecting to %s:%u",
                  sock, remoteIp.c_str(), remotePort);
    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(remotePort);
    inet_pton(AF_INET, remoteIp.c_str(), &srv.sin_addr);
    if (connect(sock, (sockaddr*)&srv, sizeof(srv)) < 0) {
        Logger::Error("SocketFactory: connect to %s:%u failed", remoteIp.c_str(), remotePort);
        close(sock);
        Logger::Debug("[SocketFactory::CreateTcpClientSocket] Socket fd=%d closed after connect failure", sock);
        Logger::Trace("[SocketFactory::CreateTcpClientSocket] Exit: returning -1 (connect failed)");
        return -1;
    }
    Logger::Info("[SocketFactory::CreateTcpClientSocket] TCP client connected: fd=%d to %s:%u",
                 sock, remoteIp.c_str(), remotePort);
    Logger::Trace("[SocketFactory::CreateTcpClientSocket] Exit: returning fd=%d", sock);
    return sock;
}

bool SocketFactory::ConfigureSocket(SocketHandle sock, const SocketConfig& cfg) {
    Logger::Trace("[SocketFactory::ConfigureSocket] Entry: sock=%d, nonBlocking=%s, recvTimeout=%lld ms, sendTimeout=%lld ms",
                  sock, cfg.nonBlocking ? "true" : "false",
                  (long long)cfg.recvTimeout.count(), (long long)cfg.sendTimeout.count());
    // Timeouts
    timeval tout{};
    tout.tv_sec  = cfg.recvTimeout.count() / 1000;
    tout.tv_usec = (cfg.recvTimeout.count() % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&tout, sizeof(tout));
    Logger::Debug("[SocketFactory::ConfigureSocket] Set SO_RCVTIMEO: %ld sec, %ld usec on fd=%d",
                  tout.tv_sec, tout.tv_usec, sock);
    tout.tv_sec  = cfg.sendTimeout.count() / 1000;
    tout.tv_usec = (cfg.sendTimeout.count() % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&tout, sizeof(tout));
    Logger::Debug("[SocketFactory::ConfigureSocket] Set SO_SNDTIMEO: %ld sec, %ld usec on fd=%d",
                  tout.tv_sec, tout.tv_usec, sock);

    // Buffer sizes
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)&cfg.recvBufferSize, sizeof(cfg.recvBufferSize));
    Logger::Debug("[SocketFactory::ConfigureSocket] Set SO_RCVBUF=%d on fd=%d", cfg.recvBufferSize, sock);
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char*)&cfg.sendBufferSize, sizeof(cfg.sendBufferSize));
    Logger::Debug("[SocketFactory::ConfigureSocket] Set SO_SNDBUF=%d on fd=%d", cfg.sendBufferSize, sock);

    // Non-blocking
#ifdef _WIN32
    u_long mode = cfg.nonBlocking ? 1 : 0;
    ioctlsocket(sock, FIONBIO, &mode);
    Logger::Debug("[SocketFactory::ConfigureSocket] Set FIONBIO mode=%lu on fd=%d (Windows)", mode, sock);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (cfg.nonBlocking) flags |= O_NONBLOCK;
    else flags &= ~O_NONBLOCK;
    fcntl(sock, F_SETFL, flags);
    Logger::Debug("[SocketFactory::ConfigureSocket] Set fcntl flags=0x%X (nonBlocking=%s) on fd=%d",
                  flags, cfg.nonBlocking ? "true" : "false", sock);
#endif

    Logger::Info("[SocketFactory::ConfigureSocket] Socket fd=%d configured successfully", sock);
    Logger::Trace("[SocketFactory::ConfigureSocket] Exit: returning true");
    return true;
}
