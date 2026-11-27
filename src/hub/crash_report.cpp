#include "crash_report.h"
#include "hub_log.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2
#endif
#include <windows.h>
#include <psapi.h>
#endif

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <exception>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <set>
#include <sstream>
#if defined(_WIN32)
#include <crtdbg.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <ctime>
#include <unistd.h>
#include <sys/ucontext.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#endif

namespace crashrep {

/* ------------------------------------------------------------------ pure formatter (no heap, no CRT formatting) */
namespace {

struct Out {
    char* p; size_t cap; size_t n;
    void str(const char* s) { if (!s) return; while (*s && n + 1 < cap) p[n++] = *s++; p[n] = 0; }
    void ch(char c) { if (n + 1 < cap) { p[n++] = c; p[n] = 0; } }
    void hex(uint64_t v, int digits) {
        static const char* d = "0123456789abcdef";
        char b[16]; int k = 0;
        do { b[k++] = d[v & 15]; v >>= 4; } while (v && k < 16);
        str("0x");
        for (int i = k; i < digits; ++i) ch('0');
        while (k) ch(b[--k]);
    }
    void dec(unsigned long long v) {
        char b[24]; int k = 0;
        do { b[k++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (k) ch(b[--k]);
    }
    void pad_to(size_t col, size_t line_start) { while (n - line_start < col && n + 1 < cap) ch(' '); }
};

void put_addr(Out& o, const Module* m, int nm, uint64_t a) {
    o.hex(a, 16);
    int idx; uint64_t off;
    if (locate(m, nm, a, &idx, &off)) { o.str("  "); o.str(m[idx].name); o.str("+"); o.hex(off, 0); }
}

}  // namespace

const char* exception_name(uint32_t c) {
    switch (c) {
        case 0xC0000005u: return "ACCESS_VIOLATION";
        case 0xC0000006u: return "IN_PAGE_ERROR";
        case 0xC000000Du: return "INVALID_PARAMETER";
        case 0xC0000025u: return "NONCONTINUABLE_EXCEPTION";
        case 0xC000001Du: return "ILLEGAL_INSTRUCTION";
        case 0xC0000094u: return "INT_DIVIDE_BY_ZERO";
        case 0xC0000095u: return "INT_OVERFLOW";
        case 0xC0000096u: return "PRIVILEGED_INSTRUCTION";
        case 0xC000008Cu: return "ARRAY_BOUNDS_EXCEEDED";
        case 0xC000008Eu: return "FLT_DIVIDE_BY_ZERO";
        case 0xC00000FDu: return "STACK_OVERFLOW";
        case 0xC0000374u: return "HEAP_CORRUPTION";
        case 0xC0000409u: return "STACK_BUFFER_OVERRUN (fast fail)";
        case 0x80000002u: return "DATATYPE_MISALIGNMENT";
        case 0x80000003u: return "BREAKPOINT";
        case 0xE06D7363u: return "C++ exception";
        case 0xE0485001u: return "STD_TERMINATE";
        case 0xE0485002u: return "PURE_VIRTUAL_CALL";
        case 0xE0485003u: return "ABORT";
#if !defined(_WIN32)
        case 0x53000004u: return "SIGILL (illegal instruction)";
        case 0x53000005u: return "SIGTRAP (breakpoint / trap)";
        case 0x53000006u: return "SIGABRT (abort)";
        case 0x53000008u: return "SIGFPE (arithmetic)";
        case 0x5300000Au: return "SIGBUS (bus error)";
        case 0x5300000Bu: return "SIGSEGV (access violation)";
        case 0x53000100u: return "STD_TERMINATE";
#endif
        default: return "unknown";
    }
}

bool locate(const Module* mods, int n, uint64_t addr, int* index, uint64_t* offset) {
    for (int i = 0; i < n; ++i)
        if (addr >= mods[i].base && addr < mods[i].base + mods[i].size) {
            if (index) *index = i;
            if (offset) *offset = addr - mods[i].base;
            return true;
        }
    return false;
}

size_t format_report(const Input& in, char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = 0;
    Out o{out, cap, 0};
    o.str("RetroHub crash report\n=====================\n");
    o.str("Version:      "); o.str(in.version); o.ch('\n');
    o.str("Time:         "); o.str(in.time_text); o.ch('\n');
    o.str("Uptime:       "); o.dec(in.uptime_ms / 1000); o.str(" s\n");
    o.str("Kind:         "); o.str(in.kind); o.ch('\n');
    o.str("Exception:    "); o.hex(in.code, 8); o.str("  "); o.str(exception_name(in.code)); o.ch('\n');
    o.str("Address:      "); put_addr(o, in.modules, in.n_modules, in.address); o.ch('\n');
    if (in.code == 0xC0000005u && in.n_params >= 2) {
        o.str("Access:       ");
        o.str(in.params[0] == 0 ? "read of " : in.params[0] == 1 ? "write to " : in.params[0] == 8 ? "execute at " : "access of ");
        o.hex(in.params[1], 0); o.ch('\n');
    }
    o.str("Thread:       "); o.dec(in.thread_id); o.ch('\n');
    o.str("UI phase:     "); o.str(in.phase && *in.phase ? in.phase : "(none)"); o.ch('\n');
    o.str("\nSystem\n------\n"); o.str(in.system);
    if (in.n_regs > 0) {
        o.str("\nRegisters\n---------\n");
        for (int i = 0; i < in.n_regs; ++i) {
            size_t ls = o.n;
            o.str(in.reg_names[i]); o.pad_to(4, ls); o.hex(in.reg_values[i], 16);
            o.str((i % 2 == 1 || i == in.n_regs - 1) ? "\n" : "    ");
        }
    }
    o.str("\nStack scan (values that point into a loaded module, innermost first)\n--------------------------------------------------------------------\n");
    for (int i = 0; i < in.n_stack; ++i) { o.str("  "); put_addr(o, in.modules, in.n_modules, in.stack[i]); o.ch('\n'); }
    if (in.n_stack == 0) o.str("  (none found)\n");
    o.str("\nModules\n-------\n");
    for (int i = 0; i < in.n_modules; ++i) {
        o.str("  "); o.hex(in.modules[i].base, 16); o.str("  size "); o.hex(in.modules[i].size, 0); o.str("  "); o.str(in.modules[i].name); o.ch('\n');
    }
    o.str("\nLast hub log lines\n------------------\n");
    o.str(in.log_tail && *in.log_tail ? in.log_tail : "(none)\n");
    return o.n;
}

#if defined(_WIN32)
/* ------------------------------------------------------------------ handler */
namespace {

constexpr int kMaxMods = 256;
constexpr int kMaxStack = 48;

wchar_t g_dir[MAX_PATH * 2];
char g_version[64];
Module g_mods[kMaxMods];
Module g_cached[kMaxMods];
int g_n_cached = 0;
HMODULE g_hm[kMaxMods];
uint64_t g_stack[kMaxStack];
char g_logtail[12 * 1024];
char g_out[96 * 1024];
char g_time[40];
wchar_t g_path[MAX_PATH * 2 + 40];
const char* (*g_phase_fn)() = nullptr;
volatile LONG g_busy = 0;
HANDLE g_ev_go = nullptr, g_ev_done = nullptr, g_thread = nullptr;

struct Req { EXCEPTION_POINTERS* ep; const char* kind; void* stack_base; DWORD tid; } g_req;

#if defined(_M_X64)
const char* const kRegNames[] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rip", "efl"};
uint64_t g_regs[18];
constexpr int kNRegs = 18;
#else
const char* const kRegNames[] = {"pc"};
uint64_t g_regs[1];
constexpr int kNRegs = 0;
#endif

int enum_modules(Module* dst, int cap) {
    DWORD need = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), g_hm, (DWORD)sizeof g_hm, &need)) return 0;
    int n = (int)(need / sizeof(HMODULE));
    if (n > kMaxMods) n = kMaxMods;
    if (n > cap) n = cap;
    int k = 0;
    for (int i = 0; i < n; ++i) {
        MODULEINFO mi;
        wchar_t wn[128];
        if (!K32GetModuleInformation(GetCurrentProcess(), g_hm[i], &mi, sizeof mi)) continue;
        DWORD len = K32GetModuleBaseNameW(GetCurrentProcess(), g_hm[i], wn, 128);
        Module& m = dst[k++];
        m.base = (uint64_t)(uintptr_t)mi.lpBaseOfDll;
        m.size = (uint64_t)mi.SizeOfImage;
        int w = len ? WideCharToMultiByte(CP_UTF8, 0, wn, (int)len, m.name, (int)sizeof m.name - 1, nullptr, nullptr) : 0;
        m.name[w > 0 ? w : 0] = 0;
    }
    return k;
}

int scan_stack(uint64_t sp, uint64_t top, const Module* mods, int nm) {
    int found = 0;
    __try {
        if (!sp || sp >= top) return 0;
        uint64_t lim = sp + 4096 * 8;
        if (top && lim > top) lim = top;
        for (uint64_t p = sp & ~7ull; p + 8 <= lim && found < kMaxStack; p += 8) {
            uint64_t v = *(volatile uint64_t*)p;
            if (v && locate(mods, nm, v, nullptr, nullptr)) g_stack[found++] = v;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return found;
}

void put_dec2(char* p, unsigned v, int w) { for (int i = w - 1; i >= 0; --i) { p[i] = (char)('0' + v % 10); v /= 10; } }

void do_report() {
    hublog::flush_from_crash();
    EXCEPTION_POINTERS* ep = g_req.ep;
    Input in;
    in.kind = g_req.kind;
    in.version = g_version;
    in.system = hublog::system_info();
    in.thread_id = g_req.tid;
    in.uptime_ms = hublog::since_start_ms();
    in.phase = g_phase_fn ? g_phase_fn() : nullptr;
    SYSTEMTIME t;
    GetLocalTime(&t);
    put_dec2(g_time, t.wYear, 4); g_time[4] = '-'; put_dec2(g_time + 5, t.wMonth, 2); g_time[7] = '-'; put_dec2(g_time + 8, t.wDay, 2);
    g_time[10] = ' '; put_dec2(g_time + 11, t.wHour, 2); g_time[13] = ':'; put_dec2(g_time + 14, t.wMinute, 2); g_time[16] = ':'; put_dec2(g_time + 17, t.wSecond, 2); g_time[19] = 0;
    in.time_text = g_time;
    int nm = enum_modules(g_mods, kMaxMods);
    if (nm == 0) { std::memcpy(g_mods, g_cached, sizeof(Module) * (size_t)g_n_cached); nm = g_n_cached; }
    in.modules = g_mods; in.n_modules = nm;
    uint64_t sp = 0;
    if (ep && ep->ExceptionRecord) {
        in.code = ep->ExceptionRecord->ExceptionCode;
        in.address = (uint64_t)(uintptr_t)ep->ExceptionRecord->ExceptionAddress;
        in.n_params = (int)ep->ExceptionRecord->NumberParameters;
        if (in.n_params >= 2) { in.params[0] = (uint64_t)ep->ExceptionRecord->ExceptionInformation[0]; in.params[1] = (uint64_t)ep->ExceptionRecord->ExceptionInformation[1]; }
    }
#if defined(_M_X64)
    if (ep && ep->ContextRecord) {
        const CONTEXT* c = ep->ContextRecord;
        const uint64_t v[18] = {c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rbp, c->Rsp, c->R8, c->R9, c->R10, c->R11, c->R12, c->R13, c->R14, c->R15, c->Rip, c->EFlags};
        std::memcpy(g_regs, v, sizeof v);
        sp = c->Rsp;
        in.reg_names = kRegNames; in.reg_values = g_regs; in.n_regs = kNRegs;
        if (in.address == 0) in.address = c->Rip;
    }
#endif
    in.n_stack = scan_stack(sp, (uint64_t)(uintptr_t)g_req.stack_base, g_mods, nm);
    in.stack = g_stack;
    hublog::recent_lines(g_logtail, sizeof g_logtail, 50);
    in.log_tail = g_logtail;
    size_t len = format_report(in, g_out, sizeof g_out);

    wchar_t* q = g_path;
    for (const wchar_t* s = g_dir; *s && q < g_path + MAX_PATH * 2 - 1; ++s) *q++ = *s;
    const wchar_t* pre = L"crash-";
    while (*pre) *q++ = *pre++;
    char d[20];
    put_dec2(d, t.wYear, 4); put_dec2(d + 4, t.wMonth, 2); put_dec2(d + 6, t.wDay, 2); d[8] = '-';
    put_dec2(d + 9, t.wHour, 2); put_dec2(d + 11, t.wMinute, 2); put_dec2(d + 13, t.wSecond, 2);
    for (int i = 0; i < 15; ++i) *q++ = (wchar_t)d[i];
    wchar_t* const stem_end = q;
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int attempt = 1; attempt <= 9 && h == INVALID_HANDLE_VALUE; ++attempt) {   // two crashes in one second never overwrite each other
        q = stem_end;
        if (attempt > 1) { *q++ = L'-'; *q++ = (wchar_t)(L'0' + attempt); }
        const wchar_t* ext = L".txt";
        while (*ext) *q++ = *ext++;
        *q = 0;
        h = CreateFileW(g_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) break;
    }
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(h, g_out, (DWORD)len, &w, nullptr);
        FlushFileBuffers(h);
        CloseHandle(h);
    }
}

DWORD WINAPI crash_thread(void*) {
    WaitForSingleObject(g_ev_go, INFINITE);
    do_report();
    SetEvent(g_ev_done);
    return 0;
}

/* The report is written by a pre-created thread: a stack overflow leaves the crashing thread with almost no stack, and the
 * crashing thread may hold the heap or loader lock. The crashing thread only signals and waits (bounded). */
void request(EXCEPTION_POINTERS* ep, const char* kind) {
    if (InterlockedExchange(&g_busy, 1)) return;   // a second crash while reporting: do not recurse
    g_req.ep = ep;
    g_req.kind = kind;
    g_req.stack_base = ((NT_TIB*)NtCurrentTeb())->StackBase;
    g_req.tid = GetCurrentThreadId();
    if (g_thread && g_ev_go) {
        SetEvent(g_ev_go);
        WaitForSingleObject(g_ev_done, 10000);
    } else {
        do_report();
    }
}

LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* ep);
LPTOP_LEVEL_EXCEPTION_FILTER g_prev = nullptr;

LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* ep) {
    /* reassert() chains to the overlay / vendor filter that had replaced ours, and such a filter usually chains to the one it replaced:
     * ours. Without this guard that is filter -> theirs -> filter -> ... until the stack is gone, and the process hung instead of ending.
     * Called back on the same thread: step aside and let the exception go on to Windows. (Plain TLS flag: no allocation, no stack.) */
    static thread_local bool t_in_filter = false;
    if (t_in_filter) return EXCEPTION_CONTINUE_SEARCH;
    t_in_filter = true;
    request(ep, "Unhandled exception");
    LONG r = EXCEPTION_CONTINUE_SEARCH;   // Windows Error Reporting still runs; this handler shows no dialog itself
    if (g_prev && g_prev != unhandled_filter) r = g_prev(ep);   // a filter that was installed before ours (vendor / overlay DLL) still gets its turn
    t_in_filter = false;
    return r;
}

void report_here(const char* kind, uint32_t code) {
    static CONTEXT ctx;
    static EXCEPTION_RECORD rec;
    static EXCEPTION_POINTERS ep;
    RtlCaptureContext(&ctx);
    rec.ExceptionCode = code;
#if defined(_M_X64)
    rec.ExceptionAddress = (PVOID)ctx.Rip;
#endif
    ep.ExceptionRecord = &rec;
    ep.ContextRecord = &ctx;
    request(&ep, kind);
}

