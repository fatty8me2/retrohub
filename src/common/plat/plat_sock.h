#pragma once
/* Socket shim: one spelling for Winsock and BSD sockets (docs/MACOS.md). Include this instead of <winsock2.h> / <ws2tcpip.h>.
 * Windows behaviour is unchanged (the same Winsock calls); macOS maps them onto BSD sockets. Header-only. */

#include "plat.h"

#if RH_WIN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace plat {

#if RH_WIN
using socket_t = SOCKET;
constexpr socket_t kBadSocket = INVALID_SOCKET;
using socklen = int;
#else
using socket_t = int;
constexpr socket_t kBadSocket = -1;
using socklen = socklen_t;
#endif

/* WSAStartup once per process on Windows (ref-counted by Winsock, so calling it from several modules is fine). No-op on macOS,
 * which also ignores SIGPIPE for the process so a write to a closed peer returns EPIPE instead of killing us. */
inline bool sock_init() {
#if RH_WIN
    static const bool ok = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
    return ok;
#else
    static const bool ok = [] { signal(SIGPIPE, SIG_IGN); return true; }();
    return ok;
#endif
}

inline int sock_close(socket_t s) {
#if RH_WIN
    return closesocket(s);
#else
    return close(s);
#endif
}

/* Last socket error (WSAGetLastError / errno). */
inline int sock_error() {
#if RH_WIN
    return WSAGetLastError();
#else
    return errno;
#endif
}
inline bool sock_would_block(int err) {
#if RH_WIN
    return err == WSAEWOULDBLOCK;
#else
    return err == EWOULDBLOCK || err == EAGAIN || err == EINPROGRESS;
#endif
}
inline bool sock_conn_reset(int err) {
#if RH_WIN
    return err == WSAECONNRESET;
#else
    return err == ECONNRESET || err == EPIPE;
#endif
}

inline bool sock_set_nonblocking(socket_t s, bool on) {
#if RH_WIN
    u_long v = on ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &v) == 0;
#else
    const int fl = fcntl(s, F_GETFL, 0);
    if (fl < 0) return false;
    return fcntl(s, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK)) == 0;
#endif
}

/* Windows: stop a UDP socket from failing recvfrom with WSAECONNRESET after an ICMP port-unreachable (SIO_UDP_CONNRESET).
 * macOS never reports that on an unconnected UDP socket, so it is a no-op there. */
inline void udp_ignore_connreset(socket_t s) {
#if RH_WIN
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
    BOOL off = FALSE;
    DWORD ret = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, nullptr, 0, &ret, nullptr, nullptr);
#else
    (void)s;
#endif
}

/* Exclusive bind (SO_EXCLUSIVEADDRUSE on Windows). macOS sockets are already exclusive unless SO_REUSEADDR/PORT is set. */
inline void sock_exclusive(socket_t s) {
#if RH_WIN
    BOOL on = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&on, sizeof on);
#else
    (void)s;
#endif
}

/* setsockopt with the Windows (const char*) / POSIX (const void*) difference hidden. */
template <class T>
inline int sock_setopt(socket_t s, int level, int name, const T& value) {
    return setsockopt(s, level, name, (const char*)&value, (socklen)sizeof value);
}

/* poll() on both platforms (WSAPoll on Windows). Code built on WSAEventSelect + WaitForMultipleObjects keeps that path under
 * `#if RH_WIN` and uses this on macOS. Returns the number of ready entries, 0 on timeout, <0 on error. */
#if RH_WIN
using pollfd_t = WSAPOLLFD;
inline int sock_poll(pollfd_t* fds, unsigned n, int timeout_ms) { return WSAPoll(fds, (ULONG)n, timeout_ms); }
#else
using pollfd_t = struct pollfd;
inline int sock_poll(pollfd_t* fds, unsigned n, int timeout_ms) { return poll(fds, (nfds_t)n, timeout_ms); }
#endif

} // namespace plat
