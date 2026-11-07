// QA-only tool (not a ctest: it downloads the real pinned Lichii release from github.com). Runs RetroHub's real Lichii install / in-place
// upgrade job against <emulators root>\lichii and reports what happened. Nothing is launched and no window opens.
// Usage: hub_lichii_upgrade_qa <emulators root>      (the folder that holds lichii\, e.g. a temp copy of <data dir>\emulators)
// Exit: 0 installed / upgraded (or already current), 1 the job failed (the old build must still be there), 2 usage.
#include "emulators.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: hub_lichii_upgrade_qa <emulators root>\n"); return 2; }
    const std::string root = argv[1];
    const hubemu::Release& rel = hubemu::release(hubemu::Kind::Lichii);
    std::printf("pin: %s size %llu sha256 %s\n", rel.version, (unsigned long long)rel.size, rel.sha256);
    hubemu::Install before = hubemu::find(hubemu::Kind::Lichii, "", "", root);
    std::printf("before: exe=%s managed=%d outdated=%d how=%s\n", before.exe.c_str(), (int)before.managed, (int)before.outdated, before.how.c_str());
    std::string err;
    int last = -1;
    const bool ok = hubemu::install_blocking(hubemu::Kind::Lichii, root, &err, [&](int ph, uint64_t got, uint64_t total) {
        if (ph != last) { last = ph; std::printf("  phase %d (%llu / %llu bytes)\n", ph, (unsigned long long)got, (unsigned long long)total); }
    });
    hubemu::Install after = hubemu::find(hubemu::Kind::Lichii, "", "", root);
    std::printf("result: %s%s\n", ok ? "OK" : "FAILED: ", ok ? "" : err.c_str());
    std::printf("after: exe=%s managed=%d outdated=%d how=%s\n", after.exe.c_str(), (int)after.managed, (int)after.outdated, after.how.c_str());
    return ok ? 0 : 1;
}
