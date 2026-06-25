// src/Network/UDPSocket.cpp

#include "Network/UDPSocket.h"
#include "Utils/Logger.h"
#include "Network/PlatformSocket.h"

UDPSocket::UDPSocket() {
    Logger::Trace("[UDPSocket::UDPSocket] Entry: default constructor");
    Logger::Debug("[UDPSocket::UDPSocket] Created UDPSocket with m_sock=-1 (uninitialized)");
    Logger::Trace("[UDPSocket::UDPSocket] Exit");
}

UDPSocket::~UDPSocket() {
    Logger::Trace("[UDPSocket::~UDPSocket] Entry: destructor, m_sock=%d", m_sock);
    Logger::Debug("[UDPSocket::~UDPSocket] Destroying UDPSocket, calling Close()");
    Close();
    Logger::Trace("[UDPSocket::~UDPSocket] Exit");
}

bool UDPSocket::Bind(uint16_t localPort, const SocketConfig& cfg) {
    Logger::Trace("[UDPSocket::Bind] Entry: localPort=%u, nonBlocking=%s", localPort, cfg.nonBlocking ? "true" : "false");
    m_cfg = cfg;
    Logger::Debug("[UDPSocket::Bind] Stored SocketConfig, creating UDP socket");
#ifdef _WIN32
    m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_sock == INVALID_SOCKET) {
        Logger::Error("[UDPSocket::Bind] socket() returned INVALID_SOCKET (Windows)");
        Logger::Trace("[UDPSocket::Bind] Exit: returning false (INVALID_SOCKET)");
        return false;
    }
    Logger::Debug("[UDPSocket::Bind] Windows UDP socket created: fd=%d", m_sock);
#else
    m_sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (m_sock < 0) {
        Logger::Error("[UDPSocket::Bind] socket() returned %d (POSIX)", m_sock);
        Logger::Trace("[UDPSocket::Bind] Exit: returning false (socket creation failed)");
        return false;
    }
    Logger::Debug("[UDPSocket::Bind] POSIX UDP socket created: fd=%d", m_sock);
#endif

    if (!Configure(cfg)) {
        Logger::Error("[UDPSocket::Bind] Configure failed for fd=%d", m_sock);
        Close();
        Logger::Debug("[UDPSocket::Bind] Socket closed after configuration failure");
        Logger::Trace("[UDPSocket::Bind] Exit: returning false (configure failed)");
        return false;
    }
    Logger::Debug("[UDPSocket::Bind] Socket fd=%d configured successfully", m_sock);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(localPort);

    Logger::Debug("[UDPSocket::Bind] Binding to INADDR_ANY:%u on fd=%d", localPort, m_sock);
    if (::bind(m_sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        Logger::Error("UDPSocket: bind to port %u failed", localPort);
        Logger::Debug("[UDPSocket::Bind] bind() failed on fd=%d, port=%u", m_sock, localPort);
        Close();
        Logger::Trace("[UDPSocket::Bind] Exit: returning false (bind failed)");
        return false;
    }

    Logger::Info("[UDPSocket::Bind] UDP socket bound successfully: fd=%d, port=%u", m_sock, localPort);
    Logger::Trace("[UDPSocket::Bind] Exit: returning true");
    return true;
}

bool UDPSocket::Configure(const SocketConfig& cfg) {
    Logger::Trace("[UDPSocket::Configure] Entry: fd=%d, nonBlocking=%s, recvTimeout=%lld ms, sendTimeout=%lld ms",
                  m_sock, cfg.nonBlocking ? "true" : "false",
                  (long long)cfg.recvTimeout.count(), (long long)cfg.sendTimeout.count());
    // Timeouts
    timeval tv{};
    tv.tv_sec  = cfg.recvTimeout.count() / 1000;
    tv.tv_usec = (cfg.recvTimeout.count() % 1000) * 1000;
    setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));
    Logger::Debug("[UDPSocket::Configure] Set SO_RCVTIMEO: %ld sec, %ld usec on fd=%d",
                  tv.tv_sec, tv.tv_usec, m_sock);
    tv.tv_sec  = cfg.sendTimeout.count() / 1000;
    tv.tv_usec = (cfg.sendTimeout.count() % 1000) * 1000;
    setsockopt(m_sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&tv, sizeof(tv));
    Logger::Debug("[UDPSocket::Configure] Set SO_SNDTIMEO: %ld sec, %ld usec on fd=%d",
                  tv.tv_sec, tv.tv_usec, m_sock);

    // Buffer sizes
    setsockopt(m_sock, SOL_SOCKET, SO_RCVBUF, (char*)&cfg.recvBufferSize, sizeof(cfg.recvBufferSize));
    Logger::Debug("[UDPSocket::Configure] Set SO_RCVBUF=%d on fd=%d", cfg.recvBufferSize, m_sock);
    setsockopt(m_sock, SOL_SOCKET, SO_SNDBUF, (char*)&cfg.sendBufferSize, sizeof(cfg.sendBufferSize));
    Logger::Debug("[UDPSocket::Configure] Set SO_SNDBUF=%d on fd=%d", cfg.sendBufferSize, m_sock);

    // Non-blocking
#ifdef _WIN32
    u_long mode = cfg.nonBlocking ? 1 : 0;
    ioctlsocket(m_sock, FIONBIO, &mode);
    Logger::Debug("[UDPSocket::Configure] Set FIONBIO mode=%lu on fd=%d (Windows)", mode, m_sock);
