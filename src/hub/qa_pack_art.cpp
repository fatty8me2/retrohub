// Cover-art coverage check (read-only; never downloads game files, only thumbnail images + DATs into <cache_dir>).
//   qa_pack_art <cache_dir> [--library <library_cache.json>] [--no-pack] [--extra <links>]
// Loads the REAL shared-pack index (community list from GitHub, Drive folder listings), runs the hub's ghost art
// resolution for every pack game, and for every library title the owned-game resolution, then fetches the thumbnail.
// Output: one TSV line per game  (src sys status how name | title | hint | url)  and a summary.
#include "art.h"
#include "art_match.h"
#include "gamepack.h"
#include "json.h"
#include "library.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
using namespace hubgp;

struct Item { std::string src, sys, title, hint, key, cid, serial, path; };
struct Res { std::string status, how, name, url; };

static Res run(const Item& it) {
    Res r;
    std::string name, how;
    bool ok;
    if (it.src == "pack") {
        ok = hubart::detail::resolve_ghost(it.sys, "", "", it.title, it.hint, &name, &how);
    } else {
        hub::GameEntry g;
        g.system = hub::system_from_short(it.sys);
        g.key = it.key; g.title = it.title; g.catalog_id = it.cid; g.serial = it.serial; g.path = it.path;
        ok = hubart::detail::resolve_name(g, &name, &how);
    }
    if (!ok) { r.status = "NONAME"; return r; }
    r.name = name; r.how = how;
    auto urls = hubart_match::thumb_urls(it.sys, 0, name);
    r.url = urls.empty() ? "" : urls.front();
    int w = 0, h = 0; std::string err, png;
    if (hubart::detail::fetch_decode(it.sys, name, hubart::Kind::Boxart, &w, &h, &err, &png)) r.status = "OK";
    else r.status = err.find("404") != std::string::npos || err.find("known miss") != std::string::npos ? "NOART" : "ERR:" + err;
    return r;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: qa_pack_art <cache_dir> [--library file] [--no-pack] [--extra links]\n"); return 2; }
    std::string cache = argv[1], lib, extra;
    bool want_pack = true;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--library") && i + 1 < argc) lib = argv[++i];
        else if (!std::strcmp(argv[i], "--extra") && i + 1 < argc) extra = argv[++i];
        else if (!std::strcmp(argv[i], "--no-pack")) want_pack = false;
    }
    hubart::detail::init_headless(cache);
    std::vector<Item> items;
    if (want_pack) {
        index_refresh(parse_links(extra));
        unsigned last_epoch = ~0u; int stable = 0;   // done = nothing busy and no new game for 12 s (community packs list after the built-in ones)
        for (int i = 0; i < 6000 && stable < 24; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            bool busy = !index_last_attempt_ms();
            for (const char* s : {"n64", "gc", "gba", "gbc", "nds", "ps1", "wiiu", "3ds", "ps2", "nes", "snes"}) if (index_state(s) == IndexState::Checking) busy = true;
            unsigned e = index_epoch();
            stable = (e == last_epoch && !busy) ? stable + 1 : 0;
            last_epoch = e;
        }
        for (const auto& g : index_games()) {
            Item it; it.src = "pack"; it.sys = g.sys; it.title = g.title; it.hint = g.hint; it.key = g.key;
            items.push_back(it);
        }
        index_shutdown();
        std::fprintf(stderr, "pack games: %zu\n", items.size());
    }
    if (!lib.empty()) {
        std::ifstream f(lib, std::ios::binary);
        std::stringstream ss; ss << f.rdbuf();
        json::Value root;
        if (!json::parse(ss.str(), &root)) { std::fprintf(stderr, "bad library json\n"); return 2; }
        const json::Value* files = root.get("files");
        std::map<std::string, bool> seen;
        if (files) for (const auto& e : files->a) {
            Item it; it.src = "lib"; it.sys = e.str("sys"); it.title = e.str("t"); it.cid = e.str("c"); it.key = e.str("k");
            it.serial = e.str("sn"); it.path = e.str("p");
            if (it.sys.empty() || it.key.empty() || !seen.emplace(it.key, true).second) continue;
            items.push_back(it);
        }
    }
    std::vector<Res> res(items.size());
    std::atomic<size_t> next{0};
    std::vector<std::thread> th;
    for (int t = 0; t < 6; ++t) th.emplace_back([&] { for (size_t i; (i = next++) < items.size();) res[i] = run(items[i]); });
    for (auto& t : th) t.join();
    std::map<std::string, std::map<std::string, int>> cnt;   // "src sys" -> status -> n
    for (size_t i = 0; i < items.size(); ++i) {
        const Item& it = items[i]; const Res& r = res[i];
        std::printf("%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", it.src.c_str(), it.sys.c_str(), r.status.c_str(), r.how.c_str(), r.name.c_str(),
                    it.title.c_str(), it.hint.c_str(), r.url.c_str());
        ++cnt[it.src + " " + it.sys][r.status.compare(0, 3, "ERR") == 0 ? "ERR" : r.status];
    }
    int tot = 0, ok = 0;
    for (auto& c : cnt) {
        int n = 0; for (auto& s : c.second) n += s.second;
        tot += n; ok += c.second["OK"];
        std::fprintf(stderr, "%-10s total=%d ok=%d noname=%d noart=%d err=%d\n", c.first.c_str(), n, c.second["OK"], c.second["NONAME"], c.second["NOART"], c.second["ERR"]);
    }
    std::fprintf(stderr, "ALL total=%d ok=%d missing=%d\n", tot, ok, tot - ok);
    return 0;
}
