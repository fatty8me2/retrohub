// QA-only tool (not a ctest: it downloads the real pinned release from github.com). Runs RetroHub's real install / in-place
// upgrade job for a Switch emulator against <emulators root>\lichii or <emulators root>\fritchii and reports what happened.
// Nothing is launched and no window opens.
// Usage: hub_lichii_upgrade_qa <emulators root> [lichii|fritchii]     (the folder that holds lichii\ / fritchii\; a TEMP folder, never the real data dir)
// Exit: 0 installed / upgraded (or already current), 1 the job failed (the old build must still be there),
//       2 usage (also: Fritchii's pin is still a placeholder, so there is nothing to download).
#include "emulators.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: hub_lichii_upgrade_qa <emulators root> [lichii|fritchii]\n"); return 2; }
    const std::string root = argv[1];
    const std::string which = argc > 2 ? argv[2] : "lichii";
    if (which != "lichii" && which != "fritchii") { std::printf("unknown emulator '%s' (lichii or fritchii)\n", which.c_str()); return 2; }
    const hubemu::Kind kind = which == "fritchii" ? hubemu::Kind::Fritchii : hubemu::Kind::Lichii;
    if (hubemu::release_is_placeholder(kind)) { std::printf("%s: the pin is still a placeholder (no release published yet)\n", hubemu::name(kind)); return 2; }
    const hubemu::Release& rel = hubemu::release(kind);
    std::printf("%s pin: %s size %llu sha256 %s\n", hubemu::name(kind), rel.version, (unsigned long long)rel.size, rel.sha256);
    hubemu::Install before = hubemu::find(kind, "", "", root);
    std::printf("before: exe=%s managed=%d outdated=%d how=%s\n", before.exe.c_str(), (int)before.managed, (int)before.outdated, before.how.c_str());
    std::string err;
    int last = -1;
    const bool ok = hubemu::install_blocking(kind, root, &err, [&](int ph, uint64_t got, uint64_t total) {
        if (ph != last) { last = ph; std::printf("  phase %d (%llu / %llu bytes)\n", ph, (unsigned long long)got, (unsigned long long)total); }
    });
    hubemu::Install after = hubemu::find(kind, "", "", root);
    std::printf("result: %s%s\n", ok ? "OK" : "FAILED: ", ok ? "" : err.c_str());
    std::printf("after: exe=%s managed=%d outdated=%d how=%s\n", after.exe.c_str(), (int)after.managed, (int)after.outdated, after.how.c_str());
    if (ok && kind == hubemu::Kind::Fritchii) std::printf("leftover updater files: %s\n", hubemu::update_leftovers(kind, after.dir).empty() ? "none" : hubemu::update_leftovers(kind, after.dir).c_str());
    return ok ? 0 : 1;
}