#else
    int flags = fcntl(m_sock, F_GETFL, 0);
    if (cfg.nonBlocking) flags |= O_NONBLOCK;
    else flags &= ~O_NONBLOCK;
    fcntl(m_sock, F_SETFL, flags);
    Logger::Debug("[UDPSocket::Configure] Set fcntl flags=0x%X (nonBlocking=%s) on fd=%d",
                  flags, cfg.nonBlocking ? "true" : "false", m_sock);
#endif

    Logger::Info("[UDPSocket::Configure] Socket fd=%d configured successfully", m_sock);
    Logger::Trace("[UDPSocket::Configure] Exit: returning true");
    return true;
}

void UDPSocket::Close() {
    Logger::Trace("[UDPSocket::Close] Entry: fd=%d", m_sock);
    if (m_sock < 0) {
        Logger::Debug("[UDPSocket::Close] Socket already closed (fd=%d), nothing to do", m_sock);
        Logger::Trace("[UDPSocket::Close] Exit: already closed");
        return;
    }
    Logger::Debug("[UDPSocket::Close] Closing socket fd=%d", m_sock);
#ifdef _WIN32
    closesocket(m_sock);
    Logger::Debug("[UDPSocket::Close] closesocket() called on fd=%d (Windows)", m_sock);
#else
    ::close(m_sock);
    Logger::Debug("[UDPSocket::Close] close() called on fd=%d (POSIX)", m_sock);
#endif
    m_sock = -1;
    Logger::Info("[UDPSocket::Close] Socket closed, fd reset to -1");
    Logger::Trace("[UDPSocket::Close] Exit");
}

bool UDPSocket::IsOpen() const {
    Logger::Trace("[UDPSocket::IsOpen] Entry: fd=%d", m_sock);
    bool open = m_sock >= 0;
    Logger::Debug("[UDPSocket::IsOpen] fd=%d, returning %s", m_sock, open ? "true" : "false");
    Logger::Trace("[UDPSocket::IsOpen] Exit: returning %s", open ? "true" : "false");
    return open;
}

bool UDPSocket::SendTo(const std::string& remoteIp, uint16_t remotePort,
                       const uint8_t* data, size_t len) {
    Logger::Trace("[UDPSocket::SendTo] Entry: remoteIp='%s', remotePort=%u, data=%p, len=%zu",
                  remoteIp.c_str(), remotePort, (const void*)data, len);
    if (!IsOpen()) {
        Logger::Error("[UDPSocket::SendTo] Socket not open (fd=%d), cannot send", m_sock);
        Logger::Trace("[UDPSocket::SendTo] Exit: returning false (not open)");
        return false;
    }
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(remotePort);
    inet_pton(AF_INET, remoteIp.c_str(), &dest.sin_addr);

    Logger::Debug("[UDPSocket::SendTo] Sending %zu bytes to %s:%u via fd=%d", len, remoteIp.c_str(), remotePort, m_sock);
    ssize_t sent = sendto(m_sock, reinterpret_cast<const char*>(data),
                          (int)len, 0,
                          (sockaddr*)&dest, sizeof(dest));
    bool success = sent == (ssize_t)len;
    if (success) {
        Logger::Debug("[UDPSocket::SendTo] Successfully sent %zd bytes to %s:%u", sent, remoteIp.c_str(), remotePort);
    } else {
        Logger::Warn("[UDPSocket::SendTo] Partial or failed send: sent %zd of %zu bytes to %s:%u",
                     sent, len, remoteIp.c_str(), remotePort);
    }
    Logger::Trace("[UDPSocket::SendTo] Exit: returning %s", success ? "true" : "false");
    return success;
}

int UDPSocket::ReceiveFrom(std::string& outIp, uint16_t& outPort,
                           uint8_t* buffer, int bufferLen) {
    Logger::Trace("[UDPSocket::ReceiveFrom] Entry: fd=%d, buffer=%p, bufferLen=%d",
                  m_sock, (void*)buffer, bufferLen);
    if (!IsOpen()) {
        Logger::Error("[UDPSocket::ReceiveFrom] Socket not open (fd=%d), cannot receive", m_sock);
        Logger::Trace("[UDPSocket::ReceiveFrom] Exit: returning -1 (not open)");
        return -1;
    }
    sockaddr_in src{};
    socklen_t addrLen = sizeof(src);
    Logger::Debug("[UDPSocket::ReceiveFrom] Calling recvfrom on fd=%d, bufferLen=%d", m_sock, bufferLen);
    int len = recvfrom(m_sock, reinterpret_cast<char*>(buffer),
                       bufferLen, 0,
                       (sockaddr*)&src, &addrLen);
    if (len > 0) {
        // inet_ntoa returns a non-reentrant static buffer; ReceiveFrom runs on the
        // network thread, so use inet_ntop into a local buffer instead.
        char ipbuf[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &src.sin_addr, ipbuf, sizeof(ipbuf));
        outIp = ipbuf;
        outPort = ntohs(src.sin_port);
        Logger::Debug("[UDPSocket::ReceiveFrom] Received %d bytes from %s:%u", len, outIp.c_str(), outPort);
    } else {
        Logger::Debug("[UDPSocket::ReceiveFrom] recvfrom returned %d (no data or error)", len);
    }
    Logger::Trace("[UDPSocket::ReceiveFrom] Exit: returning %d", len);
    return len;
}
