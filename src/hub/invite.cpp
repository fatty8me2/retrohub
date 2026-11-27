/* Clickable invite links: parsing, link building, and the single-instance pipe handoff (see invite.h). */
#include "winutil.h"
#include "invite.h"
#include "invite_logic.h"

#if defined(_WIN32)
#include <shellapi.h>
#include <sddl.h>
#else
#include "plat/plat.h"
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#endif

#include <cstring>
#include <cwctype>
#include <vector>

#if defined(_WIN32)
const wchar_t* const INVITE_MUTEX_NAME = L"Local\\RetroHub.SingleInstance";
const wchar_t* const INVITE_PIPE_NAME = L"\\\\.\\pipe\\RetroHub.Invite";
#else
const wchar_t* const INVITE_MUTEX_NAME = L"hub.lock";   // under invite_set_runtime_dir()
const wchar_t* const INVITE_PIPE_NAME = L"hub.sock";

/* Local stand-ins so the portable parsing below stays the same text as on Windows. */
typedef wchar_t* LPWSTR;
#define _wcsicmp wcscasecmp
static LPWSTR* CommandLineToArgvW(const wchar_t* cmd, int* argc_out) {   // the CRT / CommandLineToArgvW quoting rules
    std::vector<std::wstring> a;
    std::wstring cur;
    bool in_q = false, has = false;
    const std::wstring s = cmd ? cmd : L"";
    for (size_t i = 0; i < s.size();) {
        wchar_t c = s[i];
        if (c == L'\\') {
            size_t n = 0;
            while (i < s.size() && s[i] == L'\\') { ++n; ++i; }
            if (i < s.size() && s[i] == L'"') { cur.append(n / 2, L'\\'); has = true; if (n % 2) { cur += L'"'; ++i; } }
            else { cur.append(n, L'\\'); has = true; }
            continue;
        }
        if (c == L'"') { in_q = !in_q; has = true; ++i; continue; }
        if ((c == L' ' || c == L'\t') && !in_q) { if (has) { a.push_back(cur); cur.clear(); has = false; } ++i; continue; }
        cur += c; has = true; ++i;
    }
    if (has) a.push_back(cur);
    size_t chars = 0;
    for (auto& x : a) chars += x.size() + 1;
    void* blk = std::malloc(sizeof(LPWSTR) * (a.size() + 1) + sizeof(wchar_t) * (chars + 1));
    if (!blk) return nullptr;
    LPWSTR* v = (LPWSTR*)blk;
    wchar_t* w = (wchar_t*)(v + a.size() + 1);
    for (size_t i = 0; i < a.size(); ++i) { v[i] = w; std::wmemcpy(w, a[i].c_str(), a[i].size() + 1); w += a[i].size() + 1; }
    v[a.size()] = nullptr;
    *argc_out = (int)a.size();
    return v;
}
static void LocalFree(LPWSTR* p) { std::free(p); }
#endif

namespace {

const wchar_t kWebBase[] = L"https://smash-netplay.jumpcircuit.workers.dev/j/";
const wchar_t kScheme[] = L"retrohub://join/";
const size_t kPipeMsgMax = 200;

bool starts_with_ci(const std::wstring& s, const wchar_t* prefix) {
    size_t n = wcslen(prefix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (towupper(s[i]) != towupper(prefix[i])) return false;
    return true;
}

}  // namespace

static std::wstring invite_normalize_code_keep_case(const std::wstring& raw) {
    size_t b = 0, e = raw.size();
    while (b < e && iswspace(raw[b])) ++b;
    while (e > b && iswspace(raw[e - 1])) --e;
    return raw.substr(b, e - b);
}

// Plain 6-char codes only: netplay rooms are never "-N" suffixed (the relay accepts exactly 6 chars), so no suffix is parsed.
bool invite_is_valid_code(const std::wstring& code) {
    if (code.size() != 6) return false;
    for (wchar_t c : code) {
        bool ok = (c >= L'A' && c <= L'H') || (c >= L'J' && c <= L'N') || (c >= L'P' && c <= L'Z') || (c >= L'2' && c <= L'9');
        if (!ok) return false;
    }
    return true;
}

std::wstring invite_normalize_code(const std::wstring& raw) {
    size_t b = 0, e = raw.size();
    while (b < e && iswspace(raw[b])) ++b;
    while (e > b && iswspace(raw[e - 1])) --e;
    std::wstring s = raw.substr(b, e - b);
    for (auto& c : s) c = (wchar_t)towupper(c);
    return s;
}

/* Percent-decodes %XX escapes (ASCII results only; anything else is left as is). */
static std::wstring url_unescape(const std::wstring& in) {
    std::wstring out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == L'%' && i + 2 < in.size() && iswxdigit(in[i + 1]) && iswxdigit(in[i + 2])) {
            int v = std::stoi(in.substr(i + 1, 2), nullptr, 16);
            if (v > 0 && v < 128) { out += (wchar_t)v; i += 2; continue; }
        }
        out += in[i];
    }
    return out;
}

