// Console test for "Add games" (addgames.cpp): sorting, destination, dedupe, copy job. Scratch dirs only.
#include "addgames.h"
#include "library.h"
#include "winutil.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace hub;
using namespace hub::addgames;
namespace fs = std::filesystem;
typedef std::vector<uint8_t> Bytes;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

static bool wr(const fs::path& p, const Bytes& d) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(d.data()), (std::streamsize)d.size());
    return (bool)f;
}
static Bytes noise(size_t n, uint32_t seed) {
    Bytes v(n);
    uint32_t x = seed;
    for (auto& b : v) { x = x * 1103515245u + 12345u; b = uint8_t(x >> 16); }
    return v;
}
static void put(Bytes& v, size_t off, const char* s) { std::memcpy(&v[off], s, std::strlen(s)); }
static Bytes make_n64(const char* title, uint32_t seed) {
    Bytes v = noise(1024 * 1024, seed);
    v[0] = 0x80; v[1] = 0x37; v[2] = 0x12; v[3] = 0x40;
    for (size_t i = 0x20; i < 0x34; ++i) v[i] = ' ';
    put(v, 0x20, title);
    put(v, 0x3B, "NTSE");
    return v;
}
static Bytes make_gb(const char* title, uint16_t global, uint32_t seed) {
    Bytes v = noise(32 * 1024, seed);
    std::memset(&v[0x134], 0, 16);
    put(v, 0x134, title);
    v[0x143] = 0;
    v[0x14E] = uint8_t(global >> 8);
    v[0x14F] = uint8_t(global);
    uint8_t x = 0;
    for (int i = 0x134; i <= 0x14C; ++i) x = uint8_t(x - v[i] - 1);
    v[0x14D] = x;
    return v;
}

static std::vector<Item> run_job(const std::vector<std::string>& files, const std::string& root, const std::set<std::string>& known) {
    Job j;
    j.start(files, root, known);
    j.join();
    CHECK(j.done());
    return j.items();
}

