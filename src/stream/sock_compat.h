#pragma once
/* src/stream: Winsock spellings on macOS (SOCKET, INVALID_SOCKET, closesocket, receive timeout) over plat_sock.h. Windows is unchanged. */
#include "plat/plat_sock.h"

#if !RH_WIN
#include <sys/time.h>
using SOCKET = plat::socket_t;
static const SOCKET INVALID_SOCKET = plat::kBadSocket;
static inline int closesocket(SOCKET s) { return plat::sock_close(s); }
/* SO_RCVTIMEO: a DWORD of ms on Windows, a timeval on POSIX */
static inline void sock_rcvtimeo_ms(SOCKET s, int ms) {
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}
#else
static inline void sock_rcvtimeo_ms(SOCKET s, int ms) { DWORD t = (DWORD)ms; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof t); }
#endif
