/* Hang watchdog: a background thread that notices when the UI thread stops ticking (~5 s) and writes a report
 * (userdata\logs\hang-<time>.txt, newest 5 kept), once per hang episode. Nothing is shown and nothing is killed: Windows still
 * decides what to do with a hung window.
 *
 * The report is cooperative: the UI thread leaves breadcrumbs (hang_watchdog_phase / HangPhase) as it enters each major section
 * of the frame, and the report names the section it is stuck in, how long it has been there, the page shown and the last few
 * sections before it. The watchdog never suspends, inspects or dumps another thread (1.29.3-1.29.5 did that with SuspendThread,
 * GetThreadContext, a raw stack copy, dbghelp and MiniDumpWriteDump, and Windows Defender's ML flagged the exe as a trojan).
 * Modal / blocking UI calls (file dialogs, ...) hold a HangWatchdogPause so they are not reported as hangs. */
#include "hang_watchdog.h"
#include "plat/plat.h"
#if RH_WIN
#include <windows.h>
#else
#include <chrono>
#include <condition_variable>
#include <pthread.h>
#include <mutex>
#include <thread>
#endif
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

namespace {
std::atomic<unsigned long long> g_tick{0};
std::atomic<int> g_paused{0};
#if RH_WIN
HANDLE g_th = nullptr;        // watchdog thread (a raw Win32 thread: nothing to std::terminate at exit if it is still running)
HANDLE g_stop_ev = nullptr;
DWORD g_ui_id = 0;
double g_threshold = 5.0;
std::wstring g_dir;
#else
/* macOS: a detached std::thread (nothing to std::terminate at exit if it is still running); its state is leaked on purpose so a late
 * report at process exit never touches a destroyed global. */
struct MacWd {
    std::mutex mu;
    std::condition_variable cv;
    bool stop = false;
    std::atomic<bool> done{false};
    bool started = false;
    std::string dir;
    double threshold = 5.0;
    unsigned long long ui_id = 0;
};
MacWd& wd() { static MacWd* w = new MacWd; return *w; }
#endif

/* breadcrumbs: string literals only (never freed), written by the UI thread, read by the watchdog */
std::atomic<const char*> g_where{""};
std::atomic<unsigned long long> g_where_since{0};   // GetTickCount64 when g_where was entered
std::atomic<int> g_detail{-1};                       // e.g. the page index
constexpr int kTrail = 16;
std::atomic<const char*> g_trail[kTrail];            // ring of the last phases entered (de-duplicated)
std::atomic<unsigned> g_trail_n{0};

#if RH_WIN
std::wstring stamp() {
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t b[64];
    swprintf(b, 64, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

/* keep the newest `keep` reports (hang-<stamp>.txt sort by name = by time); old .dmp files from 1.29.3-1.29.5 are removed */
void prune(size_t keep) {
    for (const wchar_t* ext : {L"txt", L"dmp"}) {
        std::vector<std::wstring> names;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((g_dir + L"hang-*." + ext).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do names.push_back(fd.cFileName); while (FindNextFileW(h, &fd));
        FindClose(h);
        std::sort(names.begin(), names.end());
        size_t k = ext[0] == L'd' ? 0 : keep;
        for (size_t i = 0; i + k < names.size(); ++i) DeleteFileW((g_dir + names[i]).c_str());
    }
}

void report(double secs) {
    std::wstring base = g_dir + L"hang-" + stamp();
    CreateDirectoryW(g_dir.c_str(), nullptr);
    FILE* f = _wfopen((base + L".txt").c_str(), L"w");
    if (f) {
        const char* where = g_where.load();
        double in_phase = (GetTickCount64() - g_where_since.load()) / 1000.0;
        std::fprintf(f, "RetroHub UI thread has not ticked for %.1f s (tid %lu).\n", secs, g_ui_id);
        std::fprintf(f, "Stuck in: %s (for %.1f s)\n", where ? where : "", in_phase);
        if (int d = g_detail.load(); d >= 0) std::fprintf(f, "Page: %d\n", d);
        std::fprintf(f, "\nRecent phases (oldest first):\n");
        unsigned n = g_trail_n.load();
        for (unsigned i = n > kTrail ? n - kTrail : 0; i < n; ++i)
            if (const char* p = g_trail[i % kTrail].load()) std::fprintf(f, "  %s\n", p);
        std::fclose(f);
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
#else
std::string stamp() {
    std::tm t{};
    plat::local_tm(std::time(nullptr), &t);
    char b[64];
    std::snprintf(b, sizeof b, "%04d%02d%02d-%02d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    return b;
}

/* keep the newest `keep` reports (hang-<stamp>.txt sort by name = by time); old .dmp files from 1.29.3-1.29.5 are removed */
void prune(size_t keep) {
    namespace fs = std::filesystem;
    for (const char* ext : {"txt", "dmp"}) {
        std::vector<std::string> names;
        std::error_code ec;
        for (fs::directory_iterator it(fs::path(wd().dir), ec), end; !ec && it != end; it.increment(ec)) {
            const std::string n = it->path().filename().string();
            const std::string suffix = std::string(".") + ext;
            if (n.rfind("hang-", 0) == 0 && n.size() >= suffix.size() && n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0) names.push_back(n);
        }
        std::sort(names.begin(), names.end());
        size_t k = ext[0] == 'd' ? 0 : keep;
        for (size_t i = 0; i + k < names.size(); ++i) fs::remove(fs::path(wd().dir) / names[i], ec);
    }
}

void report(double secs) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(wd().dir), ec);
    FILE* f = plat::fopen_path(fs::path(wd().dir) / ("hang-" + stamp() + ".txt"), "w");
    if (f) {
        const char* where = g_where.load();
        double in_phase = (double)(plat::tick_ms() - g_where_since.load()) / 1000.0;
        std::fprintf(f, "RetroHub UI thread has not ticked for %.1f s (thread %llu).\n", secs, wd().ui_id);
        std::fprintf(f, "Stuck in: %s (for %.1f s)\n", where ? where : "", in_phase);
        if (int d = g_detail.load(); d >= 0) std::fprintf(f, "Page: %d\n", d);
        std::fprintf(f, "\nRecent phases (oldest first):\n");
        unsigned n = g_trail_n.load();
        for (unsigned i = n > kTrail ? n - kTrail : 0; i < n; ++i)
            if (const char* p = g_trail[i % kTrail].load()) std::fprintf(f, "  %s\n", p);
        std::fclose(f);
    }
    prune(5);
}

void watch() {
    MacWd& w = wd();
    unsigned long long seen = g_tick.load();
    unsigned long long since = plat::tick_ms();
    bool reported = false;
    int reports = 0;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(w.mu);
            if (w.cv.wait_for(lk, std::chrono::milliseconds(250), [&] { return w.stop; })) break;
        }
        unsigned long long t = g_tick.load();
        unsigned long long now = plat::tick_ms();
        if (t != seen || g_paused.load() > 0) { seen = t; since = now; reported = false; continue; }
        double secs = (double)(now - since) / 1000.0;
        if (!reported && secs >= w.threshold && reports < 5) { reported = true; ++reports; report(secs); }
    }
    w.done = true;
}
#endif
}  // namespace

void hang_watchdog_tick() { g_tick.fetch_add(1, std::memory_order_relaxed); }
void hang_watchdog_phase(const char* where) {
    if (!where) where = "";
    g_where_since.store(plat::tick_ms(), std::memory_order_relaxed);
    if (g_where.exchange(where, std::memory_order_relaxed) == where) return;
    unsigned n = g_trail_n.load(std::memory_order_relaxed);
    g_trail[n % kTrail].store(where, std::memory_order_relaxed);
    g_trail_n.store(n + 1, std::memory_order_release);
}
const char* hang_watchdog_current_phase() { return g_where.load(); }
void hang_watchdog_detail(int d) { g_detail.store(d, std::memory_order_relaxed); }
void hang_watchdog_pause_push() { g_paused.fetch_add(1); }
void hang_watchdog_pause_pop() { g_paused.fetch_sub(1); hang_watchdog_tick(); }

#if RH_WIN
void hang_watchdog_start(const std::string& logs_dir_utf8, double threshold_s) {
    if (g_th) return;
    g_threshold = threshold_s;
    int wn = MultiByteToWideChar(CP_UTF8, 0, logs_dir_utf8.c_str(), -1, nullptr, 0);
    g_dir.assign(wn > 0 ? wn - 1 : 0, L'\0');
    if (wn > 1) MultiByteToWideChar(CP_UTF8, 0, logs_dir_utf8.c_str(), -1, &g_dir[0], wn);
    if (!g_dir.empty() && g_dir.back() != L'\\') g_dir += L'\\';
    prune(5);
    g_ui_id = GetCurrentThreadId();
    g_stop_ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_th = CreateThread(nullptr, 0, watch, nullptr, 0, nullptr);
}

void hang_watchdog_stop() {
    if (!g_th) return;
    SetEvent(g_stop_ev);
    if (WaitForSingleObject(g_th, 3000) == WAIT_OBJECT_0) {   // a report in progress may take a moment; never block exit for long
        CloseHandle(g_th);
        CloseHandle(g_stop_ev); g_stop_ev = nullptr;
    }   // else: left running (raw thread, ends with the process)
    g_th = nullptr;
}
#else
void hang_watchdog_start(const std::string& logs_dir_utf8, double threshold_s) {
    MacWd& w = wd();
    if (w.started) return;
    w.started = true;
    w.threshold = threshold_s;
    w.dir = logs_dir_utf8;
    if (!w.dir.empty() && w.dir.back() != '/') w.dir += '/';
    prune(5);
    w.ui_id = (unsigned long long)(uintptr_t)pthread_self();
    std::thread(watch).detach();
}

void hang_watchdog_stop() {
    MacWd& w = wd();
    if (!w.started) return;
    { std::lock_guard<std::mutex> lk(w.mu); w.stop = true; }
    w.cv.notify_all();
    for (int i = 0; i < 300 && !w.done.load(); ++i) plat::sleep_ms(10);   // a report in progress may take a moment; never block exit for long
    w.started = false;
}
#endif
