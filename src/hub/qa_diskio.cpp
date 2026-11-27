/* QA only: file-system calls of the UI thread while the game page draws (see qa_diskio.h). */
#include "qa_diskio.h"
#include "app.h"

#if RH_WIN
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>
#endif

#if RH_WIN
namespace qadisk {
namespace {

bool g_on = false, g_checked = false;
DWORD g_ui_tid = 0;
thread_local int t_quiet = 0;

/* UI thread only (the hooks count nothing on other threads) */
std::vector<const char*> g_scopes;
struct Frame { int page = 0, periodic = 0, ui = 0, workers = 0; bool page_drawn = false; std::vector<std::string> paths; } g_f;
struct Totals {
    int frames = 0, page_frames = 0;
    long long page = 0, periodic = 0, ui = 0, workers = 0;
    long long late_page = 0, late_periodic = 0, late_ui = 0, late_workers = 0;
    int late_frames = 0, late_page_frames = 0, late_frames_with_page_calls = 0, late_periodic_frames = 0;
    double t0 = -1.0, t_last = 0.0;
    double late_ms_max = 0.0, late_ms_sum = 0.0;   // how long the UI took to build a frame (frame_begin to frame_end)
} g_t;
double g_warm = 1.0;
int g_max_paths = 6;   // HUB_SHOT_DISKIO_PATHS: how many paths a "diskio" line lists
LARGE_INTEGER g_qpf{}, g_q0{};

std::string narrow(const wchar_t* w) {
    if (!w) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

bool device_name(const std::string& p) {   // pipes, devices, HID: not the disk
    return p.rfind("\\\\.\\", 0) == 0 || p.rfind("\\\\?\\hid", 0) == 0 || p.rfind("\\\\?\\HID", 0) == 0 || p == "CONOUT$" || p == "CONIN$" || p == "NUL";
}

void note(const char* api, const std::string& path) {
    if (!g_on || t_quiet || GetCurrentThreadId() != g_ui_tid || device_name(path)) return;
    ++g_f.ui;
    if (g_scopes.empty()) return;
    bool periodic = false;
    for (const char* s : g_scopes) periodic = periodic || std::strcmp(s, "periodic") == 0;
    if (periodic) ++g_f.periodic; else ++g_f.page;
    if ((int)g_f.paths.size() < g_max_paths) g_f.paths.push_back(std::string(g_scopes.back()) + ":" + api + ":" + path);
}

/* ---- the hooks: count, then call the real function ---- */
#define HK_DECL(ret, name, params) using name##_t = ret(WINAPI*) params; name##_t o_##name = nullptr;
HK_DECL(HANDLE, CreateFileW, (LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE))
HK_DECL(HANDLE, CreateFileA, (LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE))
HK_DECL(HANDLE, CreateFile2, (LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS))
HK_DECL(DWORD, GetFileAttributesW, (LPCWSTR))
HK_DECL(DWORD, GetFileAttributesA, (LPCSTR))
HK_DECL(BOOL, GetFileAttributesExW, (LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID))
HK_DECL(BOOL, GetFileAttributesExA, (LPCSTR, GET_FILEEX_INFO_LEVELS, LPVOID))
HK_DECL(HANDLE, FindFirstFileW, (LPCWSTR, LPWIN32_FIND_DATAW))
HK_DECL(HANDLE, FindFirstFileA, (LPCSTR, LPWIN32_FIND_DATAA))
HK_DECL(HANDLE, FindFirstFileExW, (LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD))
HK_DECL(HANDLE, FindFirstFileExA, (LPCSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD))
HK_DECL(BOOL, CreateDirectoryW, (LPCWSTR, LPSECURITY_ATTRIBUTES))
HK_DECL(BOOL, DeleteFileW, (LPCWSTR))
HK_DECL(BOOL, MoveFileExW, (LPCWSTR, LPCWSTR, DWORD))
#undef HK_DECL

HANDLE WINAPI h_CreateFileW(LPCWSTR n, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t) { note("CreateFile", narrow(n)); return o_CreateFileW(n, a, s, sa, c, f, t); }
HANDLE WINAPI h_CreateFileA(LPCSTR n, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t) { note("CreateFile", n ? n : ""); return o_CreateFileA(n, a, s, sa, c, f, t); }
HANDLE WINAPI h_CreateFile2(LPCWSTR n, DWORD a, DWORD s, DWORD c, LPCREATEFILE2_EXTENDED_PARAMETERS p) { note("CreateFile2", narrow(n)); return o_CreateFile2(n, a, s, c, p); }
DWORD WINAPI h_GetFileAttributesW(LPCWSTR n) { note("GetFileAttributes", narrow(n)); return o_GetFileAttributesW(n); }
DWORD WINAPI h_GetFileAttributesA(LPCSTR n) { note("GetFileAttributes", n ? n : ""); return o_GetFileAttributesA(n); }
BOOL WINAPI h_GetFileAttributesExW(LPCWSTR n, GET_FILEEX_INFO_LEVELS l, LPVOID i) { note("GetFileAttributesEx", narrow(n)); return o_GetFileAttributesExW(n, l, i); }
BOOL WINAPI h_GetFileAttributesExA(LPCSTR n, GET_FILEEX_INFO_LEVELS l, LPVOID i) { note("GetFileAttributesEx", n ? n : ""); return o_GetFileAttributesExA(n, l, i); }
HANDLE WINAPI h_FindFirstFileW(LPCWSTR n, LPWIN32_FIND_DATAW d) { note("FindFirstFile", narrow(n)); return o_FindFirstFileW(n, d); }
HANDLE WINAPI h_FindFirstFileA(LPCSTR n, LPWIN32_FIND_DATAA d) { note("FindFirstFile", n ? n : ""); return o_FindFirstFileA(n, d); }
HANDLE WINAPI h_FindFirstFileExW(LPCWSTR n, FINDEX_INFO_LEVELS l, LPVOID d, FINDEX_SEARCH_OPS o, LPVOID f, DWORD x) { note("FindFirstFileEx", narrow(n)); return o_FindFirstFileExW(n, l, d, o, f, x); }
HANDLE WINAPI h_FindFirstFileExA(LPCSTR n, FINDEX_INFO_LEVELS l, LPVOID d, FINDEX_SEARCH_OPS o, LPVOID f, DWORD x) { note("FindFirstFileEx", n ? n : ""); return o_FindFirstFileExA(n, l, d, o, f, x); }
BOOL WINAPI h_CreateDirectoryW(LPCWSTR n, LPSECURITY_ATTRIBUTES sa) { note("CreateDirectory", narrow(n)); return o_CreateDirectoryW(n, sa); }
BOOL WINAPI h_DeleteFileW(LPCWSTR n) { note("DeleteFile", narrow(n)); return o_DeleteFileW(n); }
BOOL WINAPI h_MoveFileExW(LPCWSTR a, LPCWSTR b, DWORD f) { note("MoveFileEx", narrow(a)); return o_MoveFileExW(a, b, f); }

struct Hook { const char* name; void** orig; void* hook; void* real[2]; };
std::vector<Hook> g_hooks;

void resolve() {
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll"), k32 = GetModuleHandleW(L"kernel32.dll");
    auto add = [&](const char* name, void** orig, void* hook) {
        Hook h{name, orig, hook, {nullptr, nullptr}};
        h.real[0] = kb ? (void*)GetProcAddress(kb, name) : nullptr;
        h.real[1] = k32 ? (void*)GetProcAddress(k32, name) : nullptr;
        *orig = h.real[0] ? h.real[0] : h.real[1];
        if (*orig) g_hooks.push_back(h);
    };
#define HK_ADD(name) add(#name, (void**)&o_##name, (void*)&h_##name)
    HK_ADD(CreateFileW); HK_ADD(CreateFileA); HK_ADD(CreateFile2);
    HK_ADD(GetFileAttributesW); HK_ADD(GetFileAttributesA); HK_ADD(GetFileAttributesExW); HK_ADD(GetFileAttributesExA);
    HK_ADD(FindFirstFileW); HK_ADD(FindFirstFileA); HK_ADD(FindFirstFileExW); HK_ADD(FindFirstFileExA);
    HK_ADD(CreateDirectoryW); HK_ADD(DeleteFileW); HK_ADD(MoveFileExW);
#undef HK_ADD
}

int patch_module(HMODULE m) {
    BYTE* base = (BYTE*)m;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return 0;
    int n = 0;
    for (const IMAGE_IMPORT_DESCRIPTOR* imp = (const IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (!imp->FirstThunk) continue;
        for (IMAGE_THUNK_DATA* th = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk); th->u1.Function; ++th) {
            void** slot = (void**)&th->u1.Function;
            for (const Hook& h : g_hooks) {
                if (*slot != h.real[0] && *slot != h.real[1]) continue;
                DWORD old = 0;
                if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
                    InterlockedExchangePointer(slot, h.hook);
                    VirtualProtect(slot, sizeof(void*), old, &old);
                    ++n;
                }
                break;
            }
        }
    }
    return n;
}

int patch_all() {
    int n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W me{};
    me.dwSize = sizeof me;
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        std::wstring nm = me.szModule;
        for (wchar_t& c : nm) c = (wchar_t)std::towlower(c);
        if (nm == L"ntdll.dll" || nm == L"kernel32.dll" || nm == L"kernelbase.dll") continue;
        n += patch_module(me.hModule);
    }
    CloseHandle(snap);
    return n;
}

}  // namespace