[[noreturn]] void die() {
    RaiseFailFastException(nullptr, nullptr, 0);   // documented; lets WER see the death, no handler runs again
    TerminateProcess(GetCurrentProcess(), 0xC0000409u);
    for (;;) {}
}

void __cdecl invalid_param(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    report_here("CRT invalid parameter", 0xC000000Du);
    die();
}
void __cdecl pure_call() {
    report_here("Pure virtual call", 0xE0485002u);
    die();
}
void terminate_handler() {
    report_here("std::terminate (uncaught C++ exception or abort)", 0xE0485001u);
    die();
}
/* std::set_terminate only covers the thread that called it: the UCRT keeps the terminate handler per thread. Any other thread (every
 * std::thread worker: its entry is noexcept, so an exception that escapes it calls std::terminate there) ends in the default handler,
 * abort(), which fast-fails past the unhandled-exception filter. The SIGABRT handler is process-wide, so it catches those (and any other
 * abort()) and still writes the report. */
void __cdecl sigabrt_handler(int) {
    report_here("abort() (an uncaught C++ exception on a worker thread, or abort)", 0xE0485003u);
    die();
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
std::string narrow_w(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

bool is_report_name(const std::wstring& n) {
    /* crash-yyyymmdd-hhmmss.txt, or crash-yyyymmdd-hhmmss-N.txt when two reports share a second */
    if (n.size() < 6 + 15 + 4 || n.size() > 6 + 15 + 2 + 4 || n.compare(0, 6, L"crash-") != 0 || n.compare(n.size() - 4, 4, L".txt") != 0) return false;
    for (size_t i = 6; i < n.size() - 4; ++i)
        if (!((n[i] >= L'0' && n[i] <= L'9') || n[i] == L'-')) return false;
    return true;
}

}  // namespace

void install(const std::string& logs_dir_utf8, const std::string& version, const char* (*phase_fn)()) {
    try {
        static bool done = false;
        if (done) return;
        done = true;
        std::wstring d = widen(logs_dir_utf8);
        if (d.empty() || (d.back() != L'\\' && d.back() != L'/')) d += L'\\';
        if (d.size() >= MAX_PATH * 2 - 30) return;
        std::memcpy(g_dir, d.c_str(), (d.size() + 1) * sizeof(wchar_t));
        CreateDirectoryW(d.substr(0, d.size() - 1).c_str(), nullptr);
        std::snprintf(g_version, sizeof g_version, "%s", version.c_str());
        g_phase_fn = phase_fn;
        g_n_cached = enum_modules(g_cached, kMaxMods);
        // keep the newest 10 reports
        {
            std::vector<std::string> names = list_reports(logs_dir_utf8);
            for (size_t i = 10; i < names.size(); ++i) {
                std::error_code ec;
                std::filesystem::remove(std::filesystem::u8path(logs_dir_utf8) / std::filesystem::u8path(names[i]), ec);
            }
        }
        g_ev_go = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_ev_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_thread = CreateThread(nullptr, 64 * 1024, crash_thread, nullptr, 0, nullptr);
        _set_abort_behavior(0, _WRITE_ABORT_MSG);   // abort() must never show a message box
        _set_invalid_parameter_handler(invalid_param);
        _set_purecall_handler(pure_call);
        std::set_terminate(terminate_handler);   // this (the main) thread only
        std::signal(SIGABRT, sigabrt_handler);   // every thread: abort(), including std::terminate's default on a worker thread
        g_prev = SetUnhandledExceptionFilter(unhandled_filter);
    } catch (...) {}
}

bool reassert() {
    LPTOP_LEVEL_EXCEPTION_FILTER cur = SetUnhandledExceptionFilter(unhandled_filter);
    if (cur == unhandled_filter) return false;
    if (cur) g_prev = cur;   // someone replaced ours: keep theirs as the one we chain to
    return true;
}

#else
/* macOS: signal handlers (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT) on an alternate stack write the same plain-text crash
 * note as Windows (no minidump: macOS writes its own .ips report in ~/Library/Logs/DiagnosticReports, which still happens because the
 * handler restores the default action and re-raises). Static buffers only; the report is formatted by the shared format_report(). */
namespace {

constexpr int kMaxMods = 256;
constexpr int kMaxStack = 48;

char g_dir[1024];
char g_version[64];
Module g_mods[kMaxMods];
int g_n_mods = 0;
uint64_t g_stack[kMaxStack];
char g_logtail[12 * 1024];
char g_out[96 * 1024];
char g_time[40];
char g_path[1100];
const char* (*g_phase_fn)() = nullptr;
volatile sig_atomic_t g_busy = 0;
unsigned long long g_t0_ms = 0;
char g_altstack[64 * 1024];

#if defined(__aarch64__)
const char* const kRegNames[] = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
                                 "x18", "x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28", "fp", "lr", "sp", "pc"};
uint64_t g_regs[33];
constexpr int kNRegs = 33;
#else
const char* const kRegNames[] = {"pc"};
uint64_t g_regs[1];
constexpr int kNRegs = 0;
#endif

void load_modules() {   // called from install(): dyld calls are not signal safe
    g_n_mods = 0;
    uint32_t n = _dyld_image_count();
    for (uint32_t i = 0; i < n && g_n_mods < kMaxMods; ++i) {
        const mach_header* h = _dyld_get_image_header(i);
        const char* nm = _dyld_get_image_name(i);
        if (!h || !nm) continue;
#if defined(__LP64__)
        unsigned long sz = 0;
        if (!getsegmentdata((const mach_header_64*)h, "__TEXT", &sz)) continue;
        Module& m = g_mods[g_n_mods++];
        m.base = (uint64_t)(uintptr_t)h;
        m.size = (uint64_t)sz;
        const char* base = std::strrchr(nm, '/');
        std::snprintf(m.name, sizeof m.name, "%s", base ? base + 1 : nm);
#endif
    }
}

void put_dec2(char* p, unsigned v, int w) { for (int i = w - 1; i >= 0; --i) { p[i] = (char)('0' + v % 10); v /= 10; } }

int scan_stack(uint64_t sp, uint64_t top, const Module* mods, int nm) {
    int found = 0;
    if (!sp || sp >= top) return 0;
    uint64_t lim = sp + 4096 * 8;
    if (top && lim > top) lim = top;
    for (uint64_t p = sp & ~7ull; p + 8 <= lim && found < kMaxStack; p += 8) {
        uint64_t v = *(volatile uint64_t*)p;
        if (v && locate(mods, nm, v, nullptr, nullptr)) g_stack[found++] = v;
    }
    return found;
}

void do_report(const char* kind, uint32_t code, uint64_t addr, uint64_t fault_addr, ucontext_t* uc) {
    hublog::flush_from_crash();
    Input in;
    in.kind = kind;
    in.code = code;
    in.version = g_version;
    in.system = hublog::system_info();
    in.thread_id = (uint32_t)(uintptr_t)pthread_mach_thread_np(pthread_self());
    in.uptime_ms = hublog::since_start_ms();
    in.phase = g_phase_fn ? g_phase_fn() : nullptr;
    std::time_t now = std::time(nullptr);
    std::tm t;
    localtime_r(&now, &t);
    put_dec2(g_time, (unsigned)(t.tm_year + 1900), 4); g_time[4] = '-'; put_dec2(g_time + 5, (unsigned)t.tm_mon + 1, 2); g_time[7] = '-'; put_dec2(g_time + 8, (unsigned)t.tm_mday, 2);
    g_time[10] = ' '; put_dec2(g_time + 11, (unsigned)t.tm_hour, 2); g_time[13] = ':'; put_dec2(g_time + 14, (unsigned)t.tm_min, 2); g_time[16] = ':'; put_dec2(g_time + 17, (unsigned)t.tm_sec, 2); g_time[19] = 0;
    in.time_text = g_time;
    in.modules = g_mods; in.n_modules = g_n_mods;
    in.address = addr;
    uint64_t sp = 0;
#if defined(__aarch64__)
    if (uc && uc->uc_mcontext) {
        const __darwin_arm_thread_state64* ss = &uc->uc_mcontext->__ss;
        for (int i = 0; i < 29; ++i) g_regs[i] = ss->__x[i];
        g_regs[29] = __darwin_arm_thread_state64_get_fp(*ss);
        g_regs[30] = (uint64_t)(uintptr_t)__darwin_arm_thread_state64_get_lr_fptr(*ss);
        g_regs[31] = __darwin_arm_thread_state64_get_sp(*ss);
        g_regs[32] = __darwin_arm_thread_state64_get_pc(*ss);
        sp = g_regs[31];
        in.reg_names = kRegNames; in.reg_values = g_regs; in.n_regs = kNRegs;
        if (in.address == 0) in.address = g_regs[32];
    }
#else
    (void)uc;
#endif
    if (fault_addr) { in.n_params = 2; in.params[0] = 0; in.params[1] = fault_addr; }
    uint64_t top = sp ? (uint64_t)(uintptr_t)pthread_get_stackaddr_np(pthread_self()) : 0;
    if (top && sp < top - (uint64_t)pthread_get_stacksize_np(pthread_self()) + 65536) top = 0;   // stack overflow: sp sits in the guard page, do not read it
    in.n_stack = scan_stack(sp, top, g_mods, g_n_mods);
    in.stack = g_stack;
    hublog::recent_lines(g_logtail, sizeof g_logtail, 50);
    in.log_tail = g_logtail;
    size_t len = format_report(in, g_out, sizeof g_out);

    size_t dl = std::strlen(g_dir);
    std::memcpy(g_path, g_dir, dl);
    char* q = g_path + dl;
    std::memcpy(q, "crash-", 6); q += 6;
    put_dec2(q, (unsigned)(t.tm_year + 1900), 4); put_dec2(q + 4, (unsigned)t.tm_mon + 1, 2); put_dec2(q + 6, (unsigned)t.tm_mday, 2); q[8] = '-';
    put_dec2(q + 9, (unsigned)t.tm_hour, 2); put_dec2(q + 11, (unsigned)t.tm_min, 2); put_dec2(q + 13, (unsigned)t.tm_sec, 2);
    q += 15;
    char* const stem_end = q;
    int fd = -1;
    for (int attempt = 1; attempt <= 9 && fd < 0; ++attempt) {   // two crashes in one second never overwrite each other
        q = stem_end;
        if (attempt > 1) { *q++ = '-'; *q++ = (char)('0' + attempt); }
        std::memcpy(q, ".txt", 5);
        fd = ::open(g_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0 && errno != EEXIST) break;
    }
    if (fd >= 0) {
        const char* p = g_out;
        size_t n = len;
        while (n > 0) { ssize_t w = ::write(fd, p, n); if (w <= 0) break; p += w; n -= (size_t)w; }
        ::fsync(fd);
        ::close(fd);
    }
}

void fatal_handler(int sig, siginfo_t* info, void* ctx) {
    if (!g_busy) {
        g_busy = 1;
        const char* kind = "Unhandled signal";
        uint64_t fa = (sig == SIGSEGV || sig == SIGBUS) && info ? (uint64_t)(uintptr_t)info->si_addr : 0;
        do_report(kind, 0x53000000u | (uint32_t)sig, info ? (uint64_t)(uintptr_t)info->si_addr : 0, fa, (ucontext_t*)ctx);
    }
    signal(sig, SIG_DFL);   // default action: macOS writes its own .ips report
    raise(sig);
}

void terminate_handler() {
    if (!g_busy) {
        g_busy = 1;
        do_report("std::terminate (uncaught C++ exception or abort)", 0x53000100u, 0, 0, nullptr);
    }
    signal(SIGABRT, SIG_DFL);
    abort();
}

bool is_report_name(const std::string& n) {
    /* crash-yyyymmdd-hhmmss.txt, or crash-yyyymmdd-hhmmss-N.txt when two reports share a second */
    if (n.size() < 6 + 15 + 4 || n.size() > 6 + 15 + 2 + 4 || n.compare(0, 6, "crash-") != 0 || n.compare(n.size() - 4, 4, ".txt") != 0) return false;
    for (size_t i = 6; i < n.size() - 4; ++i)
        if (!((n[i] >= '0' && n[i] <= '9') || n[i] == '-')) return false;
    return true;
}

}  // namespace

