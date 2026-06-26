// src/Network/PlatformSocket.h
//
// Cross-platform socket compatibility layer. Include this instead of the raw
// POSIX (<sys/socket.h>, <arpa/inet.h>, <unistd.h>, ...) or Winsock headers in
// any networking translation unit. It provides a single definition of
// SocketHandle, a portable ssize_t, a CloseSocket() helper, and the
// RS2V_INVALID_SOCKET sentinel.

#pragma once

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <basetsd.h>

  // MSVC's CRT has no ssize_t; SSIZE_T (from <basetsd.h>) is the equivalent.
  #ifndef RS2V_SSIZE_T_DEFINED
  #define RS2V_SSIZE_T_DEFINED
  using ssize_t = SSIZE_T;
  #endif

  using SocketHandle = SOCKET;

  inline int CloseSocket(SocketHandle s) { return ::closesocket(s); }

  #ifndef RS2V_INVALID_SOCKET
  #define RS2V_INVALID_SOCKET INVALID_SOCKET
  #endif
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <cerrno>

  using SocketHandle = int;

  inline int CloseSocket(SocketHandle s) { return ::close(s); }

  #ifndef RS2V_INVALID_SOCKET
  #define RS2V_INVALID_SOCKET (-1)
  #endif
#endif