/* Accepts what Windows hands the protocol handler: retrohub://join/CODE, retrohub://join/CODE/, retrohub:join/CODE,
   retrohub://CODE, %-encoded or mixed case, with a trailing query/fragment, wrapped in quotes or <>. */
bool invite_parse_url(const std::wstring& url_in, std::wstring* code) {
    std::wstring url = invite_normalize_code_keep_case(url_in);
    while (!url.empty() && (url.front() == L'"' || url.front() == L'\'' || url.front() == L'<')) url.erase(url.begin());
    while (!url.empty() && (url.back() == L'"' || url.back() == L'\'' || url.back() == L'>')) url.pop_back();
    if (!starts_with_ci(url, L"retrohub:")) return false;
    std::wstring rest = url.substr(9);
    size_t cut = rest.find_first_of(L"?#");
    if (cut != std::wstring::npos) rest.erase(cut);
    size_t b = 0;
    while (b < rest.size() && rest[b] == L'/') ++b;   // "//" (or none)
    rest = url_unescape(rest.substr(b));
    if (starts_with_ci(rest, L"join/")) rest = rest.substr(5);
    else if (rest.size() == 4 && starts_with_ci(rest, L"join")) return false;
    if (!rest.empty() && rest.back() == L'/') rest.pop_back();
    std::wstring c = invite_normalize_code(rest);
    if (!invite_is_valid_code(c)) return false;
    if (code) *code = c;
    return true;
}

bool invite_is_valid_night_id(const std::wstring& id) {
    const size_t at = id.rfind(L'@');
    if (at == std::wstring::npos || at == 0 || at > 60) return false;
    for (size_t i = 0; i < at; ++i) {
        wchar_t c = id[i];
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'_' || c == L'.' || c == L'-')) return false;
    }
    const size_t nd = id.size() - at - 1;
    if (nd < 1 || nd > 11) return false;
    for (size_t i = at + 1; i < id.size(); ++i) if (id[i] < L'0' || id[i] > L'9') return false;
    return true;
}

bool invite_parse_night_url(const std::wstring& url_in, std::wstring* id) {
    std::wstring url = url_in;
    while (!url.empty() && (url.front() == L'"' || url.front() == L'\'' || url.front() == L'<')) url.erase(url.begin());
    while (!url.empty() && (url.back() == L'"' || url.back() == L'\'' || url.back() == L'>')) url.pop_back();
    if (url.size() > 200 || !starts_with_ci(url, L"retrohub:")) return false;
    std::wstring rest = url.substr(9);
    size_t cut = rest.find_first_of(L"?#");
    if (cut != std::wstring::npos) rest.erase(cut);
    size_t b = 0;
    while (b < rest.size() && rest[b] == L'/') ++b;
    rest = url_unescape(rest.substr(b));
    if (!starts_with_ci(rest, L"night/")) return false;
    rest = rest.substr(6);
    if (!rest.empty() && rest.back() == L'/') rest.pop_back();
    if (!invite_is_valid_night_id(rest)) return false;
    if (id) *id = rest;
    return true;
}

bool invite_parse_night_command_line(const std::wstring& cmdline, std::wstring* id) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdline.c_str(), &argc);
    if (!argv) return false;
    bool found = false;
    std::wstring result;
    for (int i = 0; i < argc && !found; ++i) found = invite_parse_night_url(argv[i], &result);
    LocalFree(argv);
    if (found && id) *id = result;
    return found;
}

