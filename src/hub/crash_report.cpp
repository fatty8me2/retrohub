#include "crash_report.h"
#include "hub_log.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2
#endif
#include <windows.h>
#include <psapi.h>

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
#include <crtdbg.h>

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
    const wchar_t* ext = L".txt";
    while (*ext) *q++ = *ext++;
    *q = 0;
    HANDLE h = CreateFileW(g_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
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

LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* ep) {
    request(ep, "Unhandled exception");
    return EXCEPTION_CONTINUE_SEARCH;   // Windows Error Reporting still runs; we never show a dialog
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
    return n.size() == 6 + 15 + 4 && n.compare(0, 6, L"crash-") == 0 && n.compare(n.size() - 4, 4, L".txt") == 0;
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
        std::set_terminate(terminate_handler);
        SetUnhandledExceptionFilter(unhandled_filter);
    } catch (...) {}
}

/* ------------------------------------------------------------------ next start */
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

static std::set<std::string> read_seen(const std::string& logs_dir) {
    std::set<std::string> s;
    try {
        std::ifstream in(std::filesystem::u8path(logs_dir) / L"crash-seen.txt");
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
        auto seen = read_seen(logs_dir_utf8);
        for (const std::string& n : list_reports(logs_dir_utf8)) seen.insert(n);
        std::vector<std::string> existing = list_reports(logs_dir_utf8);
        std::ofstream o(std::filesystem::u8path(logs_dir_utf8) / L"crash-seen.txt", std::ios::trunc);
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
