/* QA helper for the two-PC save simulation (qa\saves_two_pc.ps1). Console only, not shipped.
 *
 *   qa_saves_tool key  <rom>                      -> game key, title, system short
 *   qa_saves_tool dirs <vault> <rom>               -> battery and states dirs the hub would pass to runners
 *   qa_saves_tool sync <vault> <sync_root> [--states]  -> hub::saves::sync_folder, prints the log
 *
 * Keys and dirs use the same library and savestore calls the hub uses (identify_game, system_short,
 * battery_dir/states_dir), so the simulation matches what the hub computes. */
#include "savestore.h"
#include "games.h"
#include "library.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {
int usage() {
    std::fputs("usage: qa_saves_tool key <rom>\n"
               "       qa_saves_tool dirs <vault> <rom>\n"
               "       qa_saves_tool sync <vault> <sync_root> [--states]\n", stderr);
    return 2;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    std::string cmd = argv[1];

    if (cmd == "smashdir") {   /* qa_saves_tool smashdir <vault>: battery dir the hub gives the native Smash port (n64 / native:smash64) */
        hub::saves::set_vault_dir(argv[2]);
        std::printf("battery=%s\n", hub::saves::battery_dir("n64", "native:smash64").c_str());
        return 0;
    }

    if (cmd == "key" || cmd == "dirs") {
        const char* rom = cmd == "key" ? argv[2] : (argc >= 4 ? argv[3] : nullptr);
        if (!rom) return usage();
        if (cmd == "dirs") hub::saves::set_vault_dir(argv[2]);
        hub::GameEntry e;
        if (!hub::identify_game(rom, &e)) {
            std::fprintf(stderr, "not identified: %s\n", rom);
            return 1;
        }
        std::string sys = hub::system_short(e.system);
        if (cmd == "key") {
            std::printf("key=%s\nsystem=%s\ntitle=%s\npath=%s\n", e.key.c_str(), sys.c_str(), e.title.c_str(), e.path.c_str());
            return 0;
        }
        /* Same key derivation for the runner dirs as the hub: battery_dir/states_dir(system_short, key). */
        std::printf("battery=%s\nstates=%s\n", hub::saves::battery_dir(sys, e.key).c_str(),
                    hub::saves::states_dir(sys, e.key).c_str());
        return 0;
    }

    if (cmd == "sync") {
        if (argc < 4) return usage();
        bool states = argc >= 5 && std::strcmp(argv[4], "--states") == 0;
        hub::saves::set_vault_dir(argv[2]);
        hub::saves::SyncReport r = hub::saves::sync_folder(argv[3], states);
        for (const auto& line : r.log) std::printf("  %s\n", line.c_str());
        std::printf("pushed=%d pulled=%d conflicts=%d ok=%d%s%s\n", r.pushed, r.pulled, r.conflicts, r.ok ? 1 : 0,
                    r.error.empty() ? "" : " error=", r.error.c_str());
        return r.ok ? 0 : 1;
    }
    return usage();
}
