#include "durable_write.h"

#ifdef _WIN32
#include <windows.h>
#endif
#include "plat/plat.h"
#include "winutil.h"

#if !RH_WIN
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <system_error>

namespace durable {

#if RH_WIN

namespace {

/* Absolute path, with the \\?\ prefix when it is long (the same convention as the vault's lp()). */
std::wstring lp(const std::wstring& p) {
    if (p.compare(0, 4, L"\\\\?\\") == 0) return p;
    DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (!n) return p;
    std::wstring full(n, L'\0');
    DWORD m = GetFullPathNameW(p.c_str(), n, &full[0], nullptr);
    if (!m || m >= n) return p;
    full.resize(m);
    if (full.size() < 240) return full;
    if (full.compare(0, 2, L"\\\\") == 0) return L"\\\\?\\UNC\\" + full.substr(2);
    return L"\\\\?\\" + full;
}

bool retryable(DWORD e) { return e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED || e == ERROR_LOCK_VIOLATION; }

/* One atomic replace with POSIX rename semantics (Windows 10 1709+): succeeds even when another process holds `dst` open with
 * FILE_SHARE_DELETE, which MoveFileExW cannot do. Fails (leaving both files as they were) where unsupported. */
bool posix_replace(const std::wstring& from, const std::wstring& to) {
    struct RenameEx { DWORD Flags; HANDLE RootDirectory; DWORD FileNameLength; WCHAR FileName[1]; };   // FILE_RENAME_INFO (extended)
    const DWORD kReplaceIfExists = 0x1, kPosixSemantics = 0x2;
    const int kFileRenameInfoEx = 22;
    HANDLE h = CreateFileW(from.c_str(), DELETE | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const size_t bytes = to.size() * sizeof(WCHAR);
    std::string buf(sizeof(RenameEx) + bytes, '\0');
    RenameEx* ri = reinterpret_cast<RenameEx*>(&buf[0]);
    ri->Flags = kReplaceIfExists | kPosixSemantics;
    ri->RootDirectory = nullptr;
    ri->FileNameLength = (DWORD)bytes;
    memcpy(ri->FileName, to.c_str(), bytes);
    bool ok = SetFileInformationByHandle(h, (FILE_INFO_BY_HANDLE_CLASS)kFileRenameInfoEx, ri, (DWORD)buf.size()) != 0;
    DWORD e = ok ? 0 : GetLastError();
    CloseHandle(h);
    SetLastError(e);
    return ok;
}

const DWORD kWaitMs = 50;
int attempts_for(int budget_ms) { return std::max(1, budget_ms / (int)kWaitMs + 1); }

/* Flush the temp file's data to disk. Retries while another process holds it exclusively. A read-only temp (fs::copy_file carries a
 * read-only source attribute over) is made writable for the flush; the caller puts the attribute back on the final file. */
bool flush_file(const std::wstring& path, int attempts) {
    DWORD err = ERROR_SUCCESS;
    bool cleared = false;
    for (int i = 0; i < attempts; ++i) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            bool ok = FlushFileBuffers(h) != 0;
            err = ok ? ERROR_SUCCESS : GetLastError();
            CloseHandle(h);
            if (ok) return true;
            break;
        }
        err = GetLastError();
        if (err == ERROR_ACCESS_DENIED && !cleared) {
            DWORD a = GetFileAttributesW(path.c_str());
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) {
                cleared = true;
                SetFileAttributesW(path.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
                continue;   // retry the open at once, once
            }
        }
        if (!retryable(err) || i + 1 == attempts) break;
        Sleep(kWaitMs);
    }
    SetLastError(err);
    return false;
}

}  // namespace

