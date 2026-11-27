/* durable_write: commit_replace / write_file_durable. Temp dirs only. */
#include "durable_write.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdio>
#include <string>
#include <thread>

#ifndef _WIN32
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iterator>
#include <sys/stat.h>
#include <unistd.h>

#include "plat/plat.h"
#include "winutil.h"
#endif

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

#ifdef _WIN32
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
#else  // macOS: same contracts on POSIX (atomic rename, fsync, UTF-8 paths, no legacy ANSI location)

namespace fs = std::filesystem;

static std::string plat_join(const std::string& a, const std::string& b) { return plat::join(a, b); }
static std::wstring W(const std::string& u8) { return durable::utf8_to_wide(u8); }
static bool put(const std::string& p, const std::string& d) {
    std::ofstream f(fs::path(p), std::ios::binary | std::ios::trunc);
    f.write(d.data(), (std::streamsize)d.size());
    return (bool)f;
}
static bool get(const std::string& p, std::string* out) {
    std::ifstream f(fs::path(p), std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}
static bool exists(const std::string& p) { std::error_code ec; return fs::exists(fs::path(p), ec); }

static void roundtrip(const std::string& dir) {
    const std::string dst = plat_join(dir, "save.dat"), tmp = dst + ".hubtmp";
    std::string got;
    CHECK(durable::write_file_durable(W(dst), std::string("first")));
    CHECK(get(dst, &got) && got == "first");
    CHECK(!exists(tmp));
    std::string bin("a\0b\r\n\xff", 6);
    CHECK(durable::write_file_durable(W(dst), bin));
    CHECK(get(dst, &got) && got == bin);
    CHECK(put(tmp, "third"));
    CHECK(durable::commit_replace(W(tmp), W(dst)));
    CHECK(get(dst, &got) && got == "third");
    CHECK(!exists(tmp));
    CHECK(durable::write_file_durable(W(dst), std::string()));
    CHECK(get(dst, &got) && got.empty());
    CHECK(durable::write_file_durable(W(dst), std::string("keep")));
    CHECK(!durable::commit_replace(W(plat_join(dir, "nope.hubtmp")), W(dst)));
    CHECK(get(dst, &got) && got == "keep");
    CHECK(durable::commit_replace_utf8(tmp, dst) == false);   // tmp does not exist any more
    CHECK(put(tmp, "via utf8"));
    CHECK(durable::commit_replace_utf8(tmp, dst) && get(dst, &got) && got == "via utf8");
    CHECK(put(tmp, "via ansi"));
    CHECK(durable::commit_replace_ansi(tmp, dst) && get(dst, &got) && got == "via ansi");
    /* read-only destination file: rename replaces the directory entry, as the Windows build does */
    chmod(dst.c_str(), 0444);
    CHECK(durable::write_file_durable(W(dst), std::string("over-ro")));
    CHECK(get(dst, &got) && got == "over-ro");
}

int main() {
    const std::string root = (fs::temp_directory_path() / ("durable_test_" + std::to_string(plat::current_pid()))).string();
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    roundtrip(root);

    const std::string uni = plat_join(root, "sp\xC3\xA4" "ce \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E \xF0\x9F\x8E\xAE");
    CHECK(fs::create_directories(fs::u8path(uni), ec));
    roundtrip(uni);

    std::string lng = root;
    for (int i = 0; i < 4; ++i) {
        lng = plat_join(lng, std::string(70, 'd'));
        CHECK(fs::create_directories(fs::path(lng), ec) || fs::exists(fs::path(lng)));
    }
    CHECK(lng.size() + 12 > 260);
    roundtrip(lng);

    /* wide <-> UTF-8 helpers: no ANSI code page, so the legacy path is the same path */
    {
        const std::string u = "caf\xC3\xA9 \xE6\x97\xA5";
        CHECK(durable::utf8_to_wide(u) == L"café 日");
        CHECK(durable::legacy_ansi_to_wide(u) == durable::utf8_to_wide(u));
    }

    /* read_file_utf8_migrating: Primary / NotFound / Unreadable, never Legacy */
    {
        const std::string p = plat_join(root, "mig.dat");
        std::string bytes;
        CHECK(durable::read_file_utf8_migrating(p, &bytes) == durable::ReadResult::NotFound && bytes.empty());
        CHECK(durable::write_file_atomic_utf8(p, "hello"));
        CHECK(!exists(p + ".tmp"));
        CHECK(durable::read_file_utf8_migrating(p, &bytes) == durable::ReadResult::Primary && bytes == "hello");
        CHECK(durable::write_file_atomic_utf8(p, "again") && get(p, &bytes) && bytes == "again");
        if (geteuid() != 0) {   // root reads anything
            chmod(p.c_str(), 0000);
            CHECK(durable::read_file_utf8_migrating(p, &bytes) == durable::ReadResult::Unreadable && bytes.empty());
            CHECK(!durable::write_file_atomic_utf8(p, "must not land"));   // the unreadable real file is never replaced
            chmod(p.c_str(), 0644);
            CHECK(get(p, &bytes) && bytes == "again");
            CHECK(durable::read_file_utf8_migrating(p, &bytes) == durable::ReadResult::Primary);   // readable again: guard lifted
            CHECK(durable::write_file_atomic_utf8(p, "now ok") && get(p, &bytes) && bytes == "now ok");
        }
    }

    /* the committed file keeps the temp's modification time (rename moves the inode) */
    {
        const std::string dst = plat_join(root, "mt.dat"), tmp = dst + ".hubtmp";
        CHECK(put(dst, "old"));
        CHECK(put(tmp, "stamped"));
        const auto past = fs::file_time_type::clock::now() - std::chrono::hours(24 * 400);
        fs::last_write_time(fs::path(tmp), past, ec);
        const auto want = fs::last_write_time(fs::path(tmp), ec);
        CHECK(durable::commit_replace(W(tmp), W(dst)));
        CHECK(fs::last_write_time(fs::path(dst), ec) == want);
    }

    /* read-only temp (as fs::copy_file leaves it): committed, final file stays read-only */
    {
        const std::string dst = plat_join(root, "ro.dat"), tmp = dst + ".hubtmp";
        CHECK(put(dst, "old"));
        CHECK(put(tmp, "from read-only source"));
        chmod(tmp.c_str(), 0444);
        CHECK(durable::commit_replace(W(tmp), W(dst)));
        std::string got;
        CHECK(get(dst, &got) && got == "from read-only source");
        struct stat st;
        CHECK(stat(dst.c_str(), &st) == 0 && !(st.st_mode & S_IWUSR));
        CHECK(!exists(tmp));
        chmod(dst.c_str(), 0644);
    }

    /* a relative path */
    {
        const fs::path cwd = fs::current_path();
        fs::current_path(fs::path(root));
        CHECK(put("rel.dat", "old"));
        CHECK(durable::write_file_durable(L"rel.dat", std::string("relative")));
        std::string got;
        CHECK(get(plat_join(root, "rel.dat"), &got) && got == "relative");
        CHECK(put("rel.dat.tmp", "second"));
        CHECK(durable::commit_replace(L"rel.dat.tmp", L"rel.dat"));
        CHECK(get(plat_join(root, "rel.dat"), &got) && got == "second");
        fs::current_path(cwd);
    }

    fs::remove_all(root, ec);
    std::printf(g_fail ? "durable_write_test: %d FAILED\n" : "durable_write_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
#endif
