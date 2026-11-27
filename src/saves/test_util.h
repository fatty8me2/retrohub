#pragma once
/* Small portability helpers shared by the saves console tests (Windows and macOS). */
#include "plat/plat.h"
#include "winutil.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#if RH_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <fcntl.h>
#endif

namespace tu {

/* A path literal written with '\\' separators, converted to the platform's separator ("a\\b" stays as is on Windows). */
inline std::string N(std::string s) {
    for (auto& c : s) if (c == '\\') c = plat::kSep;
    return s;
}

inline unsigned long pid() { return (unsigned long)plat::current_pid(); }

/* Sets a file's last-write time to (now - secs_ago) seconds. */
inline void set_mtime_ago(const std::filesystem::path& p, int secs_ago) {
#if RH_WIN
    HANDLE h = CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    u.QuadPart -= (ULONGLONG)secs_ago * 10000000ULL;
    ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
#else
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    struct timespec ts[2];
    ts[0].tv_sec = 0; ts[0].tv_nsec = UTIME_OMIT;
    ts[1].tv_sec = now.tv_sec - secs_ago; ts[1].tv_nsec = now.tv_nsec;
    utimensat(AT_FDCWD, p.c_str(), ts, 0);
#endif
}

/* The extended-length (\\?\) spelling of an absolute path on Windows (long-path tests); the path itself elsewhere. */
inline std::filesystem::path longpath(const std::filesystem::path& p) {
#if RH_WIN
    return std::filesystem::path(L"\\\\?\\" + std::filesystem::absolute(p).wstring());
#else
    return std::filesystem::absolute(p);
#endif
}

/* path -> std::wstring (UTF-16 on Windows, UTF-32 elsewhere), whatever the platform's native string is. */
inline std::wstring wstr(const std::filesystem::path& p) {
#if RH_WIN
    return p.wstring();
#else
    return winutil::to_wide(p.string());
#endif
}

/* Makes a file unreadable for the lifetime of the object: Windows opens it without read sharing (another program "holds" it);
 * macOS has no mandatory locking, so the read permission is removed (open() fails the same way for the vault scan). */
class Unreadable {
public:
    explicit Unreadable(const std::filesystem::path& p) : p_(p) {
#if RH_WIN
        h_ = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        ok_ = h_ != INVALID_HANDLE_VALUE;
#else
        ok_ = ::chmod(p.c_str(), 0) == 0;
#endif
    }
    bool ok() const { return ok_; }
    void release() {
        if (!ok_) return;
        ok_ = false;
#if RH_WIN
        CloseHandle(h_);
#else
        ::chmod(p_.c_str(), 0644);
#endif
    }
    ~Unreadable() { release(); }
private:
    std::filesystem::path p_;
    bool ok_ = false;
#if RH_WIN
    HANDLE h_ = INVALID_HANDLE_VALUE;
#endif
};

}  // namespace tu