bool invite_parse_command_line(const std::wstring& cmdline, std::wstring* code) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdline.c_str(), &argc);
    if (!argv) return false;
    bool found = false;
    std::wstring result;
    for (int i = 0; i < argc && !found; ++i) {
        std::wstring a = argv[i];
        if (invite_parse_url(a, &result)) { found = true; break; }
        if (_wcsicmp(a.c_str(), L"--join") == 0 && i + 1 < argc) {
            std::wstring c = invite_normalize_code(argv[i + 1]);
            if (invite_is_valid_code(c)) { result = c; found = true; }
        }
    }
    LocalFree(argv);
    if (found && code) *code = result;
    return found;
}

std::wstring invite_host_query(const std::wstring& host_name) {
    std::wstring name;
    for (wchar_t ch : host_name) {
        if (iswalnum(ch) || ch == L' ' || ch == L'.' || ch == L'_' || ch == L'-') name += ch;
        if (name.size() >= 24) break;
    }
    while (!name.empty() && name.back() == L' ') name.pop_back();
    while (!name.empty() && name.front() == L' ') name.erase(name.begin());
    if (name.empty()) return L"";
    std::string u = winutil::to_utf8(name), enc;
    static const char hex[] = "0123456789ABCDEF";
    for (unsigned char ch : u) {
        if (isalnum(ch) || ch == '.' || ch == '_' || ch == '-') enc += (char)ch;
        else { enc += '%'; enc += hex[ch >> 4]; enc += hex[ch & 15]; }
    }
    return L"?h=" + winutil::to_wide(enc);
}

std::wstring invite_web_link(const std::wstring& code, const std::wstring& host_name) {
    std::wstring c = invite_normalize_code(code);
    if (!invite_is_valid_code(c)) return L"";
    std::wstring q = invite_host_query(host_name);
    if (!q.empty()) return std::wstring(kWebBase) + c + q;
    return std::wstring(kWebBase) + c;
}

#if defined(_WIN32)
bool invite_copy_text(const std::wstring& text) {
    if (!OpenClipboard(nullptr)) return false;
    bool ok = false;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        void* p = GlobalLock(h);
        if (p) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(h);
            ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
        }
        if (!ok) GlobalFree(h);   // the clipboard owns h only after a successful SetClipboardData
    }
    CloseClipboard();
    return ok;
}
#else
bool invite_copy_text(const std::wstring& text) { return invite_copy_text_utf8(winutil::to_utf8(text)); }   // NSPasteboard: url_events_mac.mm
#endif

std::wstring invite_protocol_command(const std::wstring& exe) {
    return L"\"" + exe + L"\" \"%1\"";
}

#if defined(_WIN32)
static const DWORD kClientReadMs = 2000;   // a connected client must send its message within this

/* ---- single instance ---- */

InviteInstance::InviteInstance(const std::wstring& mutex_name) {
    mutex_ = CreateMutexW(nullptr, FALSE, mutex_name.c_str());
    primary_ = mutex_ && GetLastError() != ERROR_ALREADY_EXISTS;
}

InviteInstance::~InviteInstance() {
    if (mutex_) CloseHandle(mutex_);
}

InviteServer::~InviteServer() {
    stop();
}

bool InviteServer::start(const std::wstring& pipe_name) {
    if (thread_) return false;
    pipe_ = pipe_name;
    InitializeCriticalSection(&lock_);
    lock_ready_ = true;
    InterlockedExchange(&stop_, 0);
    thread_ = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        static_cast<InviteServer*>(p)->run();
        return 0;
    }, this, 0, nullptr);
    return thread_ != nullptr;
}

/* Waits for an overlapped operation; false (and the I/O cancelled) on timeout. */
static bool wait_overlapped(HANDLE pipe, OVERLAPPED* ov, DWORD ms, DWORD* bytes) {
    if (WaitForSingleObject(ov->hEvent, ms) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, ov);
        GetOverlappedResult(pipe, ov, bytes, TRUE);
        return false;
    }
    return GetOverlappedResult(pipe, ov, bytes, FALSE) != FALSE;
}

