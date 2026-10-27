/* winutil: UTF-8/UTF-16, quoting, file helpers. Console test. */
#include "winutil.h"

#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

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
