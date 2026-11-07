/* hub_metadata_prefetch <userdata dir> [--limit N] [--no-snaps] [--only <system>] [--ghosts | --ghosts-only] [--friends <file>] [--audit-tsv <out.tsv>]
 *
 * --ghosts       also covers the friends' ghosts of <dir>\friend_libs.json, built the way the Cases view builds them (fg::build_ghosts: owned games
 *                and their clean No-Intro aliases are removed, same-title friends merged); front cover + full cover scan, no screenshots / description.
 * --pack <tsv>  adds the pack-only ghosts from a pack index file (sys, title, key, hint per line); --pack-live <tsv> lists the shared Drive
 *                folders once (what the app does at start-up), saves the index to that file and uses it.
 * --ghosts-only  only the ghosts. --audit-tsv writes the same table the app's HUB_CASES_AUDIT hook writes (cases_audit.h) for every row covered.
 *
 * Headless coverage tool for cover art + descriptions. Reads <dir>\library_cache.json and, for every game in it, fetches INTO <dir>:
 *   art\<sys>\boxart|title|snap\   (libretro-thumbnails, GameTDB front cover as fallback, Switch .nup icon)
 *   art\<sys>\coverfull\           (GameTDB full cover scans: GameCube / Wii U / DS / 3DS)
 *   gameinfo\<sys>_<title>.json    (Wikidata + Wikipedia description, GameTDB synopsis as the fallback)
 * and prints a per-game coverage table plus totals. Same code paths as the app's background workers (hubart::detail::fetch_game,
 * gameinfo::resolve_description), run one request at a time with the same politeness (GameTDB >= 0.5 s, Wikimedia >= 1 s, 429 honoured).
 * NEVER point it at a folder you do not want written to: copy the user data first. Not a ctest (live network). */
#include "art.h"
#include "cases_audit.h"
#include "friend_games.h"
#include "gamepack.h"
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
#include <optional>
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