void InviteServer::run() {
    /* Only the current user may talk to the pipe (owner-rights SDDL, protected DACL). */
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;OW)", SDDL_REVISION_1, &sd, nullptr))
        sa.lpSecurityDescriptor = sd;
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    while (ev && !InterlockedCompareExchange(&stop_, 0, 0)) {
        HANDLE pipe = CreateNamedPipeW(pipe_.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                       PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                       PIPE_UNLIMITED_INSTANCES, 512, 512, 0, sd ? &sa : nullptr);
        if (pipe == INVALID_HANDLE_VALUE) { Sleep(200); continue; }
        OVERLAPPED ov{}; ov.hEvent = ev; ResetEvent(ev);
        bool connected = false;
        if (ConnectNamedPipe(pipe, &ov)) connected = true;
        else {
            DWORD e = GetLastError();
            if (e == ERROR_PIPE_CONNECTED) connected = true;
            else if (e == ERROR_IO_PENDING) {
                DWORD d = 0;
                while (!InterlockedCompareExchange(&stop_, 0, 0)) {
                    if (WaitForSingleObject(ev, 200) == WAIT_OBJECT_0) { connected = GetOverlappedResult(pipe, &ov, &d, FALSE) != FALSE; break; }
                }
                if (!connected) { CancelIoEx(pipe, &ov); GetOverlappedResult(pipe, &ov, &d, TRUE); }
            }
        }
        if (connected && !InterlockedCompareExchange(&stop_, 0, 0)) {
            char buf[kPipeMsgMax + 1] = {};
            DWORD got = 0;
            ResetEvent(ev); ov = OVERLAPPED{}; ov.hEvent = ev;
            bool ok = false;
            if (ReadFile(pipe, buf, (DWORD)kPipeMsgMax, &got, &ov)) ok = true;
            else if (GetLastError() == ERROR_IO_PENDING) ok = wait_overlapped(pipe, &ov, kClientReadMs, &got);   // silent client: give up
            std::wstring text;
            if (ok && got > 0) {
                text = winutil::to_wide(std::string(buf, got));
            }
            bool accepted = false;
            if (text == L"show") { push(2, L""); accepted = true; }
            else if (text.compare(0, 5, L"acct:") == 0) {
                std::string c;
                if (hubnet::invite_valid(winutil::to_utf8(text.substr(5)))) { c = hubnet::invite_format(winutil::to_utf8(text.substr(5))); push(3, winutil::to_wide(c)); accepted = true; }
            }
            else if (text.compare(0, 6, L"night:") == 0) {
                std::wstring id = text.substr(6);
                if (invite_is_valid_night_id(id)) { push(4, id); accepted = true; }
            }
            else if (text.compare(0, 5, L"play:") == 0) {
                std::wstring key = text.substr(5);
                if (invite_is_valid_game_key(key)) { push(1, key); accepted = true; }
            } else {
                std::wstring code = invite_normalize_code(text);
                if (invite_is_valid_code(code)) { push(0, code); accepted = true; }
            }
            if (ok) {
                const char* reply = accepted ? "OK" : "ERR";
                DWORD wrote = 0;
                ResetEvent(ev); ov = OVERLAPPED{}; ov.hEvent = ev;
                if (WriteFile(pipe, reply, 2 + (accepted ? 0 : 1), &wrote, &ov) || GetLastError() == ERROR_IO_PENDING)
                    wait_overlapped(pipe, &ov, kClientReadMs, &wrote);
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    if (ev) CloseHandle(ev);
    if (sd) LocalFree(sd);
}

void InviteServer::stop() {
    if (!thread_) return;
    InterlockedExchange(&stop_, 1);
    // The accept loop is blocked in ConnectNamedPipe; one client connection wakes it so it sees stop_.
    CallNamedPipeW(pipe_.c_str(), (LPVOID)"", 0, nullptr, 0, nullptr, 500);
    WaitForSingleObject(thread_, 2000);
    CloseHandle(thread_);
    thread_ = nullptr;
    if (lock_ready_) { DeleteCriticalSection(&lock_); lock_ready_ = false; }
}

void InviteServer::push(int kind, const std::wstring& text) {
    EnterCriticalSection(&lock_);
    if (count_ < 16) { queue_[count_].kind = kind; queue_[count_].text = text; ++count_; }
    LeaveCriticalSection(&lock_);
}

bool InviteServer::pop(int kind, std::wstring* text) {
    if (!lock_ready_) return false;
    EnterCriticalSection(&lock_);
    bool got = false;
    for (int i = 0; i < count_ && !got; ++i) {
        if (queue_[i].kind != kind) continue;
        if (text) *text = queue_[i].text;
        for (int j = i + 1; j < count_; ++j) queue_[j - 1] = queue_[j];
        --count_;
        got = true;
    }
    LeaveCriticalSection(&lock_);
    return got;
}

bool InviteServer::take(std::wstring* code) { return pop(0, code); }
bool InviteServer::take_play(std::wstring* key) { return pop(1, key); }
bool InviteServer::take_account(std::wstring* code) { return pop(3, code); }
bool InviteServer::take_night(std::wstring* id) { return pop(4, id); }
bool InviteServer::take_show() { return pop(2, nullptr); }
#else
static const unsigned long kClientReadMs = 2000;   // a connected client must send its message within this

/* ---- single instance (macOS): an flock(2) lock file instead of the named mutex ---- */
namespace {

std::mutex g_q_mu;
struct QMsg { int kind; std::wstring text; };
std::vector<QMsg> g_queue;   // process-wide: the socket thread and the Apple Event handler both feed it
std::string g_runtime_dir;

std::string tmp_dir() {
    const char* t = std::getenv("TMPDIR");
    std::string d = (t && *t) ? t : "/tmp";
    if (d.back() != '/') d += '/';
    return d;
}

std::string resolve_path(const std::wstring& name) {
    std::string n = winutil::to_utf8(name);
    if (!n.empty() && n[0] == '/') return n;
    std::string dir;
    { std::lock_guard<std::mutex> l(g_q_mu); dir = g_runtime_dir; }
    if (dir.empty()) dir = tmp_dir();
    if (dir.back() != '/') dir += '/';
    return dir + n;
}

std::string fnv_hex(const std::string& s) {
    unsigned long long h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", h);
    return b;
}

/* sockaddr_un.sun_path holds 104 bytes on macOS: a longer path falls back to $TMPDIR/rh-<hash>.sock (same on both sides). */
std::string socket_path(const std::wstring& name) {
    std::string p = resolve_path(name);
    if (p.size() < sizeof(((sockaddr_un*)nullptr)->sun_path)) return p;
    return tmp_dir() + "rh-" + fnv_hex(p) + ".sock";
}

bool make_addr(const std::string& path, sockaddr_un* a) {
    std::memset(a, 0, sizeof *a);
    if (path.size() >= sizeof a->sun_path) return false;
    a->sun_family = AF_UNIX;
    a->sun_len = (uint8_t)sizeof *a;
    std::memcpy(a->sun_path, path.c_str(), path.size() + 1);
    return true;
}

}  // namespace

void invite_set_runtime_dir(const std::string& dir_utf8) {
    std::lock_guard<std::mutex> l(g_q_mu);
    g_runtime_dir = dir_utf8;
}

InviteInstance::InviteInstance(const std::wstring& mutex_name) {
    const std::string path = resolve_path(mutex_name);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd_ < 0) return;
    if (::flock(fd_, LOCK_EX | LOCK_NB) == 0) primary_ = true;
}

InviteInstance::~InviteInstance() {
    if (fd_ >= 0) ::close(fd_);   // closing drops the flock
}

struct InviteServer::Impl {
    std::thread thread;
    int listen_fd = -1;
    int wake_r = -1, wake_w = -1;
    std::atomic<bool> stop{false};
    std::string path;
};

InviteServer::~InviteServer() {
    stop();
}

static void queue_push(int kind, const std::wstring& text) {
    std::lock_guard<std::mutex> l(g_q_mu);
    if (g_queue.size() < 16) g_queue.push_back({kind, text});
}

/* Classifies one wire message ("ABCDEF", "play:<key>", "acct:<code>", "night:<id>", "show"); true when valid and queued. */
static bool handle_message(const std::wstring& text) {
    if (text == L"show") { queue_push(2, L""); return true; }
    if (text.compare(0, 5, L"acct:") == 0) {
        if (hubnet::invite_valid(winutil::to_utf8(text.substr(5)))) {
            queue_push(3, winutil::to_wide(hubnet::invite_format(winutil::to_utf8(text.substr(5)))));
            return true;
        }
        return false;
    }
    if (text.compare(0, 6, L"night:") == 0) {
        std::wstring id = text.substr(6);
        if (invite_is_valid_night_id(id)) { queue_push(4, id); return true; }
        return false;
    }
    if (text.compare(0, 5, L"play:") == 0) {
        std::wstring key = text.substr(5);
        if (invite_is_valid_game_key(key)) { queue_push(1, key); return true; }
        return false;
    }
    std::wstring code = invite_normalize_code(text);
    if (invite_is_valid_code(code)) { queue_push(0, code); return true; }
    return false;
}

bool invite_deliver_url(const std::string& url_utf8) {
    const std::wstring u = winutil::to_wide(url_utf8);
    std::wstring v;
    if (invite_parse_url(u, &v)) { queue_push(0, v); return true; }
    if (invite_parse_account_url(u, &v)) { queue_push(3, v); return true; }
    if (invite_parse_night_url(u, &v)) { queue_push(4, v); return true; }
    return false;
}

template <class Impl>
static void serve(Impl* im) {   // template: Impl is private to InviteServer, a deduced type needs no access
    while (!im->stop.load()) {
        pollfd fds[2] = {{im->listen_fd, POLLIN, 0}, {im->wake_r, POLLIN, 0}};
        if (::poll(fds, 2, -1) < 0) { if (errno == EINTR) continue; break; }
        if (fds[1].revents || im->stop.load()) break;
        if (!(fds[0].revents & POLLIN)) continue;
        int c = ::accept(im->listen_fd, nullptr, nullptr);
        if (c < 0) continue;
        char buf[kPipeMsgMax + 1] = {};
        size_t got = 0;
        bool ok = false;
        const auto t0 = std::chrono::steady_clock::now();
        for (;;) {   // the client sends one message and half-closes; a silent client is dropped after kClientReadMs
            int left = 2000 - (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            if (left <= 0) break;
            pollfd cp = {c, POLLIN, 0};
            int r = ::poll(&cp, 1, left);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            ssize_t n = ::read(c, buf + got, kPipeMsgMax - got);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { ok = got > 0; break; }   // EOF: the message is complete
            got += (size_t)n;
            if (got >= kPipeMsgMax) { ok = true; break; }
        }
        if (ok) {
            bool accepted = handle_message(winutil::to_wide(std::string(buf, got)));
            const char* reply = accepted ? "OK" : "ERR";
            ssize_t ignored = ::write(c, reply, accepted ? 2 : 3);
            (void)ignored;
        }
        ::close(c);
    }
}

bool InviteServer::start(const std::wstring& pipe_name) {
    if (impl_) return false;
    pipe_ = pipe_name;
    const std::string path = socket_path(pipe_name);
    sockaddr_un addr;
    if (!make_addr(path, &addr)) return false;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    ::unlink(path.c_str());   // we hold the single-instance lock, so a socket file here is stale
    if (::bind(fd, (sockaddr*)&addr, sizeof addr) != 0 || ::listen(fd, 8) != 0) { ::close(fd); return false; }
    ::chmod(path.c_str(), 0600);   // only the current user may talk to it
    int p[2];
    if (::pipe(p) != 0) { ::close(fd); ::unlink(path.c_str()); return false; }
    ::fcntl(p[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(p[1], F_SETFD, FD_CLOEXEC);
    impl_ = new Impl();
    impl_->listen_fd = fd;
    impl_->wake_r = p[0];
    impl_->wake_w = p[1];
    impl_->path = path;
    impl_->thread = std::thread([im = impl_] { serve(im); });
    return true;
}

void InviteServer::stop() {
    if (!impl_) return;
    impl_->stop = true;
    char b = 1;
    ssize_t ignored = ::write(impl_->wake_w, &b, 1);
    (void)ignored;
    if (impl_->thread.joinable()) impl_->thread.join();
    ::close(impl_->listen_fd);
    ::close(impl_->wake_r);
    ::close(impl_->wake_w);
    ::unlink(impl_->path.c_str());
    delete impl_;
    impl_ = nullptr;
}

void InviteServer::push(int kind, const std::wstring& text) { queue_push(kind, text); }

bool InviteServer::pop(int kind, std::wstring* text) {
    std::lock_guard<std::mutex> l(g_q_mu);
    for (size_t i = 0; i < g_queue.size(); ++i) {
        if (g_queue[i].kind != kind) continue;
        if (text) *text = g_queue[i].text;
        g_queue.erase(g_queue.begin() + (long)i);
        return true;
    }
    return false;
}

bool InviteServer::take(std::wstring* code) { return pop(0, code); }
bool InviteServer::take_play(std::wstring* key) { return pop(1, key); }
bool InviteServer::take_account(std::wstring* code) { return pop(3, code); }
bool InviteServer::take_night(std::wstring* id) { return pop(4, id); }
bool InviteServer::take_show() { return pop(2, nullptr); }
#endif  // _WIN32

bool invite_is_valid_game_key(const std::wstring& key) {
    if (key.empty() || key.size() > 120) return false;
    /* Library keys are "<system>:<id>" (library.h), and ROM hacks add "+h<crc>" (hack_logic.h); without ':' every real game
     * was refused ("Add to desktop", --play). The key is only ever looked up in the library, never used as a path. */
    for (wchar_t c : key)
        if (!((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'.' || c == L'_' || c == L'-' || c == L':' || c == L'+')) return false;
    return true;
}

bool invite_parse_play(const std::wstring& cmdline, std::wstring* key) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdline.c_str(), &argc);
    if (!argv) return false;
    bool found = false;
    for (int i = 0; i < argc && !found; ++i) {
        if (_wcsicmp(argv[i], L"--play") == 0 && i + 1 < argc && invite_is_valid_game_key(argv[i + 1])) {
            if (key) *key = argv[i + 1];
            found = true;
        }
    }
    LocalFree(argv);
    return found;
}

std::wstring invite_play_arguments(const std::wstring& key) { return L"--play " + key; }

#if defined(_WIN32)
static bool pipe_call(const std::wstring& pipe_name, const std::wstring& text, DWORD timeout_ms) {
    std::string msg_utf8 = winutil::to_utf8(text);
    std::vector<char> msg(msg_utf8.begin(), msg_utf8.end());
    DWORD deadline = GetTickCount() + timeout_ms;
    for (;;) {
        char reply[8] = {};
        DWORD got = 0;
        if (CallNamedPipeW(pipe_name.c_str(), msg.data(), (DWORD)msg.size(), reply, sizeof(reply), &got, 500)) {
            return got >= 2 && reply[0] == 'O' && reply[1] == 'K';
        }
        if ((LONG)(GetTickCount() - deadline) >= 0) return false;   // past the deadline (wrap-safe)
        Sleep(50);   // primary still starting, or busy
    }
}

bool invite_send(const std::wstring& pipe_name, const std::wstring& code, DWORD timeout_ms) {
    std::wstring c = invite_normalize_code(code);
    return invite_is_valid_code(c) && pipe_call(pipe_name, c, timeout_ms);
}
bool invite_send_play(const std::wstring& pipe_name, const std::wstring& key, DWORD timeout_ms) {
    return invite_is_valid_game_key(key) && pipe_call(pipe_name, L"play:" + key, timeout_ms);
}
bool invite_send_night(const std::wstring& pipe_name, const std::wstring& id, DWORD timeout_ms) {
    return invite_is_valid_night_id(id) && pipe_call(pipe_name, L"night:" + id, timeout_ms);
}
bool invite_send_show(const std::wstring& pipe_name, DWORD timeout_ms) {
    return pipe_call(pipe_name, L"show", timeout_ms);
}

bool invite_wait_for_primary(std::unique_ptr<InviteInstance>* inst, const std::wstring& mutex_name,
                             const std::function<bool(DWORD)>& hand_over, DWORD budget_ms, bool* handed) {
    if (handed) *handed = false;
    if (!inst) return false;
    if (*inst && (*inst)->is_primary()) return true;
    const ULONGLONG t0 = GetTickCount64();
    ULONGLONG next_try = t0 + 2000;
    for (;;) {
        inst->reset();   // our own handle must not keep the closing hub's mutex alive
        Sleep(100);
        inst->reset(new InviteInstance(mutex_name));
        if ((*inst)->is_primary()) return true;
        if (GetTickCount64() - t0 >= budget_ms) return false;
        if (hand_over && GetTickCount64() >= next_try) {
            if (hand_over(500)) { if (handed) *handed = true; return false; }
            next_try = GetTickCount64() + 2000;
        }
    }
}
#else
/* One request on the Unix socket: connect (retrying until the deadline while the primary starts), send, half-close, read OK/ERR. */
static bool pipe_call(const std::wstring& pipe_name, const std::wstring& text, unsigned long timeout_ms) {
    const std::string msg = winutil::to_utf8(text);
    sockaddr_un addr;
    if (!make_addr(socket_path(pipe_name), &addr)) return false;
    const auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&] { return (unsigned long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count(); };
    for (;;) {
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0) {
            ::fcntl(fd, F_SETFD, FD_CLOEXEC);
            if (::connect(fd, (sockaddr*)&addr, sizeof addr) == 0) {
                bool ok = false;
                if (::write(fd, msg.data(), msg.size()) == (ssize_t)msg.size()) {
                    ::shutdown(fd, SHUT_WR);
                    char reply[8] = {};
                    size_t got = 0;
                    pollfd p = {fd, POLLIN, 0};
                    while (got < sizeof reply && ::poll(&p, 1, 2500) > 0) {
                        ssize_t n = ::read(fd, reply + got, sizeof reply - got);
                        if (n <= 0) break;
                        got += (size_t)n;
                    }
                    ok = got >= 2 && reply[0] == 'O' && reply[1] == 'K';
                }
                ::close(fd);
                return ok;
            }
            ::close(fd);
        }
        if (elapsed() >= timeout_ms) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));   // primary still starting, or busy
    }
}

