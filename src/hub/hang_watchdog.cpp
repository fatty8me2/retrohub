/* Hang watchdog: a background thread that notices when the UI thread stops ticking (~5 s) and writes the UI thread's stack
 * (userdata\logs\hang-<time>.txt, symbolized via dbghelp + the shipped RetroHub.pdb) plus a minidump (hang-<time>.dmp), once per
 * hang episode. Nothing is shown and nothing is killed: Windows still decides what to do with a hung window.
 *
 * Safety: while the UI thread is suspended only its CONTEXT and a bounded raw copy of its stack are taken (into buffers allocated
 * up front: no heap, loader or dbghelp calls), then it is resumed at once; the walk and symbol lookup run on that copy.
 * Modal / blocking UI calls (file dialogs, ...) hold a HangWatchdogPause so they are not reported as hangs. */
#include "hang_watchdog.h"
#include <windows.h>
#include <dbghelp.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
std::atomic<unsigned long long> g_tick{0};
std::atomic<int> g_paused{0};
HANDLE g_th = nullptr;        // watchdog thread (a raw Win32 thread: nothing to std::terminate at exit if it is still running)
HANDLE g_stop_ev = nullptr;
HANDLE g_ui = nullptr;
DWORD g_ui_id = 0;
double g_threshold = 5.0;
std::wstring g_dir;
const char* volatile g_where = "";   // optional breadcrumb: the last phase the UI thread entered

constexpr size_t kStackCopy = 256 * 1024;
unsigned char* g_stack = nullptr;     // allocated in start(), filled while the UI thread is suspended
DWORD64 g_stack_lo = 0, g_stack_len = 0;

std::wstring stamp() {
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t b[64];
    swprintf(b, 64, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

/* StackWalk64 reads the stack from the snapshot (the live stack has moved on since the resume), everything else (code, unwind
 * data) from the process. */
BOOL CALLBACK read_mem(HANDLE proc, DWORD64 addr, PVOID buf, DWORD size, LPDWORD got) {
    if (addr >= g_stack_lo && addr + size <= g_stack_lo + g_stack_len) {
        std::memcpy(buf, g_stack + (addr - g_stack_lo), size);
        if (got) *got = size;
        return TRUE;
    }
    SIZE_T n = 0;
    BOOL ok = ReadProcessMemory(proc, (LPCVOID)addr, buf, size, &n);
    if (got) *got = (DWORD)n;
    return ok;
}

void write_stack(FILE* f) {
    HANDLE proc = GetCurrentProcess();
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    g_stack_lo = g_stack_len = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    if (SuspendThread(g_ui) == (DWORD)-1) { std::fprintf(f, "SuspendThread failed (%lu)\n", GetLastError()); return; }
    /* --- UI thread suspended: no allocation, no locks --- */
    BOOL have = GetThreadContext(g_ui, &ctx);
    if (have && VirtualQuery((LPCVOID)ctx.Rsp, &mbi, sizeof mbi)) {   // a syscall: takes no user-mode lock
        DWORD64 end = (DWORD64)mbi.BaseAddress + mbi.RegionSize;
        g_stack_lo = ctx.Rsp;
        g_stack_len = std::min<DWORD64>(kStackCopy, end - ctx.Rsp);
        std::memcpy(g_stack, (const void*)g_stack_lo, (size_t)g_stack_len);
    }
    ResumeThread(g_ui);
    /* --- resumed --- */
    if (!have) { std::fprintf(f, "GetThreadContext failed\n"); return; }
    STACKFRAME64 sf{};
    sf.AddrPC.Offset = ctx.Rip; sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;
    alignas(SYMBOL_INFO) char sbuf[sizeof(SYMBOL_INFO) + 512];
    for (int i = 0; i < 64; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, g_ui, &sf, &ctx, read_mem, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
        DWORD64 pc = sf.AddrPC.Offset;
        if (!pc) break;
        SYMBOL_INFO* s = (SYMBOL_INFO*)sbuf;
        std::memset(sbuf, 0, sizeof sbuf);
        s->SizeOfStruct = sizeof(SYMBOL_INFO); s->MaxNameLen = 511;
        DWORD64 disp = 0;
        char mod[MAX_PATH] = "?";
        HMODULE hm = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pc, &hm))
            GetModuleFileNameA(hm, mod, MAX_PATH);
        const char* base = std::strrchr(mod, '\\'); base = base ? base + 1 : mod;
        IMAGEHLP_LINE64 ln{}; ln.SizeOfStruct = sizeof ln; DWORD ld = 0;
        if (SymFromAddr(proc, pc, &disp, s)) {
            if (SymGetLineFromAddr64(proc, pc, &ld, &ln))
                std::fprintf(f, "#%02d %s!%s+0x%llx  %s:%lu\n", i, base, s->Name, (unsigned long long)disp, ln.FileName, ln.LineNumber);
            else
                std::fprintf(f, "#%02d %s!%s+0x%llx\n", i, base, s->Name, (unsigned long long)disp);
        } else {
            std::fprintf(f, "#%02d %s+0x%llx\n", i, base, (unsigned long long)(pc - (DWORD64)hm));
        }
    }
}