/* The tool runs on one thread: when Wikipedia had no description it may download the system's GameTDB database (once, then it is cached). */
std::optional<gametdb::Rec> tdb_find(const gameinfo::Query& q) {
    if (gametdb::db_name(q.system).empty()) return std::nullopt;
    const gametdb::Index* idx = gametdb::index_for(q.system, [](const std::string& u, std::string* b, int* st) { return gameinfo_net::http_get(u, b, st); });
    if (!idx) return std::nullopt;
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
    hubart::detail::flush();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: hub_metadata_prefetch <userdata dir> [--limit N] [--no-snaps] [--only <system>] [--ghosts|--ghosts-only] [--pack <tsv>|--pack-live <tsv>] [--friends <friend_libs.json>] [--audit-tsv <out>]\n"); return 2; }
    std::string udir = argv[1];
    if (udir.back() != '/' && udir.back() != '\\') udir += '/';
    int limit = 1 << 30;
    bool snaps = true, tdb_warm = false, want_ghosts = false, ghosts_only = false;
    std::string only, audit_tsv, friends_file, pack_file, pack_live;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--limit" && i + 1 < argc) limit = std::atoi(argv[++i]);
        else if (a == "--no-snaps") snaps = false;
        else if (a == "--tdb") tdb_warm = true;   // download / load the five GameTDB indexes, print their sizes, exit
        else if (a == "--only" && i + 1 < argc) only = argv[++i];
        else if (a == "--ghosts") want_ghosts = true;
        else if (a == "--ghosts-only") want_ghosts = ghosts_only = true;
        else if (a == "--pack" && i + 1 < argc) pack_file = argv[++i];
        else if (a == "--pack-live" && i + 1 < argc) pack_live = argv[++i];
        else if (a == "--friends" && i + 1 < argc) friends_file = argv[++i];
        else if (a == "--audit-tsv" && i + 1 < argc) audit_tsv = argv[++i];
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
    std::vector<hubui::cases_audit::Row> audit;
    auto slot = [](bool ok, const std::string& name, const std::string& err) {
        hubart::SlotInfo si;
        si.state = ok ? hubart::SlotState::Ready : hubart::SlotState::Failed;
        si.name = name; si.err = err;
        si.transient = err.find("(transient)") != std::string::npos;
        return si;
    };
    int n = 0;
    if (ghosts_only) games.clear();
    for (const hub::GameEntry& e : games) {
        Row r;
        r.sys = hub::system_short(e.system);
        r.title = e.title;
        r.key = e.key;
        std::fprintf(stderr, "[%d/%zu] %s %s\n", ++n, games.size(), r.sys.c_str(), r.title.c_str());
        int w = 0, h = 0;
        std::string err, how;
        std::string aname, fname, ferr;
        r.box = hubart::detail::fetch_game(e, hubart::Kind::Boxart, &w, &h, &err, &how, nullptr, &aname) ? 'Y' : 'N';
        r.box_how = how;
        if (r.box == 'N') r.note = "boxart: " + err;
        if (gametdb::has_fullcover(r.sys)) {
            std::string e2;
            r.full = hubart::detail::fetch_game(e, hubart::Kind::FullCover, &w, &h, &e2, nullptr, nullptr, &fname) ? 'Y' : 'N';
            ferr = e2;
            if (r.full == 'N') r.note += (r.note.empty() ? "" : "; ") + std::string("full: ") + e2;
        }
        if (snaps && !is_hack(e) && r.sys != "switch") {
            std::string e3;
            r.ttl = hubart::detail::fetch_game(e, hubart::Kind::Title, &w, &h, &e3, nullptr, nullptr) ? 'Y' : 'N';
            r.snap = hubart::detail::fetch_game(e, hubart::Kind::Snap, &w, &h, &e3, nullptr, nullptr) ? 'Y' : 'N';
        }
        if (describe(e, udir, &r) != 0) { std::this_thread::sleep_for(std::chrono::seconds(5)); describe(e, udir, &r); }
        rows.push_back(r);
        {
            hubui::cases_audit::Item it;
            it.section = "own"; it.sys = r.sys; it.title = e.title; it.key = e.key; it.cid = e.catalog_id; it.entry = e;
            audit.push_back(hubui::cases_audit::make_row((int)audit.size(), it, slot(r.box == 'Y', aname, err), slot(r.full == 'Y', fname, ferr)));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (want_ghosts && lib.get("files")) {   // friends' ghosts, as the Cases view builds them
        std::string ftext;
        std::vector<hubui::fg::Owned> mine;
        {
            std::set<std::string> seen2;
            for (const json::Value& f : lib.get("files")->a) {
                if (!f.boolean("ok")) continue;
                hub::GameEntry e;
                e.key = f.str("k"); e.system = hub::system_from_short(f.str("sys")); e.title = f.str("t"); e.catalog_id = f.str("c");
                if (e.key.empty() || e.system == hub::System::Unknown || !seen2.insert(e.key).second || is_hack(e)) continue;
                const std::string sy = hub::system_short(e.system);
                mine.push_back({sy, e.key, e.catalog_id, e.title});
                std::string nm = hubui::fg::clean_dat_name(hubart::cached_name(e.key));
                if (!nm.empty()) mine.push_back({sy, e.key, e.catalog_id, nm});
            }
        }
        const std::string pack_root = udir + "gamepack\\";
        for (const json::Value& f : lib.get("files")->a) {   // installed from a shared pack / filed under a pack console folder: also owned under those names
            if (!f.boolean("ok")) continue;
            hub::GameEntry e;
            e.key = f.str("k"); e.system = hub::system_from_short(f.str("sys")); e.title = f.str("t"); e.catalog_id = f.str("c");
            e.path = f.str("lp").empty() ? f.str("p") : f.str("lp");
            if (e.key.empty() || e.system == hub::System::Unknown || is_hack(e)) continue;
            const std::string t = hubgp::installed_folder_title(e.path, pack_root);
            if (!t.empty()) mine.push_back({hub::system_short(e.system), e.key, e.catalog_id, t});
            for (const auto& o : hubgp::pack_owned_aliases(e, pack_root)) mine.push_back(o);
        }
        const std::string fpath = friends_file.empty() ? udir + "friend_libs.json" : friends_file;
        std::vector<std::pair<std::string, const std::vector<hubnet::LibGame>*>> libs;
        std::vector<std::pair<std::string, std::vector<hubnet::LibGame>>> store;
        if (winutil::read_file(fpath, &ftext)) store = hubui::cases_audit::parse_friend_libs(ftext);
        for (auto& s2 : store) libs.push_back({s2.first, &s2.second});
        std::vector<hubui::fg::Ghost> ghosts = hubui::fg::build_ghosts(mine, libs, [](const std::string&, const std::string& key) { return hubart::cached_name(key); });
        const size_t friend_n = ghosts.size();
        if (!pack_file.empty() || !pack_live.empty()) {
            std::vector<hubgp::PackGame> pg;
            std::string ptext;
            if (!pack_live.empty()) {   // the same listing the app starts in the background
                hubgp::index_refresh({});
                for (int i = 0; i < 240; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    bool busy = false;
                    for (const char* c : {"n64", "gc", "gba", "gbc", "nds", "ps1"}) if (hubgp::index_state(c) == hubgp::IndexState::Checking) busy = true;
                    if (!busy && i > 2) break;
                }
                pg = hubgp::index_games();
                std::vector<hubui::cases_audit::PackRow> rows2;
                for (const auto& g : pg) rows2.push_back({g.sys, g.title, g.key, g.hint});
                winutil::write_file_atomic(pack_live, hubui::cases_audit::pack_tsv(rows2));
            } else if (winutil::read_file(pack_file, &ptext)) {
                for (const auto& r : hubui::cases_audit::parse_pack_tsv(ptext)) pg.push_back({r.sys, r.title, r.key, r.hint});
            }
            std::vector<hubui::fg::Ghost> pk = hubgp::pack_only_ghosts(mine, ghosts, pg, hubui::fg::kMaxGhosts - ghosts.size());
            ghosts.insert(ghosts.end(), pk.begin(), pk.end());
            std::fprintf(stderr, "pack index: %zu games, %zu pack-only ghosts\n", pg.size(), pk.size());
        }
        std::fprintf(stderr, "ghosts: %zu from %zu friend lists (+%zu pack)\n", ghosts.size(), store.size(), ghosts.size() - friend_n);
        int gi = 0;
        for (const hubui::fg::Ghost& g : ghosts) {
            if (!only.empty() && g.sys != only) continue;
            std::fprintf(stderr, "[ghost %d/%zu] %s %s\n", ++gi, ghosts.size(), g.sys.c_str(), g.title.c_str());
            Row r;
            r.sys = g.sys; r.title = g.title; r.key = g.key + " (friend)";
            int w = 0, h = 0;
            std::string err, aname, fname, ferr;
            r.box = hubart::detail::fetch_ghost(g.sys, g.key, g.cid, g.title, g.hint, hubart::Kind::Boxart, &w, &h, &err, &aname) ? 'Y' : 'N';
            r.box_how = aname;
            if (r.box == 'N') r.note = "boxart: " + err;
            if (gametdb::has_fullcover(g.sys)) {
                r.full = hubart::detail::fetch_ghost(g.sys, g.key, g.cid, g.title, g.hint, hubart::Kind::FullCover, &w, &h, &ferr, &fname) ? 'Y' : 'N';
                if (r.full == 'N') r.note += (r.note.empty() ? "" : "; ") + std::string("full: ") + ferr;
            }
            r.desc = "-";
            rows.push_back(r);
            hubui::cases_audit::Item it;
            it.section = g.uids.empty() ? "pack" : "friend"; it.sys = g.sys; it.title = g.title; it.key = g.key; it.cid = g.cid; it.hint = g.hint; it.ghost = true;
            audit.push_back(hubui::cases_audit::make_row((int)audit.size(), it, slot(r.box == 'Y', aname, err), slot(r.full == 'Y', fname, ferr)));
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }
    if (!audit_tsv.empty()) {
        std::string text = hubui::cases_audit::tsv_header();
        for (const auto& r : audit) text += hubui::cases_audit::tsv_row(r);
        winutil::write_file_atomic(audit_tsv, text);
        std::printf("audit: %s\n", hubui::cases_audit::summary(audit).c_str());
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
    hubart::detail::flush();
    return 0;
}
