// Console test for the Controller Pak policy and .mpk file naming (no emulator, no network).
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "mempak.h"
#include "save_sync.h"

using namespace n64run;
namespace fs = std::filesystem;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main() {
    // Policy: offline always on; online only with shared host saves.
    CHECK(mempak_enabled(false, false));
    CHECK(mempak_enabled(false, true));
    CHECK(!mempak_enabled(true, false));
    CHECK(mempak_enabled(true, true));

    // Names: .mpk is a battery name for sync, case-insensitive; other names and paths are rejected.
    CHECK(is_mempak_file("Mario Kart 64 (U) [!]-3A67D998.mpk"));
    CHECK(is_mempak_file("PAK.MPK"));
    CHECK(!is_mempak_file("notes.mpk.txt"));
    CHECK(!is_mempak_file("mpk"));
    CHECK(!is_mempak_file("../x.mpk"));
    // Rumble Pak: offline + profile option only; the core's rumble write is decoded to the motor state.
    CHECK(rumble_enabled(false, true) && !rumble_enabled(true, true) && !rumble_enabled(false, false));
    {
        unsigned char cmd[0x26] = {0x23, 0x01, 0x03, 0xC0, 0x1B};
        bool on = false;
        for (int i = 5; i < 0x25; i++) cmd[i] = 0x01;
        CHECK(decode_rumble_command(cmd, &on) && on);
        for (int i = 5; i < 0x25; i++) cmd[i] = 0x00;
        CHECK(decode_rumble_command(cmd, &on) && !on);
        cmd[3] = 0x80;   // a different pak address is not rumble
        CHECK(!decode_rumble_command(cmd, &on));
    }
    CHECK(!is_mempak_file("a\\b.mpk"));
    CHECK(!is_mempak_file("Game.eep"));

    // Paths: a pak written into a save dir is loaded back by load_save_dir and written by write_save_file.
    fs::path d = fs::temp_directory_path() / "n64run_mempak_test";
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    std::vector<uint8_t> pak(32768, 0x5A);
    CHECK(write_save_file(d.string(), "Mario Kart 64 (U) [!]-3A67D998.mpk", pak));
    CHECK(fs::exists(d / "Mario Kart 64 (U) [!]-3A67D998.mpk"));
    CHECK(!write_save_file(d.string(), "../escape.mpk", pak));   // traversal rejected
    CHECK(!fs::exists(d.parent_path() / "escape.mpk"));
    std::vector<SaveFile> got;
    std::vector<std::string> skipped;
    CHECK(load_save_dir(d.string(), &got, &skipped));
    CHECK(got.size() == 1 && got[0].name == "Mario Kart 64 (U) [!]-3A67D998.mpk" && got[0].data == pak);
    fs::remove_all(d, ec);

    std::printf("%s (%d failure(s))\n", fails ? "FAILED" : "OK", fails);
    return fails ? 1 : 0;
}
