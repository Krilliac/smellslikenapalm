// src/Network/SocketFactory.h
#pragma once

#include <memory>
#include <string>
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

class SocketFactory {
public:
    // Initialize platform networking (e.g. WSAStartup on Windows)
    static bool Initialize();

    // Cleanup platform networking
    static void Shutdown();

    // Create and configure a UDP socket bound to localPort (any address)
    static SocketHandle CreateUdpSocket(uint16_t localPort,
                                        const SocketConfig& cfg = {});

    // Create and configure a TCP listening socket on localPort
    static SocketHandle CreateTcpListenSocket(uint16_t localPort,
                                              int backlog = 64,
                                              const SocketConfig& cfg = {});

    // Create and configure a TCP client socket and connect to remote
    static SocketHandle CreateTcpClientSocket(const std::string& remoteIp,
                                              uint16_t remotePort,
                                              const SocketConfig& cfg = {});
private:
    static bool ConfigureSocket(SocketHandle sock, const SocketConfig& cfg);
};