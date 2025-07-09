// src/Network/TCPSocket.cpp

#include "Network/TCPSocket.h"
#include "Utils/Logger.h"

#ifdef _WIN32
  #include <ws2tcpip.h>
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <fcntl.h>
  #include <unistd.h>
  #include <cstring>
#endif

TCPSocket::TCPSocket() = default;

TCPSocket::~TCPSocket() {
    Close();
}

bool TCPSocket::Open(const SocketConfig& cfg) {
#ifdef _WIN32
    m_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
    m_sock = ::socket(AF_INET, SOCK_STREAM, 0);
#endif
    if (m_sock < 0) {
        Logger::Error("TCPSocket: socket() failed");
        return false;
    }
    return Configure(cfg);
}

bool TCPSocket::Connect(const std::string& remoteIp, uint16_t remotePort, const SocketConfig& cfg) {
    if (!Open(cfg)) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(remotePort);
    inet_pton(AF_INET, remoteIp.c_str(), &addr.sin_addr);
    if (::connect(m_sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        Logger::Error("TCPSocket: connect to %s:%u failed", remoteIp.c_str(), remotePort);
        Close();
        return false;
    }
    return true;
}

bool TCPSocket::Listen(uint16_t listenPort, int backlog, const SocketConfig& cfg) {
    if (!Open(cfg)) return false;
    int opt = 1;
    setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(listenPort);
    if (::bind(m_sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        Logger::Error("TCPSocket: bind() on port %u failed", listenPort);
        Close();
        return false;
    }
    if (::listen(m_sock, backlog) < 0) {
        Logger::Error("TCPSocket: listen() failed");
        Close();
        return false;
    }
    return true;
}

std::unique_ptr<TCPSocket> TCPSocket::Accept() {
    sockaddr_in clientAddr{};
    socklen_t addrLen = sizeof(clientAddr);
#ifdef _WIN32
    SOCKET clientSock = ::accept(m_sock, (sockaddr*)&clientAddr, &addrLen);
    if (clientSock == INVALID_SOCKET) return nullptr;
#else
    int clientSock = ::accept(m_sock, (sockaddr*)&clientAddr, &addrLen);
    if (clientSock < 0) return nullptr;
#endif
    auto client = std::make_unique<TCPSocket>();
    client->m_sock = clientSock;
    client->m_blocking = m_blocking;
    return client;
}

ssize_t TCPSocket::Send(const void* data, size_t len) {
    return ::send(m_sock, (const char*)data, (int)len, 0);
}

ssize_t TCPSocket::Receive(void* buffer, size_t len) {
    return ::recv(m_sock, (char*)buffer, (int)len, 0);
}

void TCPSocket::ShutdownSend() {
    if (m_sock>=0) ::shutdown(m_sock, 
#ifdef _WIN32
        SD_SEND
#else
        SHUT_WR
#endif
    );
}

void TCPSocket::ShutdownReceive() {
    if (m_sock>=0) ::shutdown(m_sock, 
#ifdef _WIN32
        SD_RECEIVE
#else
        SHUT_RD
#endif
    );
}

void TCPSocket::Close() {
    if (m_sock < 0) return;
#ifdef _WIN32
    closesocket(m_sock);
#else
    ::close(m_sock);
#endif
    m_sock = -1;
}

bool TCPSocket::IsOpen() const {
    return m_sock >= 0;
}

bool TCPSocket::IsBlocking() const {
    return m_blocking;
}

void TCPSocket::SetBlocking(bool blocking) {
#ifdef _WIN32
    u_long mode = blocking ? 0 : 1;
    ioctlsocket(m_sock, FIONBIO, &mode);
#else
    int flags = fcntl(m_sock, F_GETFL, 0);
    if (blocking) flags &= ~O_NONBLOCK;
    else flags |= O_NONBLOCK;
    fcntl(m_sock, F_SETFL, flags);
#endif
    m_blocking = blocking;
}

void TCPSocket::SetRecvTimeout(std::chrono::milliseconds timeout) {
    timeval tv{ 
        .tv_sec = (long)(timeout.count()/1000),
        .tv_usec = (long)((timeout.count()%1000)*1000)
    };
    setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));
}

void TCPSocket::SetSendTimeout(std::chrono::milliseconds timeout) {
    timeval tv{ 
        .tv_sec = (long)(timeout.count()/1000),
        .tv_usec = (long)((timeout.count()%1000)*1000)
    };
    setsockopt(m_sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&tv, sizeof(tv));
}

bool TCPSocket::Configure(const SocketConfig& cfg) {
    SetBlocking(!cfg.nonBlocking);
    SetRecvTimeout(cfg.recvTimeout);
    SetSendTimeout(cfg.sendTimeout);
    setsockopt(m_sock, SOL_SOCKET, SO_RCVBUF, (char*)&cfg.recvBufferSize, sizeof(cfg.recvBufferSize));
    setsockopt(m_sock, SOL_SOCKET, SO_SNDBUF, (char*)&cfg.sendBufferSize, sizeof(cfg.sendBufferSize));
    return true;
}