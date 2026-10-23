/* Clickable invite links: parsing, link building, and the single-instance pipe handoff (see invite.h). */
#include "invite.h"

#include <shellapi.h>

#include <cstring>
#include <cwctype>
#include <vector>

const wchar_t* const INVITE_MUTEX_NAME = L"Local\\RetroHub.SingleInstance";
const wchar_t* const INVITE_PIPE_NAME = L"\\\\.\\pipe\\RetroHub.Invite";

namespace {

const wchar_t kWebBase[] = L"https://smash-netplay.jumpcircuit.workers.dev/j/";
const wchar_t kScheme[] = L"retrohub://join/";
const size_t kPipeMsgMax = 64;

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

void InviteServer::run() {
    while (!InterlockedCompareExchange(&stop_, 0, 0)) {
        HANDLE pipe = CreateNamedPipeW(pipe_.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                       PIPE_UNLIMITED_INSTANCES, 256, 256, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) { Sleep(200); continue; }
        BOOL connected = ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
        if (connected && !InterlockedCompareExchange(&stop_, 0, 0)) {
            char buf[kPipeMsgMax + 1] = {};
            DWORD got = 0;
            bool ok = ReadFile(pipe, buf, (DWORD)kPipeMsgMax, &got, nullptr) != FALSE;
            std::wstring text;
            if (ok && got > 0) {
                int n = MultiByteToWideChar(CP_UTF8, 0, buf, (int)got, nullptr, 0);
                if (n > 0) { text.resize(n); MultiByteToWideChar(CP_UTF8, 0, buf, (int)got, &text[0], n); }
            }
            std::wstring code = invite_normalize_code(text);
            bool accepted = invite_is_valid_code(code);
            if (accepted) {
                EnterCriticalSection(&lock_);
                if (count_ < 16) queue_[count_++] = code;
                LeaveCriticalSection(&lock_);
            }
            const char* reply = accepted ? "OK" : "ERR";
            DWORD wrote = 0;
            WriteFile(pipe, reply, 2 + (accepted ? 0 : 1), &wrote, nullptr);
            FlushFileBuffers(pipe);
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
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

bool InviteServer::take(std::wstring* code) {
    if (!lock_ready_) return false;
    EnterCriticalSection(&lock_);
    bool got = count_ > 0;
    if (got) {
        *code = queue_[0];
        for (int i = 1; i < count_; ++i) queue_[i - 1] = queue_[i];
        --count_;
    }
    LeaveCriticalSection(&lock_);
    return got;
}

bool invite_send(const std::wstring& pipe_name, const std::wstring& code, DWORD timeout_ms) {
    std::wstring c = invite_normalize_code(code);
    if (!invite_is_valid_code(c)) return false;
    int n = WideCharToMultiByte(CP_UTF8, 0, c.c_str(), (int)c.size(), nullptr, 0, nullptr, nullptr);
    std::vector<char> msg((size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, c.c_str(), (int)c.size(), msg.data(), n, nullptr, nullptr);
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