bool invite_send(const std::wstring& pipe_name, const std::wstring& code, unsigned long timeout_ms) {
    std::wstring c = invite_normalize_code(code);
    return invite_is_valid_code(c) && pipe_call(pipe_name, c, timeout_ms);
}
bool invite_send_play(const std::wstring& pipe_name, const std::wstring& key, unsigned long timeout_ms) {
    return invite_is_valid_game_key(key) && pipe_call(pipe_name, L"play:" + key, timeout_ms);
}
bool invite_send_night(const std::wstring& pipe_name, const std::wstring& id, unsigned long timeout_ms) {
    return invite_is_valid_night_id(id) && pipe_call(pipe_name, L"night:" + id, timeout_ms);
}
bool invite_send_show(const std::wstring& pipe_name, unsigned long timeout_ms) {
    return pipe_call(pipe_name, L"show", timeout_ms);
}

bool invite_wait_for_primary(std::unique_ptr<InviteInstance>* inst, const std::wstring& mutex_name,
                             const std::function<bool(unsigned long)>& hand_over, unsigned long budget_ms, bool* handed) {
    if (handed) *handed = false;
    if (!inst) return false;
    if (*inst && (*inst)->is_primary()) return true;
    const unsigned long long t0 = plat::tick_ms();
    unsigned long long next_try = t0 + 2000;
    for (;;) {
        inst->reset();   // our own descriptor must not keep the closing hub's lock alive
        plat::sleep_ms(100);
        inst->reset(new InviteInstance(mutex_name));
        if ((*inst)->is_primary()) return true;
        if (plat::tick_ms() - t0 >= budget_ms) return false;
        if (hand_over && plat::tick_ms() >= next_try) {
            if (hand_over(500)) { if (handed) *handed = true; return false; }
            next_try = plat::tick_ms() + 2000;
        }
    }
}
#endif  // _WIN32

