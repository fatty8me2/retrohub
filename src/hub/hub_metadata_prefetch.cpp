/* hub_metadata_prefetch <userdata dir> [--limit N] [--no-snaps] [--only <system>]
 *
 * Headless coverage tool for cover art + descriptions. Reads <dir>\library_cache.json and, for every game in it, fetches INTO <dir>:
 *   art\<sys>\boxart|title|snap\   (libretro-thumbnails, GameTDB front cover as fallback, Switch .nup icon)
 *   art\<sys>\coverfull\           (GameTDB full cover scans: GameCube / Wii U / DS / 3DS)
 *   gameinfo\<sys>_<title>.json    (Wikidata + Wikipedia description, GameTDB synopsis as the fallback)
 * and prints a per-game coverage table plus totals. Same code paths as the app's background workers (hubart::detail::fetch_game,
 * gameinfo::resolve_description), run one request at a time with the same politeness (GameTDB >= 0.5 s, Wikimedia >= 1 s, 429 honoured).
 * NEVER point it at a folder you do not want written to: copy the user data first. Not a ctest (live network). */
#include "art.h"
#include "gameinfo.h"
#include "gameinfo_net.h"
#include "gametdb.h"
#include "games.h"
#include "json.h"
#include "library.h"
#include "winutil.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <thread>

namespace fs = std::filesystem;

#ifndef TOOL_DATA_DIR
#define TOOL_DATA_DIR "data/"
#endif

