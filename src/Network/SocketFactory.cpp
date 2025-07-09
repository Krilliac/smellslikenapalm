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
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        Logger::Error("SocketFactory: WSAStartup failed");
        return false;
    }
#endif
    return true;
}

void SocketFactory::Shutdown() {
#ifdef _WIN32
    WSACleanup();
#endif
}

SocketHandle SocketFactory::CreateUdpSocket(uint16_t localPort, const SocketConfig& cfg) {
    SocketHandle sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        Logger::Error("SocketFactory: UDP socket creation failed");
        return -1;
    }
    if (!ConfigureSocket(sock, cfg)) {
        close(sock);
        return -1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(localPort);
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        Logger::Error("SocketFactory: UDP bind to port %u failed", localPort);
        close(sock);
        return -1;
    }
    return sock;
}

SocketHandle SocketFactory::CreateTcpListenSocket(uint16_t localPort, int backlog, const SocketConfig& cfg) {
    SocketHandle sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        Logger::Error("SocketFactory: TCP socket creation failed");
        return -1;
    }
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    if (!ConfigureSocket(sock, cfg)) {
        close(sock);
        return -1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(localPort);
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        Logger::Error("SocketFactory: TCP bind to port %u failed", localPort);
        close(sock);
        return -1;
    }
    if (listen(sock, backlog) < 0) {
        Logger::Error("SocketFactory: listen() failed");
        close(sock);
        return -1;
    }
    return sock;
}

SocketHandle SocketFactory::CreateTcpClientSocket(const std::string& remoteIp, uint16_t remotePort, const SocketConfig& cfg) {
    SocketHandle sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        Logger::Error("SocketFactory: TCP client socket creation failed");
        return -1;
    }
    if (!ConfigureSocket(sock, cfg)) {
        close(sock);
        return -1;
    }
    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(remotePort);
    inet_pton(AF_INET, remoteIp.c_str(), &srv.sin_addr);
    if (connect(sock, (sockaddr*)&srv, sizeof(srv)) < 0) {
        Logger::Error("SocketFactory: connect to %s:%u failed", remoteIp.c_str(), remotePort);
        close(sock);
        return -1;
    }
    return sock;
}

bool SocketFactory::ConfigureSocket(SocketHandle sock, const SocketConfig& cfg) {
    // Timeouts
    timeval tout{};
    tout.tv_sec  = cfg.recvTimeout.count() / 1000;
    tout.tv_usec = (cfg.recvTimeout.count() % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&tout, sizeof(tout));
    tout.tv_sec  = cfg.sendTimeout.count() / 1000;
    tout.tv_usec = (cfg.sendTimeout.count() % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&tout, sizeof(tout));

    // Buffer sizes
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)&cfg.recvBufferSize, sizeof(cfg.recvBufferSize));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char*)&cfg.sendBufferSize, sizeof(cfg.sendBufferSize));

    // Non-blocking
#ifdef _WIN32
    u_long mode = cfg.nonBlocking ? 1 : 0;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (cfg.nonBlocking) flags |= O_NONBLOCK;
    else flags &= ~O_NONBLOCK;
    fcntl(sock, F_SETFL, flags);
#endif

    return true;
}