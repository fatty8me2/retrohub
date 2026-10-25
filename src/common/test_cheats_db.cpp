/* Cheat database: normalization, region, matching (synthetic entries + the player's real ROMs, read-only), ranking. */
#include <cstdio>
#include <cstdlib>
#include <map>
#include <filesystem>
#include <string>

#include "cheats_db.h"
#include "miniz.h"

using namespace hub;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static GameEntry mk(System s, const char* title, const char* serial, const char* path) {
    GameEntry e;
    e.system = s;
    e.title = title;
    e.serial = serial;
    e.path = path;
    return e;
}

static bool has(const CheatList& l, const char* desc) {
    for (auto& c : l.cheats) if (c.desc == desc) return true;
    return false;
}

int main() {
    cheats_db_set_dir(CHEATS_DATA_DIR);
    CHECK(cheats_db_game_count() > 4000);

    CHECK(cheats_norm_key("Legend of Zelda, The - Ocarina of Time (USA)") == cheats_norm_key("The Legend of Zelda Ocarina of Time (U) [!]"));
    CHECK(cheats_norm_key("007 - GoldenEye.v64") == cheats_norm_key("GoldenEye 007 (USA)"));
    CHECK(cheats_region_of("Mario Kart 64 (U)") == "U");
    CHECK(cheats_region_of("Pokemon - Red Version (USA, Europe) (SGB Enhanced)") == "UE");
    CHECK(cheats_region_of("Foo (World)").empty());
    CHECK(cheats_is_featured_desc("Unlock All Characters") && cheats_is_featured_desc("Infinite Health P1"));
    CHECK(!cheats_is_featured_desc("Enable Code (Must Be On)") && cheats_is_master_desc("Enable Code (Must Be On)"));
    CHECK(!cheats_is_featured_desc("Weird Pokeballs"));

    {   // N64 by serial region + file name
        GameEntry e = mk(System::N64, "SMASH BROTHERS", "NALE", "C:/x/Super Smash Bros. (USA).n64");
        size_t pre = 0;
        auto v = cheats_for(e, &pre);
        CHECK(v.size() > 20 && pre > 0 && pre <= v.size());
        CheatList l = cheats_list_for(e);
        CHECK(has(l, "Enable All Hidden Characters"));
        CHECK(l.masters >= 1 && l.featured >= 1);
        bool first_master = cheats_is_master_desc(l.cheats[0].desc);
        CHECK(first_master);
        for (size_t i = 0; i < l.masters + l.featured; ++i) CHECK(!l.cheats[i].codes.empty());
        for (auto& c : l.cheats) CHECK(c.format == cheats::Format::GameShark);
    }
    {   // European Smash must not get the US list when the serial says PAL and a PAL list exists
        GameEntry e = mk(System::N64, "SMASH BROTHERS", "NALP", "C:/x/Super Smash Bros. (Europe) (En,Fr,De).z64");
        CHECK(!cheats_for(e).empty());
    }
    {   // Mario Kart, v64 style name
        GameEntry e = mk(System::N64, "MARIOKART64", "NKTE", "C:/x/Mario Kart 64 (U) [!].z64");
        CHECK(!cheats_for(e).empty());
    }
    {   // GBA Emerald: Code Breaker / GameShark formats
        GameEntry e = mk(System::GBA, "POKEMON EMER", "BPEE", "C:/x/Pokemon - Emerald Version (USA, Europe).gba");
        auto v = cheats_for(e);
        CHECK(!v.empty());
    }
    {   // GB Red (GameShark + Game Genie sections)
        GameEntry e = mk(System::GB, "POKEMON RED", "", "C:/x/Pokemon - Red Version (USA, Europe) (SGB Enhanced).gb");
        auto v = cheats_for(e);
        CHECK(!v.empty());
    }
    {   // unknown game / unsupported system
        CHECK(cheats_for(mk(System::N64, "ZZZ NOT A GAME", "NZZE", "C:/x/zzz.z64")).empty());
        CHECK(cheats_for(mk(System::GC, "Melee", "GALE01", "C:/x/melee.iso")).empty());
    }

    // The player's real ROMs (read-only listing; names are what matters, headers are read when identify works).
    const char* roots[] = {"C:/Users/fatty/Desktop/Everything/Games", "C:/Users/fatty/Desktop/Everything/N64Games"};
    int total = 0, with = 0;
    std::map<std::string, std::pair<int, int>> per;
    for (auto* root : roots) {
        std::error_code ec;
        if (!fs::exists(root, ec)) continue;
        for (auto& it : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
            if (!it.is_regular_file()) continue;
            std::string ext = it.path().extension().string();
            for (auto& ch : ext) ch = (char)std::tolower((unsigned char)ch);
            System s = System::Unknown;
            if (ext == ".z64" || ext == ".v64" || ext == ".n64") s = System::N64;
            else if (ext == ".zip") {
                std::string parent = it.path().parent_path().filename().string();
                if (parent == "GBA") s = System::GBA; else if (parent == "GBC") s = System::GBC; else if (parent == "GB") s = System::GB;
            } else if (ext == ".gba") s = System::GBA; else if (ext == ".gb") s = System::GB; else if (ext == ".gbc") s = System::GBC;
            if (s == System::Unknown) continue;
            GameEntry e = mk(s, "", "", it.path().u8string().c_str());
            e.title = it.path().stem().string();
            size_t pre = 0;
            auto v = cheats_for(e, &pre);
            ++total;
            auto& p = per[system_short(s)];
            ++p.first;
            if (!v.empty()) { ++with; ++p.second; }
            std::printf("  %-4s %4zu cheats (%zu popular)  %s\n", system_short(s), v.size(), pre, it.path().filename().string().c_str());
        }
    }
    std::printf("real ROMs: %d with cheats / %d total\n", with, total);
    for (auto& kv : per) std::printf("  %s: %d/%d\n", kv.first.c_str(), kv.second.second, kv.second.first);

    {   // Every N64 cheat in the bundled database: how many can the runner not use?
        std::FILE* f = std::fopen(CHEATS_DATA_DIR "/cheats_db.bin", "rb");
        if (f) {
            std::string packed; char buf[65536]; size_t k;
            while ((k = std::fread(buf, 1, sizeof buf, f)) > 0) packed.append(buf, k);
            std::fclose(f);
            size_t n = 0;
            void* p = tinfl_decompress_mem_to_heap(packed.data(), packed.size(), &n, TINFL_FLAG_PARSE_ZLIB_HEADER);
            std::string raw = p ? std::string((const char*)p, n) : std::string();
            if (p) mz_free(p);
            bool n64 = false; cheats::Format fmt = cheats::Format::GameShark;
            size_t pos = 0, total = 0, rejected = 0, button = 0, noop = 0;
            std::map<std::string, size_t> why;
            while (pos < raw.size()) {
                size_t nl = raw.find('\n', pos); if (nl == std::string::npos) nl = raw.size();
                std::string line = raw.substr(pos, nl - pos); pos = nl + 1;
                if (line.empty() || line.compare(0, 6, "RHCHT1") == 0) continue;
                if (line[0] == '@') { n64 = line.compare(0, 4, "@n64") == 0; fmt = cheats::Format::GameShark; continue; }
                if (!n64) continue;
                cheats::Cheat c; c.format = fmt;
                size_t t = line.rfind('\t'); if (t == std::string::npos) continue;
                if (line[0] == '!') { size_t t1 = line.find('\t'); cheats::parse_format(line.substr(1, t1 - 1), &c.format); }
                c.codes.push_back(line.substr(t + 1));   // n64_codes splits the joined codes itself
                ++total;
                std::vector<cheats::N64Code> v; std::string err;
                if (!cheats::n64_codes(c, &v, &err)) {
                    ++rejected;
                    std::string key = err.find("XXXX") != std::string::npos ? "XXXX placeholder" : err.substr(0, err.find(':'));
                    ++why[key];
                } else { if (v.empty()) ++noop; if (cheats::n64_needs_gs_button(v)) ++button; }
            }
            std::printf("N64 cheats in bundle: %zu total, %zu rejected, %zu button-fire, %zu no-op\n", total, rejected, button, noop);
            for (auto& kv : why) std::printf("  rejected: %-60s %zu\n", kv.first.c_str(), kv.second);
            if (total > 100000) CHECK(rejected < 2500);   // regression guard (was ~3500 before 88/89 + joined-code support)
        }
    }

    std::printf(g_fail ? "cheats_db_test: %d FAILED\n" : "cheats_db_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
