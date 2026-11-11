/* durable_write: commit_replace / write_file_durable. Temp dirs only. */
#include "durable_write.h"

#include <windows.h>

#include <cstdio>
#include <string>
#include <thread>

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

    /* destination held open without FILE_SHARE_DELETE: commit fails safely, old content intact, temp cleaned (write_file_durable) or left (commit_replace) */
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

    /* held open WITH FILE_SHARE_DELETE: replaced (POSIX rename), content new, temp gone */
    {
        const std::wstring dst = root + L"\\shared.dat";
        CHECK(put(dst, "old"));
        HANDLE h = CreateFileW(dst.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        CHECK(durable::write_file_durable(dst, std::string("new")));
        CloseHandle(h);
        std::string got;
        CHECK(get(dst, &got) && got == "new");
        CHECK(!exists(dst + L".hubtmp"));
    }

    /* a short-lived lock (no FILE_SHARE_DELETE) released after ~100 ms: the retry gets through */
    {
        const std::wstring dst = root + L"\\brief.dat";
        CHECK(put(dst, "old"));
        HANDLE h = CreateFileW(dst.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        std::thread t([h] { Sleep(100); CloseHandle(h); });
        bool ok = durable::write_file_durable(dst, std::string("after"));
        t.join();
        CHECK(ok);
        std::string got;
        CHECK(get(dst, &got) && got == "after");
        /* a lock that outlives the short budget fails, a long budget outlasts it */
        CHECK(put(dst, "old"));
        h = CreateFileW(dst.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        std::thread t2([h] { Sleep(700); CloseHandle(h); });
        CHECK(!durable::write_file_durable(dst, std::string("x"), durable::kRetryShortMs));
        CHECK(durable::write_file_durable(dst, std::string("y"), durable::kRetryLongMs));
        t2.join();
        CHECK(get(dst, &got) && got == "y");
    }

    /* read-only temp (as fs::copy_file leaves it): committed, and the final file stays read-only */
    {
        const std::wstring dst = root + L"\\ro.dat", tmp = dst + L".hubtmp";
        CHECK(put(dst, "old"));
        CHECK(put(tmp, "from read-only source"));
        SetFileAttributesW(tmp.c_str(), FILE_ATTRIBUTE_READONLY);
        CHECK(durable::commit_replace(tmp, dst));
        std::string got;
        CHECK(get(dst, &got) && got == "from read-only source");
        DWORD a = GetFileAttributesW(dst.c_str());
        CHECK(a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY));
        CHECK(!exists(tmp));
        /* and a read-only temp over a read-only destination */
        CHECK(put(tmp + L"2", "again"));
        SetFileAttributesW((tmp + L"2").c_str(), FILE_ATTRIBUTE_READONLY);
        CHECK(durable::commit_replace(tmp + L"2", dst));
        CHECK(get(dst, &got) && got == "again");
        SetFileAttributesW(dst.c_str(), FILE_ATTRIBUTE_NORMAL);
    }

    /* the committed file keeps the temp's modification time */
    {
        const std::wstring dst = root + L"\\mt.dat", tmp = dst + L".hubtmp";
        CHECK(put(dst, "old"));
        CHECK(put(tmp, "stamped"));
        HANDLE h = CreateFileW(tmp.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        FILETIME ft; ft.dwLowDateTime = 0x12345678; ft.dwHighDateTime = 0x01D00000;   // a fixed past time
        CHECK(h != INVALID_HANDLE_VALUE && SetFileTime(h, nullptr, nullptr, &ft));
        CloseHandle(h);
        CHECK(durable::commit_replace(tmp, dst));
        WIN32_FILE_ATTRIBUTE_DATA d;
        CHECK(GetFileAttributesExW(dst.c_str(), GetFileExInfoStandard, &d));
        CHECK(d.ftLastWriteTime.dwLowDateTime == ft.dwLowDateTime && d.ftLastWriteTime.dwHighDateTime == ft.dwHighDateTime);
    }

    /* ANSI (narrow) paths */
    /* Needs a temp folder the ANSI code page can represent AND whose narrow bytes widen back to the real wide path (byte-wise
     * widening below). With TEMP inside a non-ANSI folder the narrow path names another folder: SKIP, never fail or crash. */
    bool ansi_ok = false;
    {
        char tpa[MAX_PATH];
        DWORD n = GetTempPathA(MAX_PATH, tpa);
        ansi_ok = n > 0 && n < MAX_PATH;
        for (DWORD i = 0; ansi_ok && i < n; ++i) if ((unsigned char)tpa[i] >= 0x80 || tpa[i] == '?') ansi_ok = false;
        if (ansi_ok) { std::string a(tpa); ansi_ok = std::wstring(a.begin(), a.end()) == std::wstring(tp); }
    }
    if (!ansi_ok) std::printf("SKIP: ANSI (narrow) path checks: the temp folder is not representable in the ANSI code page\n");
    else {
        char tpa[MAX_PATH];
        GetTempPathA(MAX_PATH, tpa);
        std::string dir = std::string(tpa) + "durable_test_" + std::to_string(GetCurrentProcessId());
        std::string dst = dir + "\\ansi.dat", tmp = dst + ".tmp";
        CHECK(put(std::wstring(dst.begin(), dst.end()), "old"));
        CHECK(put(std::wstring(tmp.begin(), tmp.end()), "ansi new"));
        CHECK(durable::commit_replace_ansi(tmp, dst));
        std::string got;
        CHECK(get(std::wstring(dst.begin(), dst.end()), &got) && got == "ansi new");
        CHECK(!exists(std::wstring(tmp.begin(), tmp.end())));
    }

    /* a relative path */
    {
        wchar_t cwd[MAX_PATH];
        GetCurrentDirectoryW(MAX_PATH, cwd);
        CHECK(SetCurrentDirectoryW(root.c_str()));
        CHECK(put(L"rel.dat", "old"));
        CHECK(durable::write_file_durable(L"rel.dat", std::string("relative")));
        std::string got;
        CHECK(get(root + L"\\rel.dat", &got) && got == "relative");
        CHECK(put(L"rel.dat.tmp", "second"));
        CHECK(durable::commit_replace(L"rel.dat.tmp", L"rel.dat"));
        CHECK(get(root + L"\\rel.dat", &got) && got == "second");
        SetCurrentDirectoryW(cwd);
    }

    std::printf(g_fail ? "durable_write_test: %d FAILED\n" : "durable_write_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
