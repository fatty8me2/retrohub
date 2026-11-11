#include <cstdio>
#include <filesystem>

#include "recycle_bin.h"

namespace fs = std::filesystem;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main() {
    using namespace hubrb;
    std::wstring b = shfile_path_buffer(L"C:\\a\\b.png");
    CHECK(b.size() == 10 + 2 && b[10] == L'\0' && b[11] == L'\0' && b.compare(0, 10, L"C:\\a\\b.png") == 0);
    CHECK(shfile_path_buffer(L"").size() == 2);
    CHECK(drive_type_has_bin(3) && !drive_type_has_bin(4) && !drive_type_has_bin(2) && !drive_type_has_bin(0));   // fixed yes; network, removable, unknown no

    fs::path d = fs::temp_directory_path() / "hubrb_test_del";
    fs::remove_all(d);
    fs::create_directories(d);
    std::string f1 = (d / "a.png").u8string(), f2 = (d / "b.png").u8string(), f3 = (d / "c.png").u8string();
    for (auto& f : {f1, f2, f3}) { FILE* fp = std::fopen(f.c_str(), "wb"); if (fp) { std::fputs("x", fp); std::fclose(fp); } }
    int calls = 0;
    CHECK(delete_user_file(f1, [&](const std::string& p) { ++calls; std::error_code e; return fs::remove(fs::u8path(p), e); }));   // fake bin takes it
    CHECK(calls == 1 && !fs::exists(d / "a.png"));
    CHECK(delete_user_file(f2, [&](const std::string&) { ++calls; return false; }));   // bin refuses: permanent delete fallback
    CHECK(calls == 2 && !fs::exists(d / "b.png"));
    CHECK(delete_user_file(f2, [&](const std::string&) { ++calls; return false; }) && calls == 2);   // already gone: success, bin not asked
    CHECK(fs::exists(d / "c.png"));   // untouched by the calls above
    fs::remove_all(d);
    if (!fails) std::printf("hub_recycle_test: all passed\n");
    return fails ? 1 : 0;
}
