// QA driver for GamePack against a local fake (RETROHUB_QA=1 RETROHUB_GP_BASE=127.0.0.1:<port>). Not a ctest.
//   qa_gamepack index  <links> <sys> <title>...      -> prints refresh latency, state, index_has per title
//   qa_gamepack install <links> <sys> <title> [cancel_after_bytes]
#include "gamepack.h"
#include "library.h"
#include "games.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>
using namespace hubgp;
namespace fs = std::filesystem;
static long long ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "index-list") {   // read-only: index with no extra links (+ optional links in argv[2]), print every console / display title
        auto roots = parse_links(argc > 2 ? argv[2] : "");
        index_refresh(roots);
        for (int i = 0; i < 1200; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); if (i > 5 && index_last_attempt_ms() && index_state("n64") != IndexState::Checking && index_state("ps1") != IndexState::Checking && index_state("gc") != IndexState::Checking && index_state("gba") != IndexState::Checking && index_state("gbc") != IndexState::Checking && index_state("nds") != IndexState::Checking) break; }
        for (int i = 0; i < 50; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto games = index_games();
        std::string cur;
        for (const auto& g : games) { if (g.sys != cur) { cur = g.sys; std::printf("== %s\n", cur.c_str()); } std::printf("  %s   [%s]\n", g.title.c_str(), g.key.c_str()); }
        std::printf("total=%zu epoch=%u\n", games.size(), index_epoch());
        index_shutdown();
        return 0;
    }
    if (argc >= 5 && std::string(argv[1]) == "e2e") {   // install -> scan pack_root -> same retitle/owned rules as the hub -> is the ghost gone?  e2e <links> <sys> <title>
        auto roots = parse_links(argv[2]);
        std::string sys = argv[3], title = argv[4];
        hub::library_set_data_dir(hub::data_dir());
        index_refresh(roots);
        for (int i = 0; i < 400 && index_state(sys) != IndexState::Ready; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto has_ghost = [&](const std::vector<hubui::fg::Owned>& mine) {
            for (const auto& g : pack_only_ghosts(mine, {}, index_games(), 1000)) if (g.sys == sys && pack_key(g.title) == pack_key(title)) return true;
            return false;
        };
        std::printf("ghost_before=%d\n", (int)has_ghost({}));
        Installer in; in.start(sys, title, roots);
        bool fin = false, ok = false;
        while (!fin) { fin = in.poll(&ok); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        std::printf("install ok=%d err=[%s]\n", (int)ok, in.error.c_str());
        auto games = hub::scan_library({pack_root()}, nullptr);
        std::vector<hubui::fg::Owned> mine;
        bool found = false;
        for (auto& e : games) {
            std::string t = installed_folder_title(e.path, pack_root());
            std::printf("entry sys=%s key=%s title=[%s] folder=[%s] issue=%s\n", hub::system_short(e.system), e.key.c_str(), e.title.c_str(), t.c_str(), e.issue.c_str());
            if (e.catalog_id.empty() && !t.empty() && pack_key(t) != pack_key(e.title)) e.title = t;
            mine.push_back({hub::system_short(e.system), e.key, e.catalog_id, e.title});
            if (std::string(hub::system_short(e.system)) == sys) found = true;
            if (e.issue == "extract") {
                std::string path, err;
                bool x = !hub::extracted_3ds_path(e).empty() || hub::extract_3ds_zip(e, nullptr, &path, &err);
                std::printf("unpack ok=%d %s\n", (int)x, err.c_str());
            }
        }
        std::printf("scan_found=%d ghost_after=%d\n", (int)found, (int)has_ghost(mine));
        index_shutdown();
        return 0;
    }
    if (argc < 5) return 2;
    std::string mode = argv[1], links = argv[2], sys = argv[3];
    auto roots = parse_links(links);
    if (mode == "index") {
        long long t0 = ms(); index_refresh(roots); std::printf("refresh_call_ms=%lld\n", ms() - t0);
        for (int i = 0; i < 400 && index_state(sys) != IndexState::Ready; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::printf("state=%d after_ms=%lld source=%d\n", (int)index_state(sys), ms() - t0, (int)index_source(sys));
        for (int i = 4; i < argc; ++i) std::printf("has[%s]=%d\n", argv[i], (int)index_has(sys, argv[i]));
        index_shutdown();
        return 0;
    }
    std::string title = argv[4];
    uint64_t cancel_at = argc > 5 ? std::strtoull(argv[5], nullptr, 10) : 0;
    Installer in; long long t0 = ms(); in.start(sys, title, roots);
    long long maxpoll = 0; bool ok = false, cancelled = false;
    for (;;) {
        long long a = ms(); bool s = false; bool fin = in.poll(&s); maxpoll = std::max(maxpoll, ms() - a);
        if (fin) { ok = s; break; }
        if (cancel_at && !cancelled && in.got.load() >= cancel_at) { in.request_cancel(); cancelled = true; std::printf("cancel requested at got=%llu\n", (unsigned long long)in.got.load()); }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::printf("ok=%d err=[%s] ms=%lld max_poll_ms=%lld installed=%d\n", (int)ok, in.error.c_str(), ms() - t0, maxpoll, (int)installed(sys, title));
    std::error_code ec;
    for (auto& e : fs::recursive_directory_iterator(fs::u8path(pack_root()), ec)) std::printf("  %s\n", e.path().u8string().c_str());
    return 0;
}
