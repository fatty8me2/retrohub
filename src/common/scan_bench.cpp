/* scan_bench <scratch data dir> <folder>...  : times scan_library twice (cold then warm cache) against a scratch data dir.
 * Read-only on the folders; extracted cartridge ROMs and the scan cache go to the scratch dir.
 * SCAN_BENCH_LAUNCH=1: also prints "launch <sys> <key> <path>" for every game, unpacking archived discs first (as Play does). */
#include "library.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc < 3) { std::puts("usage: scan_bench <scratch data dir> <folder>..."); return 2; }
    hub::library_set_data_dir(argv[1]);
    std::vector<std::string> folders(argv + 2, argv + argc);
    const bool launch = std::getenv("SCAN_BENCH_LAUNCH") != nullptr;
    for (int pass = 0; pass < 2; ++pass) {
        auto t0 = std::chrono::steady_clock::now();
        auto games = hub::scan_library(folders, nullptr);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("%s: %zu games, %.0f ms\n", pass ? "warm" : "cold", games.size(), ms);
        if (pass == 0) {
            for (auto& g : games) std::printf("  %-5s %-40s %s%s\n", hub::system_short(g.system), g.key.c_str(), g.title.c_str(), g.issue.empty() ? "" : (" [" + g.issue + "]").c_str());
            for (auto& s : hub::scan_skipped_archives()) std::printf("  skipped %s: %s\n", s.path.c_str(), s.reason.c_str());
            if (launch)
                for (auto& g : games) {
                    std::string path = g.path, err;
                    if (g.issue == "extract" && hub::extracted_3ds_path(g).empty() && !hub::extract_3ds_zip(g, nullptr, &path, &err)) { std::printf("launch %s %s UNPACK-FAILED %s\n", hub::system_short(g.system), g.key.c_str(), err.c_str()); continue; }
                    if (g.issue == "extract") path = hub::extracted_3ds_path(g);
                    std::printf("launch %s %s %s\n", hub::system_short(g.system), g.key.c_str(), path.c_str());
                }
        }
    }
    return 0;
}