void install(const std::string& logs_dir_utf8, const std::string& version, const char* (*phase_fn)()) {
    try {
        static bool done = false;
        if (done) return;
        done = true;
        std::string d = logs_dir_utf8;
        if (d.empty() || d.back() != '/') d += '/';
        if (d.size() >= sizeof g_dir - 30) return;
        std::memcpy(g_dir, d.c_str(), d.size() + 1);
        std::error_code ec;
        std::filesystem::create_directories(logs_dir_utf8, ec);
        std::snprintf(g_version, sizeof g_version, "%s", version.c_str());
        g_phase_fn = phase_fn;
        load_modules();
        {   // keep the newest 10 reports
            std::vector<std::string> names = list_reports(logs_dir_utf8);
            for (size_t i = 10; i < names.size(); ++i) {
                std::error_code e2;
                std::filesystem::remove(std::filesystem::path(logs_dir_utf8) / names[i], e2);
            }
        }
        stack_t ss;
        ss.ss_sp = g_altstack;
        ss.ss_size = sizeof g_altstack;
        ss.ss_flags = 0;
        sigaltstack(&ss, nullptr);   // this thread only; other threads fall back to their own stack
        struct sigaction sa;
        std::memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = fatal_handler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT}) sigaction(sig, &sa, nullptr);
        std::set_terminate(terminate_handler);
    } catch (...) {}
}

