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

TCPSocket::TCPSocket() {
    Logger::Trace("[TCPSocket::TCPSocket] Entry: default constructor");
    Logger::Debug("[TCPSocket::TCPSocket] Created TCPSocket with m_sock=-1 (uninitialized)");
    Logger::Trace("[TCPSocket::TCPSocket] Exit");
}

TCPSocket::~TCPSocket() {
    Logger::Trace("[TCPSocket::~TCPSocket] Entry: destructor, m_sock=%d", m_sock);
    Logger::Debug("[TCPSocket::~TCPSocket] Destroying TCPSocket, calling Close()");
    Close();
    Logger::Trace("[TCPSocket::~TCPSocket] Exit");
}

bool TCPSocket::Open(const SocketConfig& cfg) {
    Logger::Trace("[TCPSocket::Open] Entry: nonBlocking=%s", cfg.nonBlocking ? "true" : "false");
#ifdef _WIN32
    Logger::Debug("[TCPSocket::Open] Platform is Windows, creating SOCK_STREAM with IPPROTO_TCP");
    m_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
    Logger::Debug("[TCPSocket::Open] Platform is POSIX, creating SOCK_STREAM");
    m_sock = ::socket(AF_INET, SOCK_STREAM, 0);
#endif
    if (m_sock < 0) {
        Logger::Error("TCPSocket: socket() failed");
        Logger::Debug("[TCPSocket::Open] socket() returned %d", m_sock);
        Logger::Trace("[TCPSocket::Open] Exit: returning false (socket creation failed)");
        return false;
    }
    Logger::Debug("[TCPSocket::Open] Socket created: fd=%d", m_sock);
    bool configResult = Configure(cfg);
    Logger::Debug("[TCPSocket::Open] Configure returned %s for fd=%d", configResult ? "true" : "false", m_sock);
    Logger::Trace("[TCPSocket::Open] Exit: returning %s", configResult ? "true" : "false");
    return configResult;
}

