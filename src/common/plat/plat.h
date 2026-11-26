#pragma once
/* Platform basics shared by every module (docs/MACOS.md). Windows keeps its existing behaviour exactly; macOS (arm64) gets the
 * POSIX equivalent. Header-only, no link dependency.
 *
 * Rules for ported code:
 *  - Paths are UTF-8 std::string on every platform. Build them with std::filesystem or plat::join / plat::kSep, never with a
 *    hard-coded '\\' (Windows output must stay byte-identical: vault layout, game keys and save names).
 *  - Code that only makes sense on Windows stays under `#if RH_WIN`; give the macOS side a real implementation or an explicit
 *    "not supported on this platform" result, never a silent stub that pretends to succeed. */

#if defined(_WIN32)
#define RH_WIN 1
#define RH_MAC 0
#elif defined(__APPLE__)
#define RH_WIN 0
#define RH_MAC 1
#else
#error "RetroHub builds on Windows and macOS only"
#endif

#include <chrono>
#include <cstdint>
#include <string>

#if RH_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace plat {

#if RH_WIN
constexpr char kSep = '\\';
constexpr const char* kExeSuffix = ".exe";
constexpr const char* kLibSuffix = ".dll";
#else
constexpr char kSep = '/';
constexpr const char* kExeSuffix = "";
constexpr const char* kLibSuffix = ".dylib";   // libretro cores on macOS use .dylib
#endif

/* a + separator + b; no separator is added when a is empty or already ends in one ('/' counts on Windows too). */
inline std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    const char last = a.back();
    if (last == '/' || last == kSep) return a + b;
    return a + kSep + b;
}

/* Monotonic clock (QueryPerformanceCounter on Windows, mach_absolute_time via steady_clock on macOS). */
inline uint64_t now_us() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline uint64_t now_ms() { return now_us() / 1000; }

inline void sleep_ms(unsigned ms) {
#if RH_WIN
    Sleep(ms);
#else
    usleep((useconds_t)ms * 1000);
#endif
}

inline uint32_t current_pid() {
#if RH_WIN
    return (uint32_t)GetCurrentProcessId();
#else
    return (uint32_t)getpid();
#endif
}

/* Dynamic libraries (libretro cores, mupen64plus plugins). Path is UTF-8. */
using LibHandle = void*;
inline LibHandle lib_open(const std::string& path_utf8) {
#if RH_WIN
    const int n = MultiByteToWideChar(CP_UTF8, 0, path_utf8.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? (size_t)n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, path_utf8.c_str(), -1, &w[0], n);
    return (LibHandle)LoadLibraryW(w.c_str());
#else
    return dlopen(path_utf8.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}
inline void* lib_sym(LibHandle h, const char* name) {
#if RH_WIN
    return h ? (void*)GetProcAddress((HMODULE)h, name) : nullptr;
#else
    return h ? dlsym(h, name) : nullptr;
#endif
}
inline void lib_close(LibHandle h) {
    if (!h) return;
#if RH_WIN
    FreeLibrary((HMODULE)h);
#else
    dlclose(h);
#endif
}

} // namespace plat