bool invite_parse_account_url(const std::wstring& url, std::wstring* code) {
    std::string c;
    if (!hubnet::invite_link_code(winutil::to_utf8(url), &c)) return false;
    if (code) *code = winutil::to_wide(c);
    return true;
}
bool invite_parse_account_command_line(const std::wstring& cmdline, std::wstring* code) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdline.c_str(), &argc);
    if (!argv) return false;
    bool found = false;
    std::wstring result;
    for (int i = 0; i < argc && !found; ++i) {
        if (invite_parse_account_url(argv[i], &result)) found = true;
        else if (_wcsicmp(argv[i], L"--invite") == 0 && i + 1 < argc && hubnet::invite_valid(winutil::to_utf8(argv[i + 1]))) {
            result = winutil::to_wide(hubnet::invite_format(winutil::to_utf8(argv[i + 1])));
            found = true;
        }
    }
    LocalFree(argv);
    if (found && code) *code = result;
    return found;
}
bool invite_send_account(const std::wstring& pipe_name, const std::wstring& code, unsigned long timeout_ms) {
    const std::string c = winutil::to_utf8(code);
    return hubnet::invite_valid(c) && pipe_call(pipe_name, L"acct:" + winutil::to_wide(hubnet::invite_format(c)), timeout_ms);
}