bool commit_replace(const std::wstring& tmp, const std::wstring& dst, int budget_ms) {
    const std::wstring ltmp = lp(tmp), ldst = lp(dst);
    const int attempts = attempts_for(budget_ms);
    const DWORD tmp_attrs = GetFileAttributesW(ltmp.c_str());   // what the old code left on the final file (rename keeps them)
    if (!flush_file(ltmp, attempts)) return false;
    DWORD err = ERROR_SUCCESS;
    DWORD dst_cleared = INVALID_FILE_ATTRIBUTES;   // the destination's original attributes, when we had to clear read-only
    bool moved = false;
    for (int i = 0; i < attempts && !moved; ++i) {
        if (MoveFileExW(ltmp.c_str(), ldst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { moved = true; break; }
        err = GetLastError();
        if (!retryable(err)) break;
        /* MoveFileEx cannot replace a file another process holds open even with FILE_SHARE_DELETE; std::filesystem::rename
         * (POSIX rename semantics) can. Still one atomic replace, never remove-then-rename. */
        if (posix_replace(ltmp, ldst)) { moved = true; break; }
        {
            std::error_code ec;
            std::filesystem::rename(std::filesystem::path(ltmp), std::filesystem::path(ldst), ec);
            if (!ec) { moved = true; break; }
        }
        if (i + 1 == attempts) break;
        if (err == ERROR_ACCESS_DENIED && dst_cleared == INVALID_FILE_ATTRIBUTES) {
            /* A read-only destination cannot be replaced: clear the attribute (as the vault does), then try again. */
            DWORD a = GetFileAttributesW(ldst.c_str());
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) {
                dst_cleared = a;
                SetFileAttributesW(ldst.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
                continue;
            }
        }
        Sleep(kWaitMs);
    }
    if (moved) {
        if (tmp_attrs != INVALID_FILE_ATTRIBUTES && (tmp_attrs & FILE_ATTRIBUTE_READONLY))
            SetFileAttributesW(ldst.c_str(), tmp_attrs);   // a read-only source stays read-only after the copy, as before
        return true;
    }
    if (dst_cleared != INVALID_FILE_ATTRIBUTES) SetFileAttributesW(ldst.c_str(), dst_cleared);   // failed: leave the old file as it was
    SetLastError(err);
    return false;
}

bool commit_replace_ansi(const std::string& tmp, const std::string& dst, int budget_ms) {
    auto w = [](const std::string& s) {
        if (s.empty()) return std::wstring();
        int n = MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring r(n > 0 ? n : 0, L'\0');
        if (n > 0) MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), &r[0], n);
        return r;
    };
    return commit_replace(w(tmp), w(dst), budget_ms);
}

static std::wstring mb_to_wide(UINT cp, const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring r(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), &r[0], n);
    return r;
}
std::wstring utf8_to_wide(const std::string& utf8) { return mb_to_wide(CP_UTF8, utf8); }
std::wstring legacy_ansi_to_wide(const std::string& utf8) { return mb_to_wide(CP_ACP, utf8); }

bool commit_replace_utf8(const std::string& tmp, const std::string& dst, int budget_ms) {
    return commit_replace(utf8_to_wide(tmp), utf8_to_wide(dst), budget_ms);
}