bool on() {
    if (!g_checked) {
        g_checked = true;
        const char* v = std::getenv("HUB_SHOT_DISKIO");
        g_on = g_headless && v && v[0] == '1';
        if (const char* w = std::getenv("HUB_SHOT_DISKIO_WARM")) g_warm = std::max(0.0, std::atof(w));
        if (const char* n = std::getenv("HUB_SHOT_DISKIO_PATHS")) g_max_paths = std::max(0, std::atoi(n));
    }
    return g_on;
}

void install() {
    if (!on()) return;
    g_ui_tid = GetCurrentThreadId();
    if (g_hooks.empty()) resolve();
    const int n = patch_all();
    Quiet q;
    shot_trace("diskio-install", "slots=" + std::to_string(n) + " hooks=" + std::to_string(g_hooks.size()));
}

void frame_begin() {
    if (!g_on) return;
    g_f = Frame();
    g_scopes.clear();
    if (ImGui::GetFrameCount() == 120) install();   // modules loaded since start-up (patched slots are not patched twice)
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_q0);
}

void frame_end() {
    if (!g_on) return;
    LARGE_INTEGER q1;
    QueryPerformanceCounter(&q1);
    const double ms = g_qpf.QuadPart ? (double)(q1.QuadPart - g_q0.QuadPart) * 1000.0 / (double)g_qpf.QuadPart : 0.0;
    const double t = ImGui::GetTime();
    ++g_t.frames;
    g_t.t_last = t;
    if (g_f.page_drawn) { ++g_t.page_frames; if (g_t.t0 < 0.0) g_t.t0 = t; }
    g_t.page += g_f.page; g_t.periodic += g_f.periodic; g_t.ui += g_f.ui; g_t.workers += g_f.workers;
    const bool late = g_t.t0 >= 0.0 && t >= g_t.t0 + g_warm;
    if (late) {
        ++g_t.late_frames;
        if (g_f.page_drawn) ++g_t.late_page_frames;
        if (g_f.page > 0) ++g_t.late_frames_with_page_calls;
        if (g_f.periodic > 0) ++g_t.late_periodic_frames;
        g_t.late_page += g_f.page; g_t.late_periodic += g_f.periodic; g_t.late_ui += g_f.ui; g_t.late_workers += g_f.workers;
        g_t.late_ms_max = std::max(g_t.late_ms_max, ms);
        g_t.late_ms_sum += ms;
    }
    if (g_f.page || g_f.periodic || g_f.workers) {
        char head[160];
        std::snprintf(head, sizeof head, "frame=%d t=%.2f page=%d periodic=%d ui=%d workers=%d ms=%.1f%s", ImGui::GetFrameCount(), t, g_f.page, g_f.periodic, g_f.ui, g_f.workers, ms, late ? " late" : "");
        std::string line = head;
        for (const std::string& p : g_f.paths) line += " | " + p;
        Quiet q;
        shot_trace("diskio", line);
    }
}