namespace {

struct Row {
    std::string sys, title, key;
    char box = '-', full = '-', ttl = '-', snap = '-';
    std::string box_how, desc;   // desc: online / tdb / generated / ERR
    std::string note;
};

bool is_hack(const hub::GameEntry& e) { return e.catalog_id == "remix" || e.catalog_id == "kanto" || !e.hack_patch.empty(); }

const gametdb::Rec* tdb_find(const gameinfo::Query& q) {
    if (gametdb::db_name(q.system).empty()) return nullptr;
    const gametdb::Index* idx = gametdb::index_for(q.system, [](const std::string& u, std::string* b, int* st) { return gameinfo_net::http_get(u, b, st); });
    if (!idx) return nullptr;
    return gametdb::find_record(*idx, q.system, q.serial, gameinfo::title_variants(q));
}

/* 0 = got a result (cached or fetched), 1 = network trouble */
int describe(const hub::GameEntry& e, const std::string& udir, Row* row) {
    gameinfo::Query q;
    q.system = hub::system_short(e.system);
    q.title = e.title;
    q.serial = e.serial;
    const hub::GameInfo* cat = hub::find_game(e.catalog_id);
    q.alt_title = e.catalog_id == "smash64" ? "Super Smash Bros." : (cat ? cat->title : std::string());
    const gameinfo::Meta m = gameinfo::shared_db(TOOL_DATA_DIR).lookup(q);
    if (is_hack(e)) { row->desc = "generated"; return 0; }
    const std::string dir = udir + "gameinfo/";
    const std::string file = dir + gameinfo::cache_name(q);
    gameinfo::Online on;
    std::string txt;
    bool have = winutil::read_file(file, &txt) && gameinfo::cache_from_json(txt, &on);
    if (!have || gameinfo::cache_stale(on, (int64_t)std::time(nullptr))) {
        gameinfo::Online fresh;
        gameinfo::Fetcher f = [](const std::string& u, std::string* b, int* s) { return gameinfo_net::fetch(u, b, s); };
        auto r = gameinfo::resolve_description(q, f, tdb_find, &fresh);
        if (r == gameinfo::ResolveResult::NetworkError) { if (!have) { row->desc = "ERR"; return 1; } }
        else {
            fresh.fetched = (int64_t)std::time(nullptr);
            fresh.rev = gameinfo::kCacheRev;
            std::error_code ec;
            fs::create_directories(fs::u8path(dir), ec);
            winutil::write_file_atomic(file, gameinfo::cache_to_json(fresh));
            on = fresh;
        }
    }
    if (!on.description.empty()) row->desc = on.source == "GameTDB" ? "tdb" : "online";
    else {
        row->desc = "generated";
        row->note = gameinfo::generated_description(q, m, on, e.nx2);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: hub_metadata_prefetch <userdata dir> [--limit N] [--no-snaps] [--only <system>]\n"); return 2; }
    std::string udir = argv[1];
    if (udir.back() != '/' && udir.back() != '\\') udir += '/';
    int limit = 1 << 30;
    bool snaps = true, tdb_warm = false;
    std::string only;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--limit" && i + 1 < argc) limit = std::atoi(argv[++i]);
        else if (a == "--no-snaps") snaps = false;
        else if (a == "--tdb") tdb_warm = true;   // download / load the five GameTDB indexes, print their sizes, exit
        else if (a == "--only" && i + 1 < argc) only = argv[++i];
    }
    std::string text;
    json::Value lib;
    if (!winutil::read_file(udir + "library_cache.json", &text) || !json::parse(text, &lib) || !lib.get("files")) {
        std::printf("cannot read %slibrary_cache.json\n", udir.c_str());
        return 1;
    }
    hubart::detail::init_headless(udir + "art/");
    if (tdb_warm) {
        for (const char* sy : {"gc", "wiiu", "nds", "3ds", "switch"}) {
            auto t0 = std::chrono::steady_clock::now();
            const gametdb::Index* idx = gametdb::index_for(sy, [](const std::string& u, std::string* b, int* st) { return gameinfo_net::http_get(u, b, st); });
            std::printf("%-6s %s %zu records, %lld ms\n", sy, idx ? "ok" : "UNAVAILABLE", idx ? idx->size() : (size_t)0,
                        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        return 0;
    }
    std::vector<hub::GameEntry> games;
    std::set<std::string> seen;
    for (const json::Value& f : lib.get("files")->a) {
        if (!f.boolean("ok")) continue;
        hub::GameEntry e;
        e.key = f.str("k");
        e.system = hub::system_from_short(f.str("sys"));
        if (e.key.empty() || e.system == hub::System::Unknown || !seen.insert(e.key).second) continue;
        e.title = f.str("t");
        e.serial = f.str("sn");
        e.catalog_id = f.str("c");
        e.path = f.str("lp").empty() ? f.str("p") : f.str("lp");
        e.nx2 = f.boolean("nx2");
        if (!only.empty() && hub::system_short(e.system) != only) continue;
        games.push_back(e);
    }
    std::sort(games.begin(), games.end(), [](const hub::GameEntry& a, const hub::GameEntry& b) {
        std::string sa = hub::system_short(a.system), sb = hub::system_short(b.system);
        return sa != sb ? sa < sb : a.title < b.title;
    });
    if ((int)games.size() > limit) games.resize((size_t)limit);

    std::vector<Row> rows;
    int n = 0;
    for (const hub::GameEntry& e : games) {
        Row r;
        r.sys = hub::system_short(e.system);
        r.title = e.title;
        r.key = e.key;
        std::fprintf(stderr, "[%d/%zu] %s %s\n", ++n, games.size(), r.sys.c_str(), r.title.c_str());
        int w = 0, h = 0;
        std::string err, how;
        r.box = hubart::detail::fetch_game(e, hubart::Kind::Boxart, &w, &h, &err, &how, nullptr) ? 'Y' : 'N';
        r.box_how = how;
        if (r.box == 'N') r.note = "boxart: " + err;
        if (gametdb::has_fullcover(r.sys)) {
            std::string e2;
            r.full = hubart::detail::fetch_game(e, hubart::Kind::FullCover, &w, &h, &e2, nullptr, nullptr) ? 'Y' : 'N';
            if (r.full == 'N') r.note += (r.note.empty() ? "" : "; ") + std::string("full: ") + e2;
        }
        if (snaps && !is_hack(e) && r.sys != "switch") {
            std::string e3;
            r.ttl = hubart::detail::fetch_game(e, hubart::Kind::Title, &w, &h, &e3, nullptr, nullptr) ? 'Y' : 'N';
            r.snap = hubart::detail::fetch_game(e, hubart::Kind::Snap, &w, &h, &e3, nullptr, nullptr) ? 'Y' : 'N';
        }
        if (describe(e, udir, &r) != 0) { std::this_thread::sleep_for(std::chrono::seconds(5)); describe(e, udir, &r); }
        rows.push_back(r);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    std::printf("\n%-6s %-52s %-3s %-4s %-5s %-5s %-9s %s\n", "sys", "title", "box", "full", "title", "snap", "desc", "box source / note");
    std::map<std::string, std::array<int, 6>> tot;   // box full(applicable) full(ok) desc(any) total snaps
    for (const Row& r : rows) {
        std::printf("%-6s %-52.52s %-3c %-4c %-5c %-5c %-9s %s%s%s\n", r.sys.c_str(), r.title.c_str(), r.box, r.full, r.ttl, r.snap, r.desc.c_str(),
                    r.box_how.c_str(), r.note.empty() ? "" : "  | ", r.note.c_str());
        auto& t = tot[r.sys];
        t[4]++;
        if (r.box == 'Y') t[0]++;
        if (r.full != '-') t[1]++;
        if (r.full == 'Y') t[2]++;
        if (r.desc == "online" || r.desc == "tdb" || r.desc == "generated") t[3]++;
    }
    std::printf("\n%-6s %5s %9s %11s %12s\n", "sys", "games", "boxart", "fullcover", "description");
    int g = 0, b = 0, fa = 0, fo = 0, d = 0, online = 0;
    for (const auto& kv : tot) {
        const auto& t = kv.second;
        char fc[24];
        if (t[1]) std::snprintf(fc, sizeof fc, "%d/%d", t[2], t[1]); else std::snprintf(fc, sizeof fc, "n/a");
        std::printf("%-6s %5d %6d/%-3d %11s %9d/%-3d\n", kv.first.c_str(), t[4], t[0], t[4], fc, t[3], t[4]);
        g += t[4]; b += t[0]; fa += t[1]; fo += t[2]; d += t[3];
    }
    for (const Row& r : rows) if (r.desc == "online" || r.desc == "tdb") ++online;
    std::printf("TOTAL  %5d %6d/%-3d %8d/%-3d %9d/%-3d   (real description: %d, generated: %d)\n", g, b, g, fo, fa, d, g, online, d - online);
    return 0;
}
