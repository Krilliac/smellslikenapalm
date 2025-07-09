// src/Network/UDPSocket.h

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <chrono>

#ifdef _WIN32
  #include <winsock2.h>
  using SocketHandle = SOCKET;
#else
  using SocketHandle = int;
#endif

struct SocketConfig {
    std::chrono::milliseconds recvTimeout{500};
    std::chrono::milliseconds sendTimeout{500};
    bool nonBlocking = true;
    size_t recvBufferSize = 65536;
    size_t sendBufferSize = 65536;
};

class UDPSocket {
public:
    UDPSocket();
    ~UDPSocket();

    // Bind to a local port (INADDR_ANY)
    bool Bind(uint16_t localPort, const SocketConfig& cfg = {});

    // Close socket
    void Close();

    // Send data to remote address
    bool SendTo(const std::string& remoteIp, uint16_t remotePort,
                const uint8_t* data, size_t len);

    // Receive data; returns length or -1 on error/timeout
    int ReceiveFrom(std::string& outIp, uint16_t& outPort,
                    uint8_t* buffer, int bufferLen);

    // Is socket open?
    bool IsOpen() const;

private:
    SocketHandle            m_sock{-1};
    SocketConfig            m_cfg;

    bool Configure(const SocketConfig& cfg);
};