void finish() {
    if (!g_on) return;
    char line[640];
    std::snprintf(line, sizeof line,
                  "frames=%d page_frames=%d t0=%.2f warm=%.2f t_end=%.2f page=%lld periodic=%lld ui=%lld workers=%lld | late: frames=%d page_frames=%d "
                  "page=%lld frames_with_page_calls=%d periodic=%lld periodic_frames=%d ui=%lld workers=%lld ms_max=%.1f ms_avg=%.1f",
                  g_t.frames, g_t.page_frames, g_t.t0, g_warm, g_t.t_last, g_t.page, g_t.periodic, g_t.ui, g_t.workers, g_t.late_frames, g_t.late_page_frames,
                  g_t.late_page, g_t.late_frames_with_page_calls, g_t.late_periodic, g_t.late_periodic_frames, g_t.late_ui, g_t.late_workers,
                  g_t.late_ms_max, g_t.late_frames ? g_t.late_ms_sum / g_t.late_frames : 0.0);
    Quiet q;
    shot_trace("diskio-sum", line);
}

void note_worker(const char* what) {
    if (!g_on || GetCurrentThreadId() != g_ui_tid) return;
    ++g_f.workers;
    if ((int)g_f.paths.size() < g_max_paths) g_f.paths.push_back(std::string("worker:") + (what ? what : "?"));
}

Scope::Scope(const char* what, bool page_drawn) {
    if (!g_on || GetCurrentThreadId() != g_ui_tid) return;
    active = true;
    g_scopes.push_back(what ? what : "?");
    if (page_drawn && std::strcmp(g_scopes.back(), "periodic") != 0) g_f.page_drawn = true;
}
Scope::~Scope() {
    if (active && !g_scopes.empty()) g_scopes.pop_back();
}

Quiet::Quiet() { ++t_quiet; }
Quiet::~Quiet() { --t_quiet; }

}  // namespace qadisk
#else
/* macOS: the Windows build counts the UI thread's file-system calls by patching the import tables of the loaded modules. There is no equivalent
 * that is safe on macOS (dyld interposing would need a separate dylib), so HUB_SHOT_DISKIO is not supported: on() is always false and
 * every hook below is a no-op. */
// TODO(macOS lane 8): DYLD_INSERT_LIBRARIES interposer for open / stat / opendir / mkdir / unlink / rename if the desk-idle-IO check is needed on Mac.
namespace qadisk {
bool on() { return false; }
void install() {}
void frame_begin() {}
void frame_end() {}
void finish() {}
void note_worker(const char*) {}
Scope::Scope(const char*, bool) {}
Scope::~Scope() {}
Quiet::Quiet() {}
Quiet::~Quiet() {}
}  // namespace qadisk
#endif
