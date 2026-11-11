#include "hub_log.h"
#include "hub_log_rotate.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace hublog {
namespace {

constexpr int kRing = 64;
constexpr size_t kRingLine = 240;
constexpr size_t kPendingCap = 256 * 1024;

std::mutex g_mu;        // guards g_pending and the ring writes
std::mutex g_io_mu;     // guards the file handle / size / rotation
std::string g_pending;
size_t g_dropped = 0;
HANDLE g_h = INVALID_HANDLE_VALUE;
unsigned long long g_size = 0;
std::wstring g_dir;
Options g_opt;
std::string g_profile;
bool g_ready = false;
HANDLE g_wake = nullptr;
HANDLE g_thread = nullptr;
std::atomic<bool> g_stop{false};
ULONGLONG g_t0 = 0;
char g_ring[kRing][kRingLine];
std::atomic<unsigned> g_ring_n{0};
char g_sys[1536] = "RetroHub (log not started)\n";

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
std::string narrow(const wchar_t* w) {
    if (!w || !*w) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 1 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

bool open_file_locked() {   // g_io_mu held
    if (g_h != INVALID_HANDLE_VALUE) return true;
    CreateDirectoryW(g_dir.substr(0, g_dir.size() - (g_dir.size() && g_dir.back() == L'\\' ? 1 : 0)).c_str(), nullptr);
    g_h = CreateFileW(file_name(g_dir, 0).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    g_size = GetFileSizeEx(g_h, &sz) ? (unsigned long long)sz.QuadPart : 0;
    return true;
}

void write_raw_locked(const char* p, size_t n) {   // g_io_mu held; never throws
    if (!n) return;
    if (!open_file_locked()) return;
    if (needs_rotation(g_size, n, g_opt.max_bytes)) {
        CloseHandle(g_h);
        g_h = INVALID_HANDLE_VALUE;
        rotate(g_dir, g_opt.keep);
        if (!open_file_locked()) return;
        static const char note[] = "--- log rotated; previous lines are in hub.1.log ---\n";
        DWORD w = 0;
        WriteFile(g_h, note, (DWORD)(sizeof note - 1), &w, nullptr);
        g_size += w;
    }
    DWORD w = 0;
    while (n > 0) {
        DWORD chunk = n > (1u << 20) ? (1u << 20) : (DWORD)n;
        if (!WriteFile(g_h, p, chunk, &w, nullptr) || w == 0) break;
        g_size += w; p += w; n -= w;
    }
}

void drain() {   // pending -> disk
    std::string data;
    {
        std::lock_guard<std::mutex> l(g_mu);
        data.swap(g_pending);
        if (g_dropped) { data += "--- " + std::to_string(g_dropped) + " log lines dropped (disk too slow) ---\n"; g_dropped = 0; }
    }
    if (data.empty()) return;
    std::lock_guard<std::mutex> l(g_io_mu);
    write_raw_locked(data.data(), data.size());
}

DWORD WINAPI worker(void*) {
    while (!g_stop.load()) {
        WaitForSingleObject(g_wake, 750);
        try { drain(); } catch (...) {}
    }
    return 0;
}

std::string os_version() {
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
    OSVERSIONINFOW vi;
    std::memset(&vi, 0, sizeof vi);
    vi.dwOSVersionInfoSize = sizeof vi;
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
        if (auto fn = (RtlGetVersionFn)(void*)GetProcAddress(nt, "RtlGetVersion")) fn(&vi);
    char b[96];
    std::snprintf(b, sizeof b, "Windows %lu.%lu build %lu", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
    return b;
}

std::string gpu_name() {
    DISPLAY_DEVICEW dd;
    std::memset(&dd, 0, sizeof dd);
    dd.cb = sizeof dd;
    for (DWORD i = 0; i < 4 && EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) return narrow(dd.DeviceString);
        dd.cb = sizeof dd;
    }
    if (EnumDisplayDevicesW(nullptr, 0, &dd, 0)) return narrow(dd.DeviceString);
    return std::string();
}

std::string build_header(const std::string& version) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    MEMORYSTATUSEX ms;
    std::memset(&ms, 0, sizeof ms);
    ms.dwLength = sizeof ms;
    GlobalMemoryStatusEx(&ms);
    wchar_t loc[LOCALE_NAME_MAX_LENGTH] = L"";
    GetUserDefaultLocaleName(loc, LOCALE_NAME_MAX_LENGTH);
    std::string gpu = gpu_name();
    char b[640];
    std::snprintf(b, sizeof b, "RetroHub %s\n%s\nCPU cores: %lu\nRAM: %llu MB\nGPU: %s\nLocale: %s\n", version.c_str(), os_version().c_str(),
                  (unsigned long)si.dwNumberOfProcessors, (unsigned long long)(ms.ullTotalPhys >> 20), gpu.empty() ? "unknown" : gpu.c_str(), narrow(loc).c_str());
    return b;
}

void stamp(char* out, size_t cap) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d.%03d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
}

void vlog(Level lv, const char* fmt, va_list ap) {
    char buf[1024];
    int n = std::vsnprintf(buf, sizeof buf, fmt ? fmt : "", ap);
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof buf) n = (int)sizeof buf - 1;
    buf[n] = 0;
    write(lv, std::string(buf, (size_t)n));
}

}  // namespace