bool TCPSocket::Connect(const std::string& remoteIp, uint16_t remotePort, const SocketConfig& cfg) {
    Logger::Trace("[TCPSocket::Connect] Entry: remoteIp='%s', remotePort=%u", remoteIp.c_str(), remotePort);
    if (!Open(cfg)) {
        Logger::Debug("[TCPSocket::Connect] Open() failed, cannot connect");
        Logger::Trace("[TCPSocket::Connect] Exit: returning false (Open failed)");
        return false;
    }
    Logger::Debug("[TCPSocket::Connect] Socket opened, connecting to %s:%u", remoteIp.c_str(), remotePort);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(remotePort);
    inet_pton(AF_INET, remoteIp.c_str(), &addr.sin_addr);
    if (::connect(m_sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        Logger::Error("TCPSocket: connect to %s:%u failed", remoteIp.c_str(), remotePort);
        Logger::Debug("[TCPSocket::Connect] connect() failed on fd=%d to %s:%u", m_sock, remoteIp.c_str(), remotePort);
        Close();
        Logger::Trace("[TCPSocket::Connect] Exit: returning false (connect failed)");
        return false;
    }
    Logger::Info("[TCPSocket::Connect] Connected successfully: fd=%d to %s:%u", m_sock, remoteIp.c_str(), remotePort);
    Logger::Trace("[TCPSocket::Connect] Exit: returning true");
    return true;
}

bool TCPSocket::Listen(uint16_t listenPort, int backlog, const SocketConfig& cfg) {
    Logger::Trace("[TCPSocket::Listen] Entry: listenPort=%u, backlog=%d", listenPort, backlog);
    if (!Open(cfg)) {
        Logger::Debug("[TCPSocket::Listen] Open() failed, cannot listen");
        Logger::Trace("[TCPSocket::Listen] Exit: returning false (Open failed)");
        return false;
    }
    Logger::Debug("[TCPSocket::Listen] Socket opened, setting SO_REUSEADDR on fd=%d", m_sock);
    int opt = 1;
    setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(listenPort);
    Logger::Debug("[TCPSocket::Listen] Binding to INADDR_ANY:%u on fd=%d", listenPort, m_sock);
    if (::bind(m_sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        Logger::Error("TCPSocket: bind() on port %u failed", listenPort);
        Logger::Debug("[TCPSocket::Listen] bind() failed on fd=%d, port=%u", m_sock, listenPort);
        Close();
        Logger::Trace("[TCPSocket::Listen] Exit: returning false (bind failed)");
        return false;
    }
    Logger::Debug("[TCPSocket::Listen] Bound successfully, calling listen with backlog=%d on fd=%d", backlog, m_sock);
    if (::listen(m_sock, backlog) < 0) {
        Logger::Error("TCPSocket: listen() failed");
        Logger::Debug("[TCPSocket::Listen] listen() failed on fd=%d", m_sock);
        Close();
        Logger::Trace("[TCPSocket::Listen] Exit: returning false (listen failed)");
        return false;
    }
    Logger::Info("[TCPSocket::Listen] Listening on port %u with backlog=%d, fd=%d", listenPort, backlog, m_sock);
    Logger::Trace("[TCPSocket::Listen] Exit: returning true");
    return true;
}

std::unique_ptr<TCPSocket> TCPSocket::Accept() {
    Logger::Trace("[TCPSocket::Accept] Entry: listening on fd=%d", m_sock);
    sockaddr_in clientAddr{};
    socklen_t addrLen = sizeof(clientAddr);
#ifdef _WIN32
    SOCKET clientSock = ::accept(m_sock, (sockaddr*)&clientAddr, &addrLen);
    if (clientSock == INVALID_SOCKET) {
        Logger::Debug("[TCPSocket::Accept] accept() returned INVALID_SOCKET on fd=%d", m_sock);
        Logger::Trace("[TCPSocket::Accept] Exit: returning nullptr (INVALID_SOCKET)");
        return nullptr;
    }
    Logger::Debug("[TCPSocket::Accept] Accepted client: clientSock=%lld", (long long)clientSock);
#else
    int clientSock = ::accept(m_sock, (sockaddr*)&clientAddr, &addrLen);
    if (clientSock < 0) {
        Logger::Debug("[TCPSocket::Accept] accept() returned %d on fd=%d (no pending connection or error)",
                      clientSock, m_sock);
        Logger::Trace("[TCPSocket::Accept] Exit: returning nullptr (accept failed)");
        return nullptr;
    }
    Logger::Debug("[TCPSocket::Accept] Accepted client: clientSock=%d from %s:%u",
                  clientSock, inet_ntoa(clientAddr.sin_addr), ntohs(clientAddr.sin_port));
#endif
    auto client = std::make_unique<TCPSocket>();
    client->m_sock = clientSock;
    client->m_blocking = m_blocking;
    Logger::Info("[TCPSocket::Accept] Accepted new connection: clientFd=%d, blocking=%s",
                 clientSock, m_blocking ? "true" : "false");
    Logger::Trace("[TCPSocket::Accept] Exit: returning client socket");
    return client;
}

ssize_t TCPSocket::Send(const void* data, size_t len) {
    Logger::Trace("[TCPSocket::Send] Entry: fd=%d, data=%p, len=%zu", m_sock, data, len);
    ssize_t sent = ::send(m_sock, (const char*)data, (int)len, 0);
    if (sent < 0) {
        Logger::Error("[TCPSocket::Send] send() failed on fd=%d, requested %zu bytes, returned %zd",
                      m_sock, len, sent);
    } else if (sent < (ssize_t)len) {
        Logger::Warn("[TCPSocket::Send] Partial send on fd=%d: sent %zd of %zu bytes", m_sock, sent, len);
    } else {
        Logger::Debug("[TCPSocket::Send] Sent %zd bytes on fd=%d", sent, m_sock);
    }
    Logger::Trace("[TCPSocket::Send] Exit: returning %zd", sent);
    return sent;
}

ssize_t TCPSocket::Receive(void* buffer, size_t len) {
    Logger::Trace("[TCPSocket::Receive] Entry: fd=%d, buffer=%p, len=%zu", m_sock, buffer, len);
    ssize_t received = ::recv(m_sock, (char*)buffer, (int)len, 0);
    if (received < 0) {
        Logger::Debug("[TCPSocket::Receive] recv() returned %zd on fd=%d (error or would-block)", received, m_sock);
    } else if (received == 0) {
        Logger::Debug("[TCPSocket::Receive] recv() returned 0 on fd=%d (connection closed by peer)", m_sock);
    } else {
        Logger::Debug("[TCPSocket::Receive] Received %zd bytes on fd=%d", received, m_sock);
    }
    Logger::Trace("[TCPSocket::Receive] Exit: returning %zd", received);
    return received;
}

void TCPSocket::ShutdownSend() {
    Logger::Trace("[TCPSocket::ShutdownSend] Entry: fd=%d", m_sock);
    if (m_sock>=0) {
        Logger::Debug("[TCPSocket::ShutdownSend] Shutting down send side of fd=%d", m_sock);
        ::shutdown(m_sock,
#ifdef _WIN32
        SD_SEND
#else
        SHUT_WR
#endif
    );
        Logger::Info("[TCPSocket::ShutdownSend] Send side shutdown on fd=%d", m_sock);
    } else {
        Logger::Debug("[TCPSocket::ShutdownSend] Socket not open (fd=%d), nothing to shutdown", m_sock);
    }
    Logger::Trace("[TCPSocket::ShutdownSend] Exit");
}

void TCPSocket::ShutdownReceive() {
    Logger::Trace("[TCPSocket::ShutdownReceive] Entry: fd=%d", m_sock);
    if (m_sock>=0) {
        Logger::Debug("[TCPSocket::ShutdownReceive] Shutting down receive side of fd=%d", m_sock);
        ::shutdown(m_sock,
#ifdef _WIN32
        SD_RECEIVE
#else
        SHUT_RD
#endif
    );
        Logger::Info("[TCPSocket::ShutdownReceive] Receive side shutdown on fd=%d", m_sock);
    } else {
        Logger::Debug("[TCPSocket::ShutdownReceive] Socket not open (fd=%d), nothing to shutdown", m_sock);
    }
    Logger::Trace("[TCPSocket::ShutdownReceive] Exit");
}

void TCPSocket::Close() {
    Logger::Trace("[TCPSocket::Close] Entry: fd=%d", m_sock);
    if (m_sock < 0) {
        Logger::Debug("[TCPSocket::Close] Socket already closed (fd=%d), nothing to do", m_sock);
        Logger::Trace("[TCPSocket::Close] Exit: already closed");
        return;
    }
    Logger::Debug("[TCPSocket::Close] Closing socket fd=%d", m_sock);
#ifdef _WIN32
    closesocket(m_sock);
    Logger::Debug("[TCPSocket::Close] closesocket() called on fd=%d (Windows)", m_sock);
#else
    ::close(m_sock);
    Logger::Debug("[TCPSocket::Close] close() called on fd=%d (POSIX)", m_sock);
#endif
    m_sock = -1;
    Logger::Info("[TCPSocket::Close] Socket closed, fd reset to -1");
    Logger::Trace("[TCPSocket::Close] Exit");
}

bool TCPSocket::IsOpen() const {
    Logger::Trace("[TCPSocket::IsOpen] Entry: fd=%d", m_sock);
    bool open = m_sock >= 0;
    Logger::Debug("[TCPSocket::IsOpen] fd=%d, returning %s", m_sock, open ? "true" : "false");
    Logger::Trace("[TCPSocket::IsOpen] Exit: returning %s", open ? "true" : "false");
    return open;
}

bool TCPSocket::IsBlocking() const {
    Logger::Trace("[TCPSocket::IsBlocking] Entry: m_blocking=%s", m_blocking ? "true" : "false");
    Logger::Trace("[TCPSocket::IsBlocking] Exit: returning %s", m_blocking ? "true" : "false");
    return m_blocking;
}

void TCPSocket::SetBlocking(bool blocking) {
    Logger::Trace("[TCPSocket::SetBlocking] Entry: blocking=%s, current m_blocking=%s, fd=%d",
                  blocking ? "true" : "false", m_blocking ? "true" : "false", m_sock);
#ifdef _WIN32
    u_long mode = blocking ? 0 : 1;
    ioctlsocket(m_sock, FIONBIO, &mode);
    Logger::Debug("[TCPSocket::SetBlocking] Set FIONBIO mode=%lu on fd=%d (Windows)", mode, m_sock);
#else
    int flags = fcntl(m_sock, F_GETFL, 0);
    if (blocking) flags &= ~O_NONBLOCK;
    else flags |= O_NONBLOCK;
    fcntl(m_sock, F_SETFL, flags);
    Logger::Debug("[TCPSocket::SetBlocking] Set fcntl flags=0x%X on fd=%d (blocking=%s)",
                  flags, m_sock, blocking ? "true" : "false");
#endif
    m_blocking = blocking;
    Logger::Debug("[TCPSocket::SetBlocking] m_blocking updated to %s", m_blocking ? "true" : "false");
    Logger::Trace("[TCPSocket::SetBlocking] Exit");
}

void TCPSocket::SetRecvTimeout(std::chrono::milliseconds timeout) {
    Logger::Trace("[TCPSocket::SetRecvTimeout] Entry: timeout=%lld ms, fd=%d", (long long)timeout.count(), m_sock);
    timeval tv{
        .tv_sec = (long)(timeout.count()/1000),
        .tv_usec = (long)((timeout.count()%1000)*1000)
    };
    setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));
    Logger::Debug("[TCPSocket::SetRecvTimeout] Set SO_RCVTIMEO: %ld sec, %ld usec on fd=%d",
                  tv.tv_sec, tv.tv_usec, m_sock);
    Logger::Trace("[TCPSocket::SetRecvTimeout] Exit");
}

