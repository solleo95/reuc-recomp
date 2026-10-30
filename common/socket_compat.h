#pragma once
// Winsock names on top of BSD sockets, so netplay.cpp and netplay_datagram.cpp read the same on both platforms. Windows: the real headers.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define UC_SEND_FLAGS 0
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

using SOCKET = int;
using BOOL = int;
inline constexpr int TRUE = 1;
inline constexpr SOCKET INVALID_SOCKET = -1;
inline constexpr int SOCKET_ERROR = -1;
#define UC_SEND_FLAGS MSG_NOSIGNAL   // a peer that went away must give an error, not SIGPIPE

inline int closesocket(SOCKET socket) { return ::close(socket); }
inline int WSAGetLastError() { return errno; }
#define FIONBIO 0
inline int ioctlsocket(SOCKET socket, long, unsigned long* nonblocking) {   // the only use is FIONBIO
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(socket, F_SETFL, *nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}
inline int InetPtonA(int family, const char* text, void* address) { return inet_pton(family, text, address); }

struct WSADATA {};
inline int WSAStartup(unsigned short, WSADATA*) { return 0; }
inline int WSACleanup() { return 0; }
#define MAKEWORD(low, high) 0

#define WSAEWOULDBLOCK EWOULDBLOCK
#define WSAEINPROGRESS EINPROGRESS
#define WSAENOBUFS ENOBUFS
#define WSAEMSGSIZE EMSGSIZE
#define WSAECONNRESET ECONNRESET
#define WSAECONNREFUSED ECONNREFUSED
#define WSAEADDRINUSE EADDRINUSE
#define WSAEACCES EACCES
#define SO_EXCLUSIVEADDRUSE SO_REUSEADDR   // Linux does not let two live listeners share a port with SO_REUSEADDR; it only skips the TIME_WAIT delay
#endif
