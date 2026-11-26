/* winutil: UTF-8/UTF-16, quoting, file helpers. Console test. */
#include "winutil.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <filesystem>
#include <unistd.h>
#endif

#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

#ifdef _WIN32
/* Parse a command line the way the CRT does and return the args after argv[0]. */
static std::vector<std::wstring> parse_back(const std::wstring& tail) {
    std::wstring cmd = L"prog.exe " + tail;
    int argc = 0;
    LPWSTR* av = CommandLineToArgvW(cmd.c_str(), &argc);
    std::vector<std::wstring> r;
    for (int i = 1; i < argc; ++i) r.push_back(av[i]);
    LocalFree(av);
    return r;
}

int main() {
    /* UTF-8 <-> UTF-16 */
    std::string u8 = "caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x98\x80";
    std::wstring w = winutil::to_wide(u8);
    CHECK(w == L"caf\u00e9 \u65e5\u672c \U0001F600");
    CHECK(winutil::to_utf8(w) == u8);
    CHECK(winutil::to_wide("").empty() && winutil::to_utf8(std::wstring()).empty() && winutil::to_utf8((const wchar_t*)nullptr).empty());

    /* getenv_utf8: a path variable with non-ASCII letters (a profile like C:\Users\Zoë) comes back as UTF-8, which std::getenv (ANSI)
     * does not; an unset variable is "" / false, an empty one is "" / true. */
    {
        const wchar_t* name = L"RETROHUB_WINUTIL_TEST_PATH";
        const std::wstring val = L"C:\\Users\\Zo\u00eb_\u0416\\AppData\\Roaming";
        SetEnvironmentVariableW(name, val.c_str());
        std::string got;
        CHECK(winutil::getenv_utf8("RETROHUB_WINUTIL_TEST_PATH", &got) && got == winutil::to_utf8(val));
        CHECK(winutil::getenv_utf8("RETROHUB_WINUTIL_TEST_PATH") == "C:\\Users\\Zo\xC3\xAB_\xD0\x96\\AppData\\Roaming");
        SetEnvironmentVariableW(name, L"");
        CHECK(winutil::getenv_utf8("RETROHUB_WINUTIL_TEST_PATH", &got) && got.empty());
        SetEnvironmentVariableW(name, nullptr);
        CHECK(!winutil::getenv_utf8("RETROHUB_WINUTIL_TEST_PATH", &got) && got.empty());
    }
    CHECK(winutil::to_wide("\xFF\xFE").size() == 2);   // invalid UTF-8 -> U+FFFD per bad byte, no crash

    /* exe location */
    std::wstring ep = winutil::exe_path_w();
    CHECK(!ep.empty() && ep.size() > 4);
    std::wstring ed = winutil::exe_dir_w();
    CHECK(!ed.empty() && ed.back() == L'\\' && ep.compare(0, ed.size(), ed) == 0);
    CHECK(winutil::exe_dir() == winutil::to_utf8(ed));
    CHECK(winutil::to_wide(winutil::exe_path()) == ep);

    /* quoting round trip through CommandLineToArgvW */
    const wchar_t* cases[] = {L"plain", L"with space", L"", L"C:\\dir with space\\", L"C:\\dir\\", L"a\"b", L"a\\\"b", L"a\\\\\"b",
                              L"trail\\\\", L"\"", L"\\", L"\\\\", L"tab\there", L"C:\\Users\\caf\u00e9 \u65e5\u672c\\ROM name.z64", L"a b\" c"};
    for (const wchar_t* c : cases) {
        std::vector<std::wstring> got = parse_back(winutil::quote_arg(std::wstring(c)));
        CHECK(got.size() == 1 && got[0] == c);
    }
    {   // several args keep their boundaries
        std::wstring line = winutil::quote_arg(L"a b") + L" " + winutil::quote_arg(L"c\\") + L" " + winutil::quote_arg(L"d\"e");
        std::vector<std::wstring> got = parse_back(line);
        CHECK(got.size() == 3 && got[0] == L"a b" && got[1] == L"c\\" && got[2] == L"d\"e");
    }
    CHECK(winutil::quote_arg(std::string("x y")) == "\"x y\"");
    CHECK(winutil::quote_arg(std::string("C:\\d\\")) == "\"C:\\d\\\\\"");

    /* files with non-ASCII paths */
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"winutil_test_" + std::to_wstring(GetCurrentProcessId()) + L"_\u00e9\u65e5\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::string p = winutil::to_utf8(dir) + "f\xC3\xA9.bin";
    std::string data("bin\0data\r\n\xFF", 11);
    CHECK(winutil::write_file(p, data));
    std::string back;
    CHECK(winutil::read_file(p, &back) && back == data);
    std::vector<uint8_t> bytes;
    CHECK(winutil::read_file(p, &bytes) && bytes.size() == data.size() && bytes[3] == 0);
    CHECK(winutil::write_file_atomic(p, "second") && winutil::read_file(p, &back) && back == "second");
    CHECK(GetFileAttributesW((winutil::to_wide(p) + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES);   // temp file gone
    CHECK(winutil::write_file(p, "") && winutil::read_file(p, &back) && back.empty());
    CHECK(!winutil::read_file(winutil::to_utf8(dir) + "missing.bin", &back) && back.empty());
    CHECK(!winutil::write_file(winutil::to_utf8(dir) + "no\\such\\dir\\x", "a"));
    DeleteFileW(winutil::to_wide(p).c_str());
    RemoveDirectoryW(dir.c_str());

    if (g_fail) { std::printf("winutil_test: %d FAILED\n", g_fail); return 1; }
    std::printf("winutil_test: all passed\n");
    return 0;
}
#else  // macOS
int main() {
    /* UTF-8 <-> UTF-32 (wchar_t) */
    std::string u8 = "caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x98\x80";
    std::wstring w = winutil::to_wide(u8);
    CHECK(w == L"caf\u00e9 \u65e5\u672c \U0001F600");
    CHECK(winutil::to_utf8(w) == u8);
    CHECK(winutil::to_wide("").empty() && winutil::to_utf8(std::wstring()).empty() && winutil::to_utf8((const wchar_t*)nullptr).empty());
    CHECK(winutil::to_wide("\xFF\xFE").size() == 2);   // invalid UTF-8 -> U+FFFD per bad byte, no crash
    CHECK(winutil::to_wide("a\xC3") == std::wstring(L"a\uFFFD"));   // truncated sequence
    CHECK(winutil::to_wide("\xC0\x80") == std::wstring(2, (wchar_t)0xFFFD));   // overlong

    /* getenv_utf8 */
    {
        const std::string val = "/Users/Zo\xC3\xAB_\xD0\x96/Library";
        std::string got;
        setenv("RETROHUB_WINUTIL_TEST_PATH", val.c_str(), 1);
        CHECK(winutil::getenv_utf8("RETROHUB_WINUTIL_TEST_PATH", &got) && got == val);
        CHECK(winutil::getenv_utf8("RETROHUB_WINUTIL_TEST_PATH") == val);
        setenv("RETROHUB_WINUTIL_TEST_PATH", "", 1);
        CHECK(winutil::getenv_utf8("RETROHUB_WINUTIL_TEST_PATH", &got) && got.empty());
        unsetenv("RETROHUB_WINUTIL_TEST_PATH");
        CHECK(!winutil::getenv_utf8("RETROHUB_WINUTIL_TEST_PATH", &got) && got.empty());
    }

    /* exe location */
    std::string ep = winutil::exe_path();
    std::string ed = winutil::exe_dir();
    CHECK(!ep.empty() && ep[0] == '/');
    CHECK(!ed.empty() && ed.back() == '/' && ep.compare(0, ed.size(), ed) == 0);
    CHECK(winutil::to_wide(ep) == winutil::exe_path_w());
    CHECK(winutil::to_wide(ed) == winutil::exe_dir_w());

    /* POSIX single-quote quoting */
    CHECK(winutil::quote_arg(std::string("x y")) == "'x y'");
    CHECK(winutil::quote_arg(std::string("")) == "''");
    CHECK(winutil::quote_arg(std::string("it's")) == "'it'\\''s'");
    CHECK(winutil::quote_arg(std::string("a\\b\"c$d")) == "'a\\b\"c$d'");
    CHECK(winutil::quote_arg(std::wstring(L"x y")) == L"'x y'");

    /* files with non-ASCII paths */
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / fs::u8path("winutil_test_" + std::to_string(getpid()) + "_\xC3\xA9\xE6\x97\xA5");
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    std::string d8 = dir.string() + "/";
    std::string p = d8 + "f\xC3\xA9.bin";
    std::string data("bin\0data\r\n\xFF", 11);
    CHECK(winutil::write_file(p, data));
    std::string back;
    CHECK(winutil::read_file(p, &back) && back == data);
    std::vector<uint8_t> bytes;
    CHECK(winutil::read_file(p, &bytes) && bytes.size() == data.size() && bytes[3] == 0);
    CHECK(winutil::read_file_w(winutil::to_wide(p), &back) && back == data);
    CHECK(winutil::write_file_atomic(p, "second") && winutil::read_file(p, &back) && back == "second");
    CHECK(!fs::exists(fs::path(p + ".tmp")));   // temp file gone
    CHECK(winutil::write_file(p, "") && winutil::read_file(p, &back) && back.empty());
    CHECK(!winutil::read_file(d8 + "missing.bin", &back) && back.empty());
    CHECK(!winutil::write_file(d8 + "no/such/dir/x", "a"));
    fs::remove_all(dir, ec);

    if (g_fail) { std::printf("winutil_test: %d FAILED\n", g_fail); return 1; }
    std::printf("winutil_test: all passed\n");
    return 0;
}
#endif
