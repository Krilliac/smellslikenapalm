// src/Network/TCPSocket.h

#pragma once

#include <string>
#include <vector>
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

class TCPSocket {
public:
    TCPSocket();
    ~TCPSocket();

    // Create an unconnected socket
    bool Open(const SocketConfig& cfg = {});

    // Connect to a remote server
    bool Connect(const std::string& remoteIp, uint16_t remotePort, const SocketConfig& cfg = {});

    // Bind and listen (server)
    bool Listen(uint16_t listenPort, int backlog = 64, const SocketConfig& cfg = {});

    // Accept a new client (blocking unless nonBlocking==true)
    // Returns a new TCPSocket for the client, or nullptr on failure/timeout
    std::unique_ptr<TCPSocket> Accept();

    // Send and receive
    ssize_t Send(const void* data, size_t len);
    ssize_t Receive(void* buffer, size_t len);

    // Shutdown and close
    void ShutdownSend();
    void ShutdownReceive();
    void Close();

    // State queries
    bool IsOpen() const;
    bool IsBlocking() const;

    // Utilities
    void SetBlocking(bool blocking);
    void SetRecvTimeout(std::chrono::milliseconds timeout);
    void SetSendTimeout(std::chrono::milliseconds timeout);

private:
    SocketHandle m_sock{-1};
    bool         m_blocking{true};

    bool Configure(const SocketConfig& cfg);
};