bool reassert() { return false; }   // no vendor DLLs replace signal handlers on macOS
#endif  // _WIN32

/* ------------------------------------------------------------------ next start */
#if defined(_WIN32)
#define CRASH_SEEN_NAME L"crash-seen.txt"
#else
#define CRASH_SEEN_NAME "crash-seen.txt"
#endif
#if defined(_WIN32)
std::vector<std::string> list_reports(const std::string& logs_dir_utf8) {
    std::vector<std::string> out;
    try {
        std::error_code ec;
        for (std::filesystem::directory_iterator it(std::filesystem::u8path(logs_dir_utf8), ec), end; !ec && it != end; it.increment(ec)) {
            std::wstring n = it->path().filename().wstring();
            if (is_report_name(n)) out.push_back(narrow_w(n));
        }
    } catch (...) {}
    std::sort(out.begin(), out.end(), std::greater<std::string>());
    return out;
}
#else
std::vector<std::string> list_reports(const std::string& logs_dir_utf8) {
    std::vector<std::string> out;
    try {
        std::error_code ec;
        for (std::filesystem::directory_iterator it(std::filesystem::path(logs_dir_utf8), ec), end; !ec && it != end; it.increment(ec)) {
            std::string n = it->path().filename().string();
            if (is_report_name(n)) out.push_back(n);
        }
    } catch (...) {}
    std::sort(out.begin(), out.end(), std::greater<std::string>());
    return out;
}
#endif

