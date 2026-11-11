#include "durable_write.h"

#include <windows.h>

#include <algorithm>

namespace durable {

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

const int kAttempts = 6;
const DWORD kWaitMs = 50;

/* Flush the temp file's data to disk. Retries while another process holds it exclusively. */
bool flush_file(const std::wstring& path) {
    DWORD err = ERROR_SUCCESS;
    for (int i = 0; i < kAttempts; ++i) {
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
        if (!retryable(err) || i + 1 == kAttempts) break;
        Sleep(kWaitMs);
    }
    SetLastError(err);
    return false;
}

}  // namespace

bool commit_replace(const std::wstring& tmp, const std::wstring& dst) {
    const std::wstring ltmp = lp(tmp), ldst = lp(dst);
    if (!flush_file(ltmp)) return false;
    DWORD err = ERROR_SUCCESS;
    for (int i = 0; i < kAttempts; ++i) {
        if (MoveFileExW(ltmp.c_str(), ldst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        err = GetLastError();
        if (!retryable(err) || i + 1 == kAttempts) break;
        if (err == ERROR_ACCESS_DENIED) {
            /* A read-only destination cannot be replaced: clear the attribute (as the vault does), then try again. */
            DWORD a = GetFileAttributesW(ldst.c_str());
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) {
                SetFileAttributesW(ldst.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
                continue;
            }
        }
        Sleep(kWaitMs);
    }
    SetLastError(err);
    return false;
}

bool commit_replace_ansi(const std::string& tmp, const std::string& dst) {
    auto w = [](const std::string& s) {
        if (s.empty()) return std::wstring();
        int n = MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring r(n > 0 ? n : 0, L'\0');
        if (n > 0) MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), &r[0], n);
        return r;
    };
    return commit_replace(w(tmp), w(dst));
}

bool write_file_durable(const std::wstring& dst, const void* bytes, size_t len) {
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
    if (ok) ok = commit_replace(tmp, dst);
    else SetLastError(err);
    if (!ok) {
        err = GetLastError();
        DeleteFileW(ltmp.c_str());
        SetLastError(err);
    }
    return ok;
}

bool write_file_durable(const std::wstring& dst, const std::string& bytes) {
    return write_file_durable(dst, bytes.data(), bytes.size());
}

}  // namespace durable
