/* Clickable invite links: parsing, link building, and the single-instance pipe handoff (see invite.h). */
#include "invite.h"

#include <shellapi.h>
#include <sddl.h>

#include <cstring>
#include <cwctype>
#include <vector>

const wchar_t* const INVITE_MUTEX_NAME = L"Local\\RetroHub.SingleInstance";
const wchar_t* const INVITE_PIPE_NAME = L"\\\\.\\pipe\\RetroHub.Invite";

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

bool invite_parse_url(const std::wstring& url, std::wstring* code) {
    if (!starts_with_ci(url, kScheme)) return false;
    std::wstring rest = url.substr(wcslen(kScheme));
    if (!rest.empty() && rest.back() == L'/') rest.pop_back();
    std::wstring c = invite_normalize_code(rest);
    if (!invite_is_valid_code(c)) return false;
    if (code) *code = c;
    return true;
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

std::wstring invite_web_link(const std::wstring& code) {
    std::wstring c = invite_normalize_code(code);
    if (!invite_is_valid_code(c)) return L"";
    return std::wstring(kWebBase) + c;
}

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

std::wstring invite_protocol_command(const std::wstring& exe) {
    return L"\"" + exe + L"\" \"%1\"";
}

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
                int n = MultiByteToWideChar(CP_UTF8, 0, buf, (int)got, nullptr, 0);
                if (n > 0) { text.resize(n); MultiByteToWideChar(CP_UTF8, 0, buf, (int)got, &text[0], n); }
            }
            bool accepted = false;
            if (text == L"show") { push(2, L""); accepted = true; }
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
bool InviteServer::take_show() { return pop(2, nullptr); }

bool invite_is_valid_game_key(const std::wstring& key) {
    if (key.empty() || key.size() > 120) return false;
    for (wchar_t c : key)
        if (!((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'.' || c == L'_' || c == L'-')) return false;
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

static bool pipe_call(const std::wstring& pipe_name, const std::wstring& text, DWORD timeout_ms) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::vector<char> msg((size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), msg.data(), n, nullptr, nullptr);
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
bool invite_send_show(const std::wstring& pipe_name, DWORD timeout_ms) {
    return pipe_call(pipe_name, L"show", timeout_ms);
}
