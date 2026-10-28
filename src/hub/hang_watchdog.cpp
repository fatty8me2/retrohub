/* Hang watchdog: a background thread that notices when the UI thread stops ticking (~5 s) and writes the UI thread's stack
 * (userdata\logs\hang-<time>.txt, symbolized via dbghelp + the shipped RetroHub.pdb) plus a minidump (hang-<time>.dmp), once per
 * hang episode. Nothing is shown and nothing is killed: Windows still decides what to do with a hung window. */
#include "hang_watchdog.h"
#include <windows.h>
#include <dbghelp.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace {
std::atomic<unsigned long long> g_tick{0};
std::atomic<bool> g_stop{false};
std::thread g_th;
HANDLE g_ui = nullptr;
DWORD g_ui_id = 0;
std::wstring g_dir;
const char* volatile g_where = "";   // optional breadcrumb: the last phase the UI thread entered

std::wstring stamp() {
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t b[64];
    swprintf(b, 64, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

void write_stack(FILE* f) {
    HANDLE proc = GetCurrentProcess();
    if (SuspendThread(g_ui) == (DWORD)-1) { std::fprintf(f, "SuspendThread failed (%lu)\n", GetLastError()); return; }
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    STACKFRAME64 sf{};
    DWORD64 pcs[64]; int n = 0;
    if (GetThreadContext(g_ui, &ctx)) {
        sf.AddrPC.Offset = ctx.Rip; sf.AddrPC.Mode = AddrModeFlat;
        sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
        sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;
        // Walk while suspended (only addresses: no allocation, so a lock the UI thread holds cannot deadlock us)
        while (n < 64 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, g_ui, &sf, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
            if (!sf.AddrPC.Offset) break;
            pcs[n++] = sf.AddrPC.Offset;
        }
    }
    ResumeThread(g_ui);
    alignas(SYMBOL_INFO) char sbuf[sizeof(SYMBOL_INFO) + 512];
    for (int i = 0; i < n; ++i) {
        SYMBOL_INFO* s = (SYMBOL_INFO*)sbuf;
        std::memset(sbuf, 0, sizeof sbuf);
        s->SizeOfStruct = sizeof(SYMBOL_INFO); s->MaxNameLen = 511;
        DWORD64 disp = 0;
        char mod[MAX_PATH] = "?";
        HMODULE hm = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pcs[i], &hm))
            GetModuleFileNameA(hm, mod, MAX_PATH);
        const char* base = std::strrchr(mod, '\\'); base = base ? base + 1 : mod;
        IMAGEHLP_LINE64 ln{}; ln.SizeOfStruct = sizeof ln; DWORD ld = 0;
        if (SymFromAddr(proc, pcs[i], &disp, s)) {
            if (SymGetLineFromAddr64(proc, pcs[i], &ld, &ln))
                std::fprintf(f, "#%02d %s!%s+0x%llx  %s:%lu\n", i, base, s->Name, (unsigned long long)disp, ln.FileName, ln.LineNumber);
            else
                std::fprintf(f, "#%02d %s!%s+0x%llx\n", i, base, s->Name, (unsigned long long)disp);
        } else {
            std::fprintf(f, "#%02d %s+0x%llx\n", i, base, (unsigned long long)(pcs[i] - (DWORD64)hm));
        }
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
    if (h != INVALID_HANDLE_VALUE) {
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h,
                          (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory), nullptr, nullptr, nullptr);
        CloseHandle(h);
    }
}
}  // namespace

void hang_watchdog_tick() { g_tick.fetch_add(1, std::memory_order_relaxed); }
void hang_watchdog_phase(const char* where) { g_where = where; }

void hang_watchdog_start(const std::string& logs_dir_utf8, double threshold_s) {
    if (g_th.joinable()) return;
    int wn = MultiByteToWideChar(CP_UTF8, 0, logs_dir_utf8.c_str(), -1, nullptr, 0);
    g_dir.assign(wn > 0 ? wn - 1 : 0, L'\0');
    if (wn > 1) MultiByteToWideChar(CP_UTF8, 0, logs_dir_utf8.c_str(), -1, &g_dir[0], wn);
    if (!g_dir.empty() && g_dir.back() != L'\\') g_dir += L'\\';
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_ui,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    g_ui_id = GetCurrentThreadId();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);   // searches the exe's folder: RetroHub.pdb ships next to RetroHub.exe
    g_th = std::thread([threshold_s] {
        unsigned long long seen = g_tick.load();
        ULONGLONG since = GetTickCount64();
        bool reported = false;
        int reports = 0;
        while (!g_stop.load()) {
            Sleep(250);
            unsigned long long t = g_tick.load();
            ULONGLONG now = GetTickCount64();
            if (t != seen) { seen = t; since = now; reported = false; continue; }
            double secs = (now - since) / 1000.0;
            if (!reported && secs >= threshold_s && reports < 5) { reported = true; ++reports; report(secs); }
        }
    });
}

void hang_watchdog_stop() {
    g_stop = true;
    if (g_th.joinable()) g_th.join();
    if (g_ui) { CloseHandle(g_ui); g_ui = nullptr; }
}