void TCPSocket::SetSendTimeout(std::chrono::milliseconds timeout) {
    Logger::Trace("[TCPSocket::SetSendTimeout] Entry: timeout=%lld ms, fd=%d", (long long)timeout.count(), m_sock);
    timeval tv{
        .tv_sec = (long)(timeout.count()/1000),
        .tv_usec = (long)((timeout.count()%1000)*1000)
    };
    setsockopt(m_sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&tv, sizeof(tv));
    Logger::Debug("[TCPSocket::SetSendTimeout] Set SO_SNDTIMEO: %ld sec, %ld usec on fd=%d",
                  tv.tv_sec, tv.tv_usec, m_sock);
    Logger::Trace("[TCPSocket::SetSendTimeout] Exit");
}

bool TCPSocket::Configure(const SocketConfig& cfg) {
    Logger::Trace("[TCPSocket::Configure] Entry: fd=%d, nonBlocking=%s, recvTimeout=%lld ms, sendTimeout=%lld ms, recvBufSize=%d, sendBufSize=%d",
                  m_sock, cfg.nonBlocking ? "true" : "false",
                  (long long)cfg.recvTimeout.count(), (long long)cfg.sendTimeout.count(),
                  cfg.recvBufferSize, cfg.sendBufferSize);
    SetBlocking(!cfg.nonBlocking);
    Logger::Debug("[TCPSocket::Configure] SetBlocking(%s) completed", cfg.nonBlocking ? "false" : "true");
    SetRecvTimeout(cfg.recvTimeout);
    Logger::Debug("[TCPSocket::Configure] SetRecvTimeout completed");
    SetSendTimeout(cfg.sendTimeout);
    Logger::Debug("[TCPSocket::Configure] SetSendTimeout completed");
    setsockopt(m_sock, SOL_SOCKET, SO_RCVBUF, (char*)&cfg.recvBufferSize, sizeof(cfg.recvBufferSize));
    Logger::Debug("[TCPSocket::Configure] Set SO_RCVBUF=%d on fd=%d", cfg.recvBufferSize, m_sock);
    setsockopt(m_sock, SOL_SOCKET, SO_SNDBUF, (char*)&cfg.sendBufferSize, sizeof(cfg.sendBufferSize));
    Logger::Debug("[TCPSocket::Configure] Set SO_SNDBUF=%d on fd=%d", cfg.sendBufferSize, m_sock);
    Logger::Info("[TCPSocket::Configure] Socket fd=%d fully configured", m_sock);
    Logger::Trace("[TCPSocket::Configure] Exit: returning true");
    return true;
}