static std::set<std::string> read_seen(const std::string& logs_dir) {
    std::set<std::string> s;
    try {
        std::ifstream in(std::filesystem::u8path(logs_dir) / CRASH_SEEN_NAME);
        std::string line;
        while (std::getline(in, line)) { while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back(); if (!line.empty()) s.insert(line); }
    } catch (...) {}
    return s;
}

bool find_unseen(const std::string& logs_dir_utf8, Found* out) {
    try {
        auto seen = read_seen(logs_dir_utf8);
        for (const std::string& n : list_reports(logs_dir_utf8)) {
            if (seen.count(n)) continue;
            if (out) {
                out->name = n;
                out->dir_utf8 = logs_dir_utf8;
                out->path_utf8 = (std::filesystem::u8path(logs_dir_utf8) / std::filesystem::u8path(n)).u8string();
            }
            return true;
        }
    } catch (...) {}
    return false;
}

void mark_all_seen(const std::string& logs_dir_utf8) {
    try {
        std::vector<std::string> existing = list_reports(logs_dir_utf8);
        std::ofstream o(std::filesystem::u8path(logs_dir_utf8) / CRASH_SEEN_NAME, std::ios::trunc);
        for (const std::string& n : existing) o << n << "\n";   // only names that still exist: the file never grows
    } catch (...) {}
}

std::string read_report(const std::string& path_utf8, size_t max_bytes) {
    try {
        std::ifstream in(std::filesystem::u8path(path_utf8), std::ios::binary);
        std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (s.size() > max_bytes) s.resize(max_bytes);
        return s;
    } catch (...) { return std::string(); }
}

}  // namespace crashrep
