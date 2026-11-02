#pragma once
/* `--net-probe`: the Windows Firewall asks "Allow access?" once per executable the first time it binds a UDP port.
 * RetroHub's setup wizard runs `n64run --net-probe` and `retrorun --net-probe` while RetroHub is in front, so the
 * prompt appears there and never over a first online game (audit NC-M6). The probe binds UDP on all interfaces,
 * listens for a moment and exits with 0. It opens no window, loads no game and talks to nobody.
 * Header only; included by the mains of n64run and retrorun. */
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstring>

namespace hubprobe {

inline bool is_flag(const char* a) { return a && std::strcmp(a, "--net-probe") == 0; }
inline bool is_flag(const wchar_t* a) { return a && a[0] == L'-' && a[1] == L'-' && !std::wcscmp(a, L"--net-probe"); }

/* Returns 0 when a port could be bound (whether or not the user allowed it: the OS decides afterwards), 1 otherwise. */
inline int run(int listen_ms = 600) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
    int rc = 1;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s != INVALID_SOCKET) {
        sockaddr_in a;
        std::memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = 0;
        if (bind(s, (sockaddr*)&a, sizeof a) == 0) {
            rc = 0;
            DWORD to = (DWORD)listen_ms;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof to);
            char buf[16];
            recvfrom(s, buf, sizeof buf, 0, nullptr, nullptr);   // returns on timeout; a receive is what makes the OS evaluate the rule
        }
        closesocket(s);
    }
    WSACleanup();
    return rc;
}

}  // namespace hubprobe