/* keep the newest `keep` reports (hang-<stamp>.txt/.dmp sort by name = by time) */
void prune(size_t keep) {
    for (const wchar_t* ext : {L"txt", L"dmp"}) {
        std::vector<std::wstring> names;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((g_dir + L"hang-*." + ext).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do names.push_back(fd.cFileName); while (FindNextFileW(h, &fd));
        FindClose(h);
        std::sort(names.begin(), names.end());
        for (size_t i = 0; i + keep < names.size(); ++i) DeleteFileW((g_dir + names[i]).c_str());
    }
}

void report(double secs) {
    std::wstring base = g_dir + L"hang-" + stamp();
    CreateDirectoryW(g_dir.c_str(), nullptr);
    FILE* f = _wfopen((base + L".txt").c_str(), L"w");
    if (f) {
        std::fprintf(f, "RetroHub UI thread has not ticked for %.1f s (tid %lu). Last phase: %s\n\n", secs, g_ui_id, g_where ? g_where : "");
        write_stack(f);
        std::fclose(f);
    }
    HANDLE h = CreateFileW((base + L".dmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {   // small dump: thread stacks + module list (MiniDumpWriteDump suspends the threads itself)
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h,
                          (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules), nullptr, nullptr, nullptr);
        CloseHandle(h);
    }
    prune(5);
}

DWORD WINAPI watch(void*) {
    unsigned long long seen = g_tick.load();
    ULONGLONG since = GetTickCount64();
    bool reported = false;
    int reports = 0;
    while (WaitForSingleObject(g_stop_ev, 250) == WAIT_TIMEOUT) {
        unsigned long long t = g_tick.load();
        ULONGLONG now = GetTickCount64();
        if (t != seen || g_paused.load() > 0) { seen = t; since = now; reported = false; continue; }
        double secs = (now - since) / 1000.0;
        if (!reported && secs >= g_threshold && reports < 5) { reported = true; ++reports; report(secs); }
    }
    return 0;
}
}  // namespace

void hang_watchdog_tick() { g_tick.fetch_add(1, std::memory_order_relaxed); }
void hang_watchdog_phase(const char* where) { g_where = where; }
void hang_watchdog_pause_push() { g_paused.fetch_add(1); }
void hang_watchdog_pause_pop() { g_paused.fetch_sub(1); hang_watchdog_tick(); }

void hang_watchdog_start(const std::string& logs_dir_utf8, double threshold_s) {
    if (g_th) return;
    g_threshold = threshold_s;
    int wn = MultiByteToWideChar(CP_UTF8, 0, logs_dir_utf8.c_str(), -1, nullptr, 0);
    g_dir.assign(wn > 0 ? wn - 1 : 0, L'\0');
    if (wn > 1) MultiByteToWideChar(CP_UTF8, 0, logs_dir_utf8.c_str(), -1, &g_dir[0], wn);
    if (!g_dir.empty() && g_dir.back() != L'\\') g_dir += L'\\';
    prune(5);
    g_stack = (unsigned char*)VirtualAlloc(nullptr, kStackCopy, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_stack) return;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_ui,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    g_ui_id = GetCurrentThreadId();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);   // searches the exe's folder: RetroHub.pdb ships next to RetroHub.exe
    g_stop_ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_th = CreateThread(nullptr, 0, watch, nullptr, 0, nullptr);
}

void hang_watchdog_stop() {
    if (!g_th) return;
    SetEvent(g_stop_ev);
    if (WaitForSingleObject(g_th, 3000) == WAIT_OBJECT_0) {   // a report in progress may take a moment; never block exit for long
        CloseHandle(g_th);
        CloseHandle(g_stop_ev); g_stop_ev = nullptr;
        if (g_ui) { CloseHandle(g_ui); g_ui = nullptr; }
    }   // else: left running (raw thread, ends with the process)
    g_th = nullptr;
}
