/* Console test: UTF-8-correct file helpers (u8file.h). Round trips in temp folders named with several scripts, the account_code.txt
 * legacy-location migration, and byte-identical behaviour on ASCII paths. Offline, temp dirs only. */
#include "u8file.h"

#include <cstdio>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static std::string U(const fs::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); }
static bool put_wide(const std::wstring& wp, const std::string& bytes) {
    std::error_code ec;
    fs::create_directories(fs::path(wp).parent_path(), ec);
    std::ofstream o(wp, std::ios::binary | std::ios::trunc);
    o << bytes;
    return (bool)o;
}
static std::string get_wide(const std::wstring& wp) {
    std::ifstream i(wp, std::ios::binary);
    std::stringstream s;
    s << i.rdbuf();
    return s.str();
}

static void run(const std::string& label, const fs::path& dir) {
    std::printf("-- %s\n", label.c_str());
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const std::string base = U(dir) + "\\";

    /* slurp / open_in / open_out */
    const std::string data("a\0b\r\nc", 6);
    {
        std::ofstream o = u8file::open_out(base + "x.bin", std::ios::binary | std::ios::trunc);
        o << data;
    }
    std::string got;
    CHECK(u8file::slurp(base + "x.bin", &got) && got == data);
    CHECK(get_wide(durable::utf8_to_wide(base + "x.bin")) == data);
    { std::ofstream a = u8file::open_out(base + "x.bin", std::ios::app | std::ios::binary); a << "Z"; }
    { std::ifstream i = u8file::open_in(base + "x.bin", std::ios::binary); std::stringstream s; s << i.rdbuf(); CHECK(s.str() == data + "Z"); }
    CHECK(!u8file::slurp(base + "nope.bin", &got));
    if (FILE* f = u8file::fopen_utf8(base + "x.bin", "rb")) { CHECK(std::fgetc(f) == 'a'); std::fclose(f); } else CHECK(false);

    /* account code: write, read back, CRLF tolerated, unreadable/missing never invents one */
    const std::string cp = base + "account_code.txt";
    std::string code = "keep";
    CHECK(!u8file::read_account_code(cp, &code) && code == "keep");   // missing file: the caller's value is untouched
    CHECK(u8file::write_account_code(cp, "ABCD-EFGH-JKLM"));
    CHECK(get_wide(durable::utf8_to_wide(cp)) == "ABCD-EFGH-JKLM\n");   // exact bytes the old ofstream wrote
    CHECK(u8file::read_account_code(cp, &code) && code == "ABCD-EFGH-JKLM");
    CHECK(put_wide(durable::utf8_to_wide(cp), "ABCD-EFGH-JKLM \r\nsecond line\n"));
    CHECK(u8file::read_account_code(cp, &code) && code == "ABCD-EFGH-JKLM");
    u8file::clear_account_code(cp);
    code = "keep";
    CHECK(!u8file::read_account_code(cp, &code) && code == "keep");   // cleared

    /* legacy location: older builds wrote the UTF-8 bytes read as the ANSI code page */
    const std::wstring wp = durable::utf8_to_wide(cp), wl = durable::legacy_ansi_to_wide(cp);
    if (wp == wl) {
        std::printf("   (ANSI reading of this path is identical: no legacy location, migration sub-case not applicable)\n");
    } else {
        fs::remove(fs::path(wp), ec);
        if (!put_wide(wl, "WXYZ-WXYZ-WXYZ\n")) {
            std::printf("   SKIP legacy migration: the ANSI code page cannot hold this path\n");
        } else {
            CHECK(u8file::read_account_code(cp, &code) && code == "WXYZ-WXYZ-WXYZ");
            CHECK(get_wide(wp) == "WXYZ-WXYZ-WXYZ\n");            // re-saved at the correct path
            CHECK(get_wide(wl) == "WXYZ-WXYZ-WXYZ\n");            // legacy file never touched
            CHECK(put_wide(wp, "NEWW-NEWW-NEWW\n"));               // the correct file wins once it exists
            CHECK(u8file::read_account_code(cp, &code) && code == "NEWW-NEWW-NEWW");
            u8file::clear_account_code(cp);                        // the old code must not come back from the legacy file
            code = "keep";
            CHECK(!u8file::read_account_code(cp, &code) && code == "keep");
            CHECK(get_wide(wl) == "WXYZ-WXYZ-WXYZ\n");
        }
        /* locked file: a code that exists but cannot be read is neither shown nor overwritten */
        CHECK(u8file::write_account_code(cp, "LOCK-LOCK-LOCK"));
        HANDLE h = CreateFileW(wp.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);   // exclusive
        if (h != INVALID_HANDLE_VALUE) {
            code = "keep";
            CHECK(!u8file::read_account_code(cp, &code) && code == "keep");
            CHECK(!u8file::write_account_code(cp, "OTHR-OTHR-OTHR"));
            CloseHandle(h);
            CHECK(get_wide(wp) == "LOCK-LOCK-LOCK\n");
        }
    }
    fs::remove_all(dir, ec);
}

int main() {
    const std::string pid = std::to_string(GetCurrentProcessId());
    run("ASCII path", fs::temp_directory_path() / ("rh_u8file_ascii_" + pid));
    run("multi-script path", fs::temp_directory_path() / fs::u8path("rh_u8file_Zo\xC3\xAB_\xD0\x96_\xE6\x97\xA5\xE6\x9C\xAC_" + pid));
    std::printf(g_fail ? "FAILED (%d)\n" : "all ok\n", g_fail);
    return g_fail ? 1 : 0;
}
