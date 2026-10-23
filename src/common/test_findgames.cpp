// Console unit test for findgames.cpp (ranking / collapsing + a bounded scan of a scratch tree). Exit code 0 = all passed.
#include "findgames.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace hub::fg;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

static DirCount dc(const char* path, int sys, int n, int arch = 0) {
    DirCount d;
    d.path = path;
    if (sys >= 0) d.c.n[sys] = n;
    d.c.archives = arch;
    return d;
}

static void touch(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << "x";
}

int main() {
    {   // a scan still running when the runner goes away is cancelled and joined (no std::terminate)
        std::atomic<bool> saw_cancel{false};
        {
            hub::fg::ScanRunner r;
            r.start([&] { while (!r.ctl.cancel) std::this_thread::sleep_for(std::chrono::milliseconds(2)); saw_cancel = true; });
        }
        CHECK(saw_cancel);
        hub::fg::ScanRunner r2;
        r2.stop();   // idle stop is a no-op
        int n = 0;
        r2.start([&] { n = 1; });
        r2.join();
        CHECK(n == 1);
    }
    CHECK(classify_ext(".z64") == N64 && classify_ext(".rvz") == GC && classify_ext(".gbc") == GBC && classify_ext(".wua") == WIIU);
    CHECK(classify_ext(".zip") == kArchive && classify_ext(".7z") == kArchive && classify_ext(".txt") == kNone && classify_ext("") == kNone);
    CHECK(norm_path("C:/Users/A/") == "c:\\users\\a" && skip_dir_name("Windows") && skip_dir_name("$RECYCLE.BIN") && !skip_dir_name("Roms"));

    const std::vector<Root> roots = {{"C:\\U\\Desktop", false}, {"C:\\U\\Documents", false}, {"C:\\U\\Documents\\RetroHub Games", true}, {"D:\\Games", true}};

    /* siblings collapse into their parent; the parent narrows to the deepest common folder */
    {
        auto r = collapse_folders({dc("C:\\U\\Desktop\\Emu\\Roms\\N64", N64, 5), dc("C:\\U\\Desktop\\Emu\\Roms\\GBA", GBA, 3), dc("C:\\U\\Desktop\\Emu\\Roms", GB, 1)}, roots);
        CHECK(r.size() == 1 && r[0].path == "C:\\U\\Desktop\\Emu\\Roms" && r[0].c.games() == 9 && r[0].c.n[N64] == 5 && r[0].ticked);
    }
    /* a single leaf stays as deep as its files; two groups rank by game count */
    {
        auto r = collapse_folders({dc("C:\\U\\Desktop\\A\\B\\C", N64, 2), dc("C:\\U\\Documents\\Big", NDS, 40)}, roots);
        CHECK(r.size() == 2 && r[0].path == "C:\\U\\Documents\\Big" && r[1].path == "C:\\U\\Desktop\\A\\B\\C");
    }
    /* stray files straight in Desktop never make a row; few zips are noise, three or more are a library */
    {
        auto r = collapse_folders({dc("C:\\U\\Desktop", GBA, 9), dc("C:\\U\\Desktop\\Z1", -1, 0, 2), dc("C:\\U\\Desktop\\Z2", -1, 0, 3)}, roots);
        CHECK(r.size() == 1 && r[0].path == "C:\\U\\Desktop\\Z2" && r[0].c.archives == 3 && r[0].c.games() == 0);
    }
    /* library roots are rows themselves (never narrowed) and win over the generic root they sit in; case/slash-insensitive */
    {
        auto r = collapse_folders({dc("c:/u/documents/retrohub games/N64", N64, 2), dc("C:\\U\\Documents\\RetroHub Games\\GBA\\Hacks", GBA, 1), dc("D:\\Games\\X", GC, 4)}, roots);
        CHECK(r.size() == 2 && r[0].path == "D:\\Games" && r[0].c.games() == 4);
        CHECK(r[1].path == "C:\\U\\Documents\\RetroHub Games" && r[1].c.games() == 3);
    }
    /* a folder under no root still becomes a row; empty dirs are ignored */
    {
        auto r = collapse_folders({dc("E:\\Stuff\\Carts", GBC, 1), dc("E:\\Stuff\\Empty", -1, 0)}, roots);
        CHECK(r.size() == 1 && r[0].path == "E:\\Stuff\\Carts");
    }
    /* emulator config parsers */
    {
        auto d = parse_dolphin_ini_paths("[General]\r\nISOPaths = 2\r\nISOPath0 = D:\\GC Games\r\nISOPath1=E:/Wii\r\nISOPath2 = relative\r\n");
        CHECK(d.size() == 2 && d[0] == "D:\\GC Games" && d[1] == "E:\\Wii");
        auto c = parse_cemu_xml_paths("<content><GamePaths><Entry>F:\\WiiU</Entry><Entry>F:\\WiiU</Entry></GamePaths><Other><Entry>X:\\no</Entry></Other></content>");
        CHECK(c.size() == 1 && c[0] == "F:\\WiiU");
        auto q = parse_qt_gamedirs("[UI]\nPaths\\gamedirs\\size=1\nPaths\\gamedirs\\1\\path=G:/3DS\nPaths\\gamedirs\\1\\deep=false\n");
        CHECK(q.size() == 1 && q[0] == "G:\\3DS");
    }

    /* the real walk on a scratch tree: depth cap, skipped system folders, junction loop, cancel */
    {
        fs::path base = fs::temp_directory_path() / "findgames_test_tree";
        std::error_code ec;
        fs::remove_all(base, ec);
        touch(base / "Desktop" / "Emu" / "Roms" / "N64" / "a.z64");
        touch(base / "Desktop" / "Emu" / "Roms" / "N64" / "b.V64");
        touch(base / "Desktop" / "Emu" / "Roms" / "GBA" / "c.gba");
        touch(base / "Desktop" / "Emu" / "readme.txt");
        touch(base / "Desktop" / "loose.gba");
        touch(base / "Downloads" / "pack" / "1.zip");
        touch(base / "Downloads" / "node_modules" / "m.gba");
        touch(base / "Documents" / "a" / "b" / "c" / "d" / "e" / "too_deep.gba");
        touch(base / "Games" / "wiiu" / "game.wua");
        std::string cmd = "mklink /J \"" + (base / "Desktop" / "Emu" / "loop").string() + "\" \"" + (base / "Desktop").string() + "\" >nul 2>&1";
        std::system(cmd.c_str());
        std::vector<Root> rs = {{(base / "Desktop").string(), false}, {(base / "Downloads").string(), false}, {(base / "Documents").string(), false},
                                {(base / "Games").string(), true}};
        Control ctl;
        auto r = scan_roots(rs, &ctl);
        CHECK(ctl.done);
        CHECK(r.size() == 2);
        if (r.size() == 2) {
            CHECK(norm_path(r[0].path) == norm_path((base / "Desktop" / "Emu" / "Roms").string()) && r[0].c.games() == 3 && r[0].c.n[N64] == 2 && r[0].c.n[GBA] == 1);
            CHECK(norm_path(r[1].path) == norm_path((base / "Games").string()) && r[1].c.n[WIIU] == 1);
        }
        Control stop;
        stop.cancel = true;
        CHECK(scan_roots(rs, &stop).empty() && stop.done);
        fs::remove_all(base, ec);
    }

    std::printf(g_fail ? "findgames_test: %d FAILED\n" : "findgames_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
