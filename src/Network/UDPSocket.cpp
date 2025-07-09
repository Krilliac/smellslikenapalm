// src/Network/UDPSocket.cpp

#include "Network/UDPSocket.h"
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

UDPSocket::UDPSocket() = default;

UDPSocket::~UDPSocket() {
    Close();
}

bool UDPSocket::Bind(uint16_t localPort, const SocketConfig& cfg) {
    m_cfg = cfg;
#ifdef _WIN32
    m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_sock == INVALID_SOCKET) return false;
#else
    m_sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (m_sock < 0) return false;
#endif

    if (!Configure(cfg)) {
        Close();
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(localPort);

    if (::bind(m_sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        Logger::Error("UDPSocket: bind to port %u failed", localPort);
        Close();
        return false;
    }

    return true;
}

bool UDPSocket::Configure(const SocketConfig& cfg) {
    // Timeouts
    timeval tv{};
    tv.tv_sec  = cfg.recvTimeout.count() / 1000;
    tv.tv_usec = (cfg.recvTimeout.count() % 1000) * 1000;
    setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));
    tv.tv_sec  = cfg.sendTimeout.count() / 1000;
    tv.tv_usec = (cfg.sendTimeout.count() % 1000) * 1000;
    setsockopt(m_sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&tv, sizeof(tv));

    // Buffer sizes
    setsockopt(m_sock, SOL_SOCKET, SO_RCVBUF, (char*)&cfg.recvBufferSize, sizeof(cfg.recvBufferSize));
    setsockopt(m_sock, SOL_SOCKET, SO_SNDBUF, (char*)&cfg.sendBufferSize, sizeof(cfg.sendBufferSize));

    // Non-blocking
#ifdef _WIN32
    u_long mode = cfg.nonBlocking ? 1 : 0;
    ioctlsocket(m_sock, FIONBIO, &mode);
#else
    int flags = fcntl(m_sock, F_GETFL, 0);
    if (cfg.nonBlocking) flags |= O_NONBLOCK;
    else flags &= ~O_NONBLOCK;
    fcntl(m_sock, F_SETFL, flags);
#endif

    return true;
}

void UDPSocket::Close() {
    if (m_sock < 0) return;
#ifdef _WIN32
    closesocket(m_sock);
#else
    ::close(m_sock);
#endif
    m_sock = -1;
}

bool UDPSocket::IsOpen() const {
    return m_sock >= 0;
}

bool UDPSocket::SendTo(const std::string& remoteIp, uint16_t remotePort,
                       const uint8_t* data, size_t len) {
    if (!IsOpen()) return false;
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(remotePort);
    inet_pton(AF_INET, remoteIp.c_str(), &dest.sin_addr);

    ssize_t sent = sendto(m_sock, reinterpret_cast<const char*>(data),
                          (int)len, 0,
                          (sockaddr*)&dest, sizeof(dest));
    return sent == (ssize_t)len;
}

int UDPSocket::ReceiveFrom(std::string& outIp, uint16_t& outPort,
                           uint8_t* buffer, int bufferLen) {
    if (!IsOpen()) return -1;
    sockaddr_in src{};
    socklen_t addrLen = sizeof(src);
    int len = recvfrom(m_sock, reinterpret_cast<char*>(buffer),
                       bufferLen, 0,
                       (sockaddr*)&src, &addrLen);
    if (len > 0) {
        outIp = inet_ntoa(src.sin_addr);
        outPort = ntohs(src.sin_port);
    }
    return len;
}