static bool slurp_wide(const std::wstring& w, std::string* out) {
    std::ifstream f(std::filesystem::path(w), std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

/* Paths whose last read found an existing but unreadable file: writes to them are refused for the rest of the session, so a caller
 * running on in-memory defaults can never replace the real file. */
static std::mutex g_guard_mu;
static std::set<std::wstring>& guarded() { static std::set<std::wstring> s; return s; }
static void guard_set(const std::wstring& p, bool on) { std::lock_guard<std::mutex> l(g_guard_mu); if (on) guarded().insert(p); else guarded().erase(p); }
static bool guard_has(const std::wstring& p) { std::lock_guard<std::mutex> l(g_guard_mu); return guarded().count(p) != 0; }

static bool missing_err(DWORD e) { return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND || e == ERROR_INVALID_NAME; }

ReadResult read_file_utf8_migrating(const std::string& path, std::string* bytes) {
    bytes->clear();
    const std::wstring good = utf8_to_wide(path);
    const int attempts = attempts_for(kRetryShortMs);
    for (int i = 0; i < attempts; ++i) {
        if (slurp_wide(good, bytes)) { guard_set(good, false); return ReadResult::Primary; }
        bytes->clear();
        DWORD a = GetFileAttributesW(lp(good).c_str());
        if (a == INVALID_FILE_ATTRIBUTES && missing_err(GetLastError())) break;   // really absent: legacy fallback below
        if (i + 1 == attempts) { guard_set(good, true); return ReadResult::Unreadable; }   // exists but locked / denied: never fall back
        Sleep(kWaitMs);
    }
    guard_set(good, false);
    const std::wstring old = legacy_ansi_to_wide(path);
    if (old != good && !old.empty() && slurp_wide(old, bytes)) return ReadResult::Legacy;
    bytes->clear();
    return ReadResult::NotFound;
}

bool write_file_atomic_utf8(const std::string& path, const std::string& bytes, int budget_ms) {
    const std::string tmp = path + ".tmp";
    const std::wstring wtmp = utf8_to_wide(tmp);
    if (guard_has(utf8_to_wide(path))) { SetLastError(ERROR_ACCESS_DENIED); return false; }   // the real file was unreadable at load
    {
        std::ofstream f(std::filesystem::path(wtmp), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(bytes.data(), (std::streamsize)bytes.size());
        f.flush();
        if (!f.good()) { f.close(); std::error_code ec; std::filesystem::remove(std::filesystem::path(wtmp), ec); return false; }
    }
    if (!commit_replace(wtmp, utf8_to_wide(path), budget_ms)) {
        DWORD e = GetLastError();
        std::error_code ec; std::filesystem::remove(std::filesystem::path(wtmp), ec);
        SetLastError(e);
        return false;
    }
    return true;
}

bool write_file_durable(const std::wstring& dst, const void* bytes, size_t len, int budget_ms) {
    const std::wstring tmp = dst + L".hubtmp";
    const std::wstring ltmp = lp(tmp);
    HANDLE h = CreateFileW(ltmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const unsigned char* p = static_cast<const unsigned char*>(bytes);
    bool ok = true;
    while (len && ok) {
        DWORD chunk = (DWORD)std::min<size_t>(len, 1u << 24), w = 0;
        ok = WriteFile(h, p, chunk, &w, nullptr) && w == chunk;
        p += chunk;
        len -= chunk;
    }
    if (ok) ok = FlushFileBuffers(h) != 0;
    DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(h);
    if (ok) ok = commit_replace(tmp, dst, budget_ms);
    else SetLastError(err);
    if (!ok) {
        err = GetLastError();
        DeleteFileW(ltmp.c_str());
        SetLastError(err);
    }
    return ok;
}

bool write_file_durable(const std::wstring& dst, const std::string& bytes, int budget_ms) {
    return write_file_durable(dst, bytes.data(), bytes.size(), budget_ms);
}

#else  // macOS: paths are UTF-8 already; atomic replace = flush temp, rename(), flush the directory

namespace {

const int kWaitMs = 50;
int attempts_for(int budget_ms) { return std::max(1, budget_ms / kWaitMs + 1); }

/* Errors worth waiting out (a file briefly busy), as ERROR_SHARING_VIOLATION is on Windows. */
bool retryable(int e) { return e == EBUSY || e == ETXTBSY || e == EAGAIN || e == EINTR; }

std::string u8(const std::wstring& w) { return winutil::to_utf8(w); }

/* fsync() on macOS only hands the data to the drive; F_FULLFSYNC also flushes the drive's cache (the FlushFileBuffers equivalent).
 * Falls back to fsync() where the filesystem does not support it (some network / FAT volumes). */
int full_fsync(int fd) { return ::fcntl(fd, F_FULLFSYNC) == 0 ? 0 : ::fsync(fd); }

bool fsync_path(const std::string& path, int flags) {
    int fd = ::open(path.c_str(), flags);
    if (fd < 0) return false;
    bool ok = full_fsync(fd) == 0;
    int e = errno;
    ::close(fd);
    errno = e;
    return ok;
}

/* Flush the directory entry so the rename itself survives a power cut. Best effort: some filesystems refuse fsync on a directory. */
void fsync_dir_of(const std::string& path) {
    const size_t pos = path.find_last_of('/');
    const std::string dir = pos == std::string::npos ? std::string(".") : (pos == 0 ? std::string("/") : path.substr(0, pos));
    int fd = ::open(dir.c_str(), O_RDONLY);
    if (fd < 0) return;
    (void)::fsync(fd);
    ::close(fd);
}

bool commit_replace_u8(const std::string& tmp, const std::string& dst, int budget_ms) {
    const int attempts = attempts_for(budget_ms);
    int err = 0;
    bool flushed = false;
    for (int i = 0; i < attempts; ++i) {
        if (fsync_path(tmp, O_RDONLY)) { flushed = true; break; }
        err = errno;
        if (!retryable(err) || i + 1 == attempts) break;
        plat::sleep_ms(kWaitMs);
    }
    if (!flushed) { errno = err; return false; }
    for (int i = 0; i < attempts; ++i) {
        if (::rename(tmp.c_str(), dst.c_str()) == 0) { fsync_dir_of(dst); return true; }
        err = errno;
        if (!retryable(err) || i + 1 == attempts) break;
        plat::sleep_ms(kWaitMs);
    }
    errno = err;
    return false;
}

}  // namespace

bool commit_replace(const std::wstring& tmp, const std::wstring& dst, int budget_ms) {
    return commit_replace_u8(u8(tmp), u8(dst), budget_ms);
}

/* No ANSI code page on macOS: a narrow path is already UTF-8. */
bool commit_replace_ansi(const std::string& tmp, const std::string& dst, int budget_ms) {
    return commit_replace_u8(tmp, dst, budget_ms);
}

std::wstring utf8_to_wide(const std::string& utf8) { return winutil::to_wide(utf8); }
std::wstring legacy_ansi_to_wide(const std::string& utf8) { return winutil::to_wide(utf8); }

bool commit_replace_utf8(const std::string& tmp, const std::string& dst, int budget_ms) {
    return commit_replace_u8(tmp, dst, budget_ms);
}

static bool slurp_u8(const std::string& p, std::string* out) {
    std::ifstream f(std::filesystem::path(p), std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

/* Paths whose last read found an existing but unreadable file: writes to them are refused for the rest of the session. */
static std::mutex g_guard_mu;
static std::set<std::string>& guarded() { static std::set<std::string> s; return s; }
static void guard_set(const std::string& p, bool on) { std::lock_guard<std::mutex> l(g_guard_mu); if (on) guarded().insert(p); else guarded().erase(p); }
static bool guard_has(const std::string& p) { std::lock_guard<std::mutex> l(g_guard_mu); return guarded().count(p) != 0; }

/* Primary / NotFound / Unreadable only: there is no legacy (ANSI-misread) location on macOS. */
ReadResult read_file_utf8_migrating(const std::string& path, std::string* bytes) {
    bytes->clear();
    const int attempts = attempts_for(kRetryShortMs);
    for (int i = 0; i < attempts; ++i) {
        if (slurp_u8(path, bytes)) { guard_set(path, false); return ReadResult::Primary; }
        bytes->clear();
        struct stat st;
        if (::stat(path.c_str(), &st) != 0 && (errno == ENOENT || errno == ENOTDIR)) break;   // really absent
        if (i + 1 == attempts) { guard_set(path, true); return ReadResult::Unreadable; }   // exists but locked / denied
        plat::sleep_ms(kWaitMs);
    }
    guard_set(path, false);
    return ReadResult::NotFound;
}

bool write_file_atomic_utf8(const std::string& path, const std::string& bytes, int budget_ms) {
    const std::string tmp = path + ".tmp";
    if (guard_has(path)) { errno = EACCES; return false; }   // the real file was unreadable at load
    {
        std::ofstream f(std::filesystem::path(tmp), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(bytes.data(), (std::streamsize)bytes.size());
        f.flush();
        if (!f.good()) { f.close(); std::error_code ec; std::filesystem::remove(std::filesystem::path(tmp), ec); return false; }
    }
    if (!commit_replace_u8(tmp, path, budget_ms)) {
        int e = errno;
        std::error_code ec; std::filesystem::remove(std::filesystem::path(tmp), ec);
        errno = e;
        return false;
    }
    return true;
}

bool write_file_durable(const std::wstring& dst, const void* bytes, size_t len, int budget_ms) {
    const std::string d = u8(dst);
    const std::string tmp = d + ".hubtmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return false;
    const unsigned char* p = static_cast<const unsigned char*>(bytes);
    bool ok = true;
    while (len && ok) {
        ssize_t w = ::write(fd, p, len);
        if (w < 0) { if (errno == EINTR) continue; ok = false; break; }
        p += w;
        len -= (size_t)w;
    }
    if (ok) ok = full_fsync(fd) == 0;
    int err = ok ? 0 : errno;
    ::close(fd);
    if (ok) ok = commit_replace_u8(tmp, d, budget_ms);
    else errno = err;
    if (!ok) {
        err = errno;
        ::unlink(tmp.c_str());
        errno = err;
    }
    return ok;
}

bool write_file_durable(const std::wstring& dst, const std::string& bytes, int budget_ms) {
    return write_file_durable(dst, bytes.data(), bytes.size(), budget_ms);
}

#endif

}  // namespace durable
