#include "gameinfo_net.h"
#include "winutil.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include "hub_version.h"

namespace gameinfo_net {

namespace {
std::mutex g_mu;
std::string g_fixture_dir;
bool g_fixture_set = false;
std::chrono::steady_clock::time_point g_last;

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
}  // namespace

void set_fixture_dir(const std::string& dir) {
    std::lock_guard<std::mutex> l(g_mu);
    g_fixture_dir = dir;
    g_fixture_set = true;
}

std::string fixture_dir() {
    std::lock_guard<std::mutex> l(g_mu);
    if (g_fixture_set) return g_fixture_dir;
    const char* e = std::getenv("HUB_GAMEINFO_FIXTURES");
    return e ? e : "";
}

std::string fixture_name(const std::string& url) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : url) { h ^= c; h *= 1099511628211ull; }
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", (unsigned long long)h);
    return b;
}

std::string user_agent() {
    std::string v = HUB_VERSION_STR;
    if (!v.empty() && v[0] == 'v') v.erase(0, 1);
    return "RetroHub/" + v + " (https://github.com/fatty8me2/retrohub)";
}

bool http_get(const std::string& url, std::string* body, int* status) {
    *status = 0;
    body->clear();
    std::wstring wurl = widen(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    wchar_t host[256], path[4096];
    uc.lpszHostName = host;  uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;   uc.dwUrlPathLength = 4096;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return false;
    std::wstring full(path, uc.dwUrlPathLength);
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength) full.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);

    HINTERNET ses = WinHttpOpen(widen(user_agent()).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;
    WinHttpSetTimeouts(ses, 5000, 5000, 8000, 15000);
    bool ok = false;
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", full.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                        : nullptr;
    if (req) {
        const wchar_t* hdr = L"Accept: application/json\r\nApi-User-Agent: RetroHub (https://github.com/fatty8me2/retrohub)\r\n";
        WinHttpAddRequestHeaders(req, hdr, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    }
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
        DWORD code = 0, sz = sizeof code;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
        *status = (int)code;
        ok = true;
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail)) { ok = false; break; }
            if (avail == 0) break;
            size_t old = body->size();
            if (old + avail > (8u << 20)) { ok = false; break; }
            body->resize(old + avail);
            DWORD got = 0;
            if (!WinHttpReadData(req, &(*body)[old], avail, &got)) { ok = false; break; }
            body->resize(old + got);
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    if (!ok) *status = 0;
    return ok;
}

bool fetch(const std::string& url, std::string* body, int* status) {
    std::string dir = fixture_dir();
    if (!dir.empty()) {
        if (dir.back() != '/' && dir.back() != '\\') dir += '/';
        std::string base = dir + fixture_name(url);
        std::string t;
        if (winutil::read_file(base + ".json", &t)) { *body = t; *status = 200; return true; }
        if (winutil::read_file(base + ".404", &t)) { body->clear(); *status = 404; return true; }
        *status = 0;
        return false;
    }
    std::lock_guard<std::mutex> l(g_mu);   // one request at a time, >= 1 s apart (Wikimedia asks for low rates)
    auto now = std::chrono::steady_clock::now();
    auto gap = std::chrono::milliseconds(1000);
    if (g_last.time_since_epoch().count() != 0 && now - g_last < gap) std::this_thread::sleep_for(gap - (now - g_last));
    bool r = http_get(url, body, status);
    g_last = std::chrono::steady_clock::now();
    return r;
}

} // namespace gameinfo_net
