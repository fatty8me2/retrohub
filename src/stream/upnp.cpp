// Minimal UPnP IGD client: SSDP discovery + SOAP AddPortMapping (UDP). Best effort, ~3 s worst case.
#include "media.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include <cstdio>
#include <string>

namespace rplay {

namespace {

std::string http(const std::wstring& host, int port, const std::wstring& path, bool post, const std::string& body, const std::wstring& hdrs, DWORD* status)
{
    std::string out; *status = 0;
    HINTERNET s = WinHttpOpen(L"streamrun", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
    if (!s) return out;
    WinHttpSetTimeouts(s, 1500, 1500, 1500, 1500);
    if (HINTERNET c = WinHttpConnect(s, host.c_str(), (INTERNET_PORT)port, 0)) {
        if (HINTERNET r = WinHttpOpenRequest(c, post ? L"POST" : L"GET", path.c_str(), nullptr, nullptr, nullptr, 0)) {
            if (WinHttpSendRequest(r, hdrs.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdrs.c_str(), hdrs.empty() ? 0 : (DWORD)-1L,
                                   body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) &&
                WinHttpReceiveResponse(r, nullptr)) {
                DWORD sz = sizeof(DWORD);
                WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, status, &sz, nullptr);
                char buf[4096]; DWORD n;
                while (WinHttpReadData(r, buf, sizeof buf, &n) && n) out.append(buf, n);
            }
            WinHttpCloseHandle(r);
        }
        WinHttpCloseHandle(c);
    }
    WinHttpCloseHandle(s);
    return out;
}

std::wstring w(const std::string& s) { return std::wstring(s.begin(), s.end()); }

} // namespace

bool upnp_map_udp(uint16_t port, std::string* info)
{
    WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    DWORD to = 1200; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof to);
    sockaddr_in mc = {}; mc.sin_family = AF_INET; mc.sin_port = htons(1900); inet_pton(AF_INET, "239.255.255.250", &mc.sin_addr);
    const char* st = "urn:schemas-upnp-org:device:InternetGatewayDevice:1";
    char req[512];
    snprintf(req, sizeof req, "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\nST: %s\r\n\r\n", st);
    sendto(s, req, (int)strlen(req), 0, (sockaddr*)&mc, sizeof mc);
    char buf[2048]; std::string location;
    for (int i = 0; i < 4 && location.empty(); i++) {
        int n = recv(s, buf, sizeof buf - 1, 0);
        if (n <= 0) break;
        buf[n] = 0; std::string r(buf);
        size_t p = r.find("LOCATION:"); if (p == std::string::npos) p = r.find("Location:");
        if (p != std::string::npos) { size_t e = r.find("\r\n", p); location = r.substr(p + 9, e - p - 9); while (!location.empty() && location[0] == ' ') location.erase(0, 1); }
    }
    closesocket(s);
    if (location.empty()) { if (info) *info = "no UPnP gateway answered"; return false; }
    // http://host:port/path
    std::string hostport, path = "/";
    size_t a = location.find("://"); a = (a == std::string::npos) ? 0 : a + 3;
    size_t sl = location.find('/', a);
    hostport = location.substr(a, sl == std::string::npos ? std::string::npos : sl - a);
    if (sl != std::string::npos) path = location.substr(sl);
    std::string host = hostport; int hp = 80;
    size_t col = hostport.find(':'); if (col != std::string::npos) { host = hostport.substr(0, col); hp = atoi(hostport.c_str() + col + 1); }
    DWORD status;
    std::string desc = http(w(host), hp, w(path), false, "", L"", &status);
    const char* svcs[] = {"urn:schemas-upnp-org:service:WANIPConnection:1", "urn:schemas-upnp-org:service:WANIPConnection:2", "urn:schemas-upnp-org:service:WANPPPConnection:1"};
    std::string svc, ctrl;
    for (const char* sv : svcs) {
        size_t p = desc.find(sv); if (p == std::string::npos) continue;
        size_t c1 = desc.find("<controlURL>", p), c2 = desc.find("</controlURL>", p);
        if (c1 == std::string::npos || c2 == std::string::npos) continue;
        svc = sv; ctrl = desc.substr(c1 + 12, c2 - c1 - 12); break;
    }
    if (ctrl.empty()) { if (info) *info = "gateway has no WANIPConnection service"; return false; }
    if (ctrl[0] != '/') ctrl = "/" + ctrl;
    // our LAN address as the gateway sees it
    SOCKET u = socket(AF_INET, SOCK_DGRAM, 0); sockaddr_in g = {}; g.sin_family = AF_INET; g.sin_port = htons(9);
    addrinfo hints = {}, *ai = nullptr; hints.ai_family = AF_INET;
    std::string ip = "0.0.0.0";
    if (getaddrinfo(host.c_str(), nullptr, &hints, &ai) == 0 && ai) { g.sin_addr = ((sockaddr_in*)ai->ai_addr)->sin_addr; freeaddrinfo(ai); }
    if (connect(u, (sockaddr*)&g, sizeof g) == 0) { sockaddr_in me; int ml = sizeof me; getsockname(u, (sockaddr*)&me, &ml); char b[32]; inet_ntop(AF_INET, &me.sin_addr, b, sizeof b); ip = b; }
    closesocket(u);
    char body[1200];
    snprintf(body, sizeof body,
        "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
        "<u:AddPortMapping xmlns:u=\"%s\"><NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol><NewInternalPort>%u</NewInternalPort>"
        "<NewInternalClient>%s</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>RetroHub remote play</NewPortMappingDescription><NewLeaseDuration>3600</NewLeaseDuration>"
        "</u:AddPortMapping></s:Body></s:Envelope>", svc.c_str(), port, port, ip.c_str());
    std::string hdr = "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + svc + "#AddPortMapping\"\r\n";
    http(w(host), hp, w(ctrl), true, body, w(hdr), &status);
    if (info) *info = "UPnP AddPortMapping HTTP " + std::to_string(status);
    return status == 200;
}

} // namespace rplay