void init(const std::string& logs_dir_utf8, const std::string& version, const Options& opt) {
    try {
        if (g_ready) return;
        g_opt = opt;
        g_dir = widen(logs_dir_utf8);
        if (!g_dir.empty() && g_dir.back() != L'\\' && g_dir.back() != L'/') g_dir += L'\\';
        g_profile = opt.profile_dir;
        if (g_profile.empty()) {
            wchar_t up[MAX_PATH * 2];
            DWORD n = GetEnvironmentVariableW(L"USERPROFILE", up, (DWORD)(sizeof up / sizeof up[0]));
            if (n > 0 && n < sizeof up / sizeof up[0]) g_profile = narrow(up);
        }
        g_t0 = GetTickCount64();
        g_stop = false;
        std::string head = build_header(version);
        std::snprintf(g_sys, sizeof g_sys, "%s", scrub(head, g_profile).c_str());
        g_ready = true;
        if (opt.header) {
            char ts[48];
            stamp(ts, sizeof ts);
            std::string h = std::string("\n===== RetroHub session started ") + ts + " =====\n" + scrub(head, g_profile);
            std::lock_guard<std::mutex> l(g_io_mu);
            write_raw_locked(h.data(), h.size());
        }
        if (opt.worker) {
            g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            g_thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
        }
    } catch (...) {}
}

void flush() {
    try { if (g_ready) drain(); } catch (...) {}
}

void shutdown() {
    try {
        if (!g_ready) return;
        info("shutdown (%llu ms since start)", since_start_ms());
        bool joined = true;
        if (g_thread) {
            g_stop = true;
            SetEvent(g_wake);
            joined = WaitForSingleObject(g_thread, 2000) == WAIT_OBJECT_0;
            CloseHandle(g_thread);   // a stuck worker (stalled disk) keeps running until the process ends
            g_thread = nullptr;
        }
        if (g_wake && joined) { CloseHandle(g_wake); g_wake = nullptr; }
        g_ready = false;   // no new lines from here on
        if (!joined) {   // the worker may sit inside WriteFile holding g_io_mu: exit must never wait for the log
            if (g_io_mu.try_lock()) {
                if (g_h != INVALID_HANDLE_VALUE) { CloseHandle(g_h); g_h = INVALID_HANDLE_VALUE; }
                g_io_mu.unlock();
            }
            return;
        }
        drain();   // the worker is gone: g_io_mu is free
        std::lock_guard<std::mutex> l(g_io_mu);
        if (g_h != INVALID_HANDLE_VALUE) { CloseHandle(g_h); g_h = INVALID_HANDLE_VALUE; }
    } catch (...) {}
}

void write(Level lv, const std::string& msg) {
    try {
        if (!g_ready) return;
        char ts[48];
        stamp(ts, sizeof ts);
        std::string m = scrub(msg, g_profile);
        for (char& c : m) if (c == '\n' || c == '\r') c = ' ';
        const char* tag = lv == Level::Error ? "ERROR" : lv == Level::Warn ? "WARN " : "INFO ";
        std::string line = std::string(ts) + " " + tag + " " + m + "\n";
        {
            std::lock_guard<std::mutex> l(g_mu);
            if (g_pending.size() + line.size() <= kPendingCap) g_pending += line;
            else ++g_dropped;
            unsigned n = g_ring_n.load(std::memory_order_relaxed);
            char* slot = g_ring[n % kRing];
            size_t len = line.size() - 1 < kRingLine - 1 ? line.size() - 1 : kRingLine - 1;
            std::memcpy(slot, line.data(), len);
            slot[len] = 0;
            g_ring_n.store(n + 1, std::memory_order_release);
        }
        if (lv == Level::Error && g_wake) SetEvent(g_wake);
    } catch (...) {}
}

void info(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vlog(Level::Info, fmt, ap); va_end(ap); }
void warn(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vlog(Level::Warn, fmt, ap); va_end(ap); }
void error(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vlog(Level::Error, fmt, ap); va_end(ap); }

unsigned long long since_start_ms() { return g_t0 ? (unsigned long long)(GetTickCount64() - g_t0) : 0; }
const char* system_info() { return g_sys; }

size_t recent_lines(char* out, size_t cap, int max_lines) {
    if (!out || cap == 0) return 0;
    out[0] = 0;
    unsigned n = g_ring_n.load(std::memory_order_acquire);
    unsigned have = n < (unsigned)kRing ? n : (unsigned)kRing;
    if (max_lines > 0 && have > (unsigned)max_lines) have = (unsigned)max_lines;
    size_t pos = 0;
    for (unsigned i = n - have; i < n; ++i) {
        const char* s = g_ring[i % kRing];
        size_t len = 0;
        while (len < kRingLine && s[len]) ++len;
        if (pos + len + 2 > cap) break;
        std::memcpy(out + pos, s, len);
        pos += len;
        out[pos++] = '\n';
    }
    out[pos] = 0;
    return pos;
}

void flush_from_crash() {
    if (!g_ready) return;
    if (!g_mu.try_lock()) return;
    if (!g_io_mu.try_lock()) { g_mu.unlock(); return; }
    if (g_h != INVALID_HANDLE_VALUE && !g_pending.empty()) {   // no allocation, no free: write in place, the process is about to die
        DWORD w = 0;
        WriteFile(g_h, g_pending.data(), (DWORD)g_pending.size(), &w, nullptr);
    }
    g_io_mu.unlock();
    g_mu.unlock();
}

}  // namespace hublog
