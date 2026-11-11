/* durable_write: commit_replace / write_file_durable. Temp dirs only. */
#include "durable_write.h"

#include <windows.h>

#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static std::wstring lpx(const std::wstring& p) { return p.size() >= 240 ? L"\\\\?\\" + p : p; }

static bool put(const std::wstring& p, const std::string& d) {
    HANDLE h = CreateFileW(lpx(p).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = WriteFile(h, d.data(), (DWORD)d.size(), &w, nullptr) && w == d.size();
    CloseHandle(h);
    return ok;
}
static bool get(const std::wstring& p, std::string* out) {
    HANDLE h = CreateFileW(lpx(p).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    out->clear();
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got) out->append(buf, got);
    CloseHandle(h);
    return true;
}
static bool exists(const std::wstring& p) { return GetFileAttributesW(lpx(p).c_str()) != INVALID_FILE_ATTRIBUTES; }

static void roundtrip(const std::wstring& dir) {
    const std::wstring dst = dir + L"\\save.dat", tmp = dst + L".hubtmp";
    std::string got;
    /* new file */
    CHECK(durable::write_file_durable(dst, std::string("first")));
    CHECK(get(dst, &got) && got == "first");
    CHECK(!exists(tmp));
    /* replace an existing file, content identical (including NULs) */
    std::string bin("a\0b\r\n\xff", 6);
    CHECK(durable::write_file_durable(dst, bin));
    CHECK(get(dst, &got) && got == bin);
    /* commit_replace of a caller-written temp */
    CHECK(put(tmp, "third"));
    CHECK(durable::commit_replace(tmp, dst));
    CHECK(get(dst, &got) && got == "third");
    CHECK(!exists(tmp));
    /* empty content is allowed */
    CHECK(durable::write_file_durable(dst, std::string()));
    CHECK(get(dst, &got) && got.empty());
    /* missing temp: false, destination untouched */
    CHECK(durable::write_file_durable(dst, std::string("keep")));
    CHECK(!durable::commit_replace(dir + L"\\nope.hubtmp", dst));
    CHECK(get(dst, &got) && got == "keep");
    /* read-only destination is replaced */
    SetFileAttributesW(lpx(dst).c_str(), FILE_ATTRIBUTE_READONLY);
    CHECK(durable::write_file_durable(dst, std::string("over-ro")));
    CHECK(get(dst, &got) && got == "over-ro");
}

int main() {
    wchar_t tp[MAX_PATH];
    GetTempPathW(MAX_PATH, tp);
    std::wstring root = std::wstring(tp) + L"durable_test_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(root.c_str(), nullptr);

    /* plain */
    roundtrip(root);

    /* non-ASCII */
    std::wstring uni = root + L"\\sp\u00e4ce \u65e5\u672c\u8a9e \U0001F3AE";
    CHECK(CreateDirectoryW(uni.c_str(), nullptr));
    roundtrip(uni);

    /* long (> 260) */
    std::wstring lng = root;
    for (int i = 0; i < 4; ++i) {
        lng += L"\\" + std::wstring(70, L'd');
        CHECK(CreateDirectoryW(lpx(lng).c_str(), nullptr));
    }
    CHECK(lng.size() + 12 > 260);
    roundtrip(lng);

    /* destination held open without FILE_SHARE_DELETE: commit fails, old content intact, temp left (named .hubtmp) or removed */
    {
        const std::wstring dst = root + L"\\held.dat";
        CHECK(put(dst, "old content"));
        HANDLE h = CreateFileW(dst.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        bool ok = durable::write_file_durable(dst, std::string("new content"));
        DWORD err = GetLastError();
        CHECK(!ok);
        CHECK(err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED);
        std::string got;
        CHECK(get(dst, &got) && got == "old content");
        CHECK(!exists(dst + L".hubtmp"));   /* write_file_durable cleans its temp */
        /* commit_replace leaves the caller's temp in place and reports the error */
        CHECK(put(dst + L".hubtmp", "new2"));
        CHECK(!durable::commit_replace(dst + L".hubtmp", dst));
        CHECK(GetLastError() == ERROR_SHARING_VIOLATION || GetLastError() == ERROR_ACCESS_DENIED);
        CHECK(get(dst, &got) && got == "old content");
        CHECK(exists(dst + L".hubtmp"));
        CloseHandle(h);
        /* once released, the same temp commits */
        CHECK(durable::commit_replace(dst + L".hubtmp", dst));
        CHECK(get(dst, &got) && got == "new2");
    }

    /* held open WITH FILE_SHARE_DELETE: must never corrupt */
    {
        const std::wstring dst = root + L"\\shared.dat";
        CHECK(put(dst, "old"));
        HANDLE h = CreateFileW(dst.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        /* MoveFileEx cannot replace a file that has any open handle, even a FILE_SHARE_DELETE one: fail and keep the old file, or succeed. */
        bool ok = durable::write_file_durable(dst, std::string("new"));
        CloseHandle(h);
        std::string got;
        CHECK(get(dst, &got) && got == (ok ? "new" : "old"));
        CHECK(!exists(dst + L".hubtmp"));
    }

    std::printf(g_fail ? "durable_write_test: %d FAILED\n" : "durable_write_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