int main() {
    // ---- pure helpers ----
    CHECK(is_game_file("C:\\x\\Mario.Z64") && is_game_file("a.7z") && is_game_file("a.wux") && is_game_file("a.cci"));
    CHECK(!is_game_file("a.txt") && !is_game_file("noext"));
    CHECK(console_folder(System::N64) == "Nintendo 64" && console_folder(System::WiiU) == "Wii U" && console_folder(System::NDS) == "DS");
    CHECK(console_folder(System::GBC) == "Game Boy Color" && console_folder(System::Unknown) == "Other");
    CHECK(games_root({"D:\\mine", "E:\\Stuff\\RetroHub Games\\"}, "C:\\Users\\a\\Documents") == "E:\\Stuff\\RetroHub Games");
    CHECK(games_root({"D:\\mine"}, "C:\\Users\\a\\Documents") == "C:\\Users\\a\\Documents\\RetroHub Games");
    CHECK(games_root({}, "C:\\Users\\a\\Documents\\") == "C:\\Users\\a\\Documents\\RetroHub Games");
    CHECK(dest_dir("C:\\G", System::GC) == "C:\\G\\GameCube");
    {
        auto ex = [](const std::string& p) { return p == "d\\a.gba" || p == "d\\a (2).gba"; };
        CHECK(unique_name("d", "a.gba", ex) == "a (3).gba");
        CHECK(unique_name("d", "b.gba", ex) == "b.gba");
    }

    // ---- job on scratch dirs ----
    fs::path base = fs::temp_directory_path() / "rh_addgames_test";
    std::error_code ec;
    fs::remove_all(base, ec);
    library_set_data_dir((base / "data").string());
    fs::path in = base / "in", root = base / "games";
    Bytes n64 = make_n64("TEST GAME 64", 1), gb = make_gb("TETRIS", 0x1234, 2);
    CHECK(wr(in / "mario.z64", n64));
    CHECK(wr(in / "tetris.gb", gb));
    CHECK(wr(in / "weird.gba", noise(2048, 3)));   // not a valid GBA header -> Other
    CHECK(wr(in / "notes.txt", noise(10, 4)));

    GameEntry g64, ggb;
    CHECK(identify_game((in / "mario.z64").string(), &g64) && g64.system == System::N64);
    CHECK(identify_game((in / "tetris.gb").string(), &ggb) && ggb.system == System::GB);

    auto items = run_job({(in / "mario.z64").string(), (in / "tetris.gb").string(), (in / "weird.gba").string(), (in / "notes.txt").string()},
                         root.string(), {});
    CHECK(items.size() == 4);
    CHECK(count_outcome(items, Outcome::Added) == 3 && count_outcome(items, Outcome::Unsupported) == 1);
    CHECK(fs::exists(root / "Nintendo 64" / "mario.z64"));
    CHECK(fs::exists(root / "Game Boy" / "tetris.gb"));
    CHECK(fs::exists(root / "Other" / "weird.gba"));
    CHECK(fs::exists(in / "mario.z64") && fs::exists(in / "tetris.gb"));   // sources untouched (copied, not moved)
    CHECK(fs::file_size(root / "Nintendo 64" / "mario.z64") == n64.size());
    CHECK(!fs::exists(root / "Nintendo 64" / "mario.z64.part"));
    bool other_flag = false;
    for (auto& i : items) if (i.src.find("weird") != std::string::npos) other_flag = i.other;
    CHECK(other_flag);
    std::string sum = summary_text(items);
    CHECK(sum.find("Added 3 games") == 0 && sum.find("Other folder") != std::string::npos && sum.find("notes.txt") != std::string::npos);

    // duplicate by library key, and by same file twice in one batch
    wr(in / "mario copy.z64", n64);
    items = run_job({(in / "mario copy.z64").string(), (in / "tetris.gb").string()}, root.string(), {g64.key});
    CHECK(count_outcome(items, Outcome::Duplicate) == 2 && count_outcome(items, Outcome::Added) == 0);   // mario by library key, tetris by same name+size already in the destination
    CHECK(!fs::exists(root / "Nintendo 64" / "mario copy.z64"));
    CHECK(summary_text(items).find("Already in your library") != std::string::npos);
    // tetris.gb already in the destination with the same size -> duplicate even with no key known
    items = run_job({(in / "tetris.gb").string()}, root.string(), {});
    CHECK(items.size() == 1 && items[0].outcome == Outcome::Duplicate);
    // two copies of one game in the same batch: only one lands
    fs::remove_all(root, ec);
    items = run_job({(in / "mario.z64").string(), (in / "mario copy.z64").string()}, root.string(), {});
    CHECK(count_outcome(items, Outcome::Added) == 1 && count_outcome(items, Outcome::Duplicate) == 1);
    // name clash with a different file: gets a numbered name
    wr(root / "Game Boy" / "tetris.gb", noise(100, 9));
    items = run_job({(in / "tetris.gb").string()}, root.string(), {});
    CHECK(items.size() == 1 && items[0].outcome == Outcome::Added && fs::exists(root / "Game Boy" / "tetris (2).gb"));
    // unreadable / missing file
    items = run_job({(in / "missing.gb").string()}, root.string(), {});
    CHECK(items.size() == 1 && items[0].outcome == Outcome::Failed);
    CHECK(summary_text(items).find("Could not add") == 0);
    // unwritable destination (root is a file) -> Failed, no crash
    wr(base / "afile", noise(10, 1));
    items = run_job({(in / "mario.z64").string()}, (base / "afile").string(), {});
    CHECK(items.size() == 1 && items[0].outcome == Outcome::Failed);

    // copy_file: progress + cancel leaves no partial file
    {
        std::string err;
        uint64_t tot = 0;
        int calls = 0;
        CHECK(copy_file((in / "mario.z64").string(), (base / "c1.z64").string(), &tot, [&](uint64_t, uint64_t) { ++calls; return true; }, &err));
        CHECK(calls >= 1 && tot == n64.size());
        CHECK(!copy_file((in / "mario.z64").string(), (base / "c2.z64").string(), nullptr, [](uint64_t, uint64_t) { return false; }, &err));
        CHECK(err == "cancelled" && !fs::exists(base / "c2.z64") && !fs::exists(base / "c2.z64.part"));
    }

    library_set_data_dir("");
    fs::remove_all(base, ec);
    std::printf(g_fail ? "addgames_test: %d FAILED\n" : "addgames_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
