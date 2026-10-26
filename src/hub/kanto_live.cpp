/* Manual live driver (not a ctest): installs the user's Kanto Together zip into a scratch dir, builds the game from a FireRed ROM, then runs a
 * local relay plus a host and a guest launcher (kanto::Session, mGBA muted) all on a private invisible desktop and waits for
 * ">>> Your partner is connected. <<<" on both sides. Optionally exercises a trade offer round trip.
 * usage: kanto_live <package zip|folder> <scratch dir> <FireRed.gba|stub> <seconds> [relay.mjs] [desktop name]
 * "stub" instead of a FireRed: install only and use a 16 MiB zero ROM (mGBA + coop.lua start, but there is no game: for connect/saves tests).
 * Saves go through a scratch vault (<scratch>\vault) with kanto::stage_saves / flush_saves per role, like the hub does. The scratch dir must not be under %TEMP%. */
#include <winsock2.h>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "kanto.h"
#include "winutil.h"

namespace fs = std::filesystem;

static void pump(kanto::Session& a, kanto::Session& b, int ms) {
    for (int t = 0; t < ms; t += 100) { a.update(); b.update(); Sleep(100); }
}

int main(int argc, char** argv) {
    if (argc < 5) { std::printf("usage: kanto_live pkg scratch firered.gba seconds [relay.mjs] [desktop]\n"); return 2; }
    std::string pkgp = argv[1], scratch = argv[2], rom = argv[3];
    int secs = atoi(argv[4]);
    std::string relay_js = argc > 5 ? argv[5] : "";
    std::string desk = argc > 6 ? argv[6] : "kt_live_desk";
    fs::create_directories(winutil::to_wide(scratch));

    kanto::Package pkg;
    bool found = kanto::inspect_zip(pkgp, &pkg) || kanto::inspect_dir(pkgp, &pkg);   // zip or extracted folder, no folder scan
    if (!found) { std::printf("not a package: %s\n", pkgp.c_str()); return 1; }

    std::string dir;
    if (rom == "stub") {
        dir = kanto::install_dir(scratch + "\\data");
        std::string err0;
        if (!kanto::install_current(dir, pkg) && !kanto::install(pkg, dir, nullptr, &err0)) { std::printf("install failed: %s\n", err0.c_str()); return 1; }
        fs::create_directories(winutil::to_wide(dir + "rom"));
        std::string zeros(1 << 20, '\0'), cur;
        FILE* rf = nullptr;
        if (_wfopen_s(&rf, winutil::to_wide(kanto::rom_path(dir)).c_str(), L"wb") == 0 && rf) { for (int i = 0; i < 16; ++i) fwrite(zeros.data(), 1, zeros.size(), rf); fclose(rf); }
        std::printf("installed (stub ROM) -> %s\n", dir.c_str());
    } else {
        kanto::Prep prep;
        prep.start(pkg, scratch + "\\data", {{rom, "BPRE"}}, desk);
        while (prep.busy()) { std::printf("prep: %s %.0f%%\r", prep.message().c_str(), prep.progress() * 100); Sleep(250); }
        std::printf("\nprep -> %s: %s\n", prep.ok() ? "OK" : "FAILED", prep.message().c_str());
        if (!prep.ok()) return 1;
        dir = prep.dir();
    }
    const std::string vault = scratch + "\\vault";

    /* local relay (loopback only) */
    std::string relay = "ws://127.0.0.1:18799";
    HANDLE relay_proc = nullptr;
    if (!relay_js.empty()) {
        STARTUPINFOW si{}; si.cb = sizeof si;
        std::wstring dk = winutil::to_wide(desk);
        CreateDesktopW(dk.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        si.lpDesktop = &dk[0];
        std::wstring cmd = winutil::quote_arg(winutil::to_wide(kanto::node_exe(dir))) + L" " + winutil::quote_arg(winutil::to_wide(relay_js)) + L" " + winutil::quote_arg(winutil::to_wide(dir)) + L" 18799";
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { std::printf("relay start failed\n"); return 1; }
        relay_proc = pi.hProcess; CloseHandle(pi.hThread);
        Sleep(1200);
    } else relay.clear();

    kanto::LaunchOpts ho;
    ho.host = true; ho.dir = dir; ho.rom = kanto::rom_path(dir); ho.saves = scratch + "\\saves_host";
    kanto::stage_saves(vault, ho.saves, true); ho.sound = false; ho.relay = relay; ho.desktop = desk;
    kanto::Session host, guest;
    std::string err;
    if (!host.start(ho, &err)) { std::printf("host start: %s\n", err.c_str()); return 1; }
    for (int i = 0; i < 100 && host.state().code.empty(); ++i) { host.update(); Sleep(100); }
    std::printf("host code: %s phase=%s\n", host.state().code.c_str(), kanto::phase_name(host.state().phase));
    int rc = 1;
    /* KT_LIVE_GUEST_DELAY=<s>: the guest starts that long after the host (the host reaches the title screen first and must WAIT: barriers);
     * KT_LIVE_GUEST_PKG=<zip|folder>: the guest runs a different package build (mixed-version session). */
    if (const char* d = std::getenv("KT_LIVE_GUEST_DELAY")) { int n = atoi(d) * 10; for (int i = 0; i < n; ++i) { host.update(); Sleep(100); } std::printf("host after delay: phase=%s barrier=%d \"%s\"\n", kanto::phase_name(host.state().phase), (int)host.state().barrier_waiting, host.state().barrier_text.c_str()); }
    kanto::Package gpkg_p;
    std::string gdir = dir;
    if (const char* g = std::getenv("KT_LIVE_GUEST_PKG")) {
        if (!(kanto::inspect_zip(g, &gpkg_p) || kanto::inspect_dir(g, &gpkg_p))) { std::printf("guest pkg not a package\n"); return 1; }
        kanto::Prep gp;
        gp.start(gpkg_p, scratch + "\\gdata", {{rom, "BPRE"}}, desk);
        while (gp.busy()) Sleep(250);
        if (!gp.ok()) { std::printf("guest prep FAILED: %s\n", gp.message().c_str()); return 1; }
        gdir = gp.dir();
        std::printf("guest package: %s (has barriers: %d)  host package: %s\n", kanto::read_pkg_ver(gdir).id.c_str(), (int)kanto::has_barriers(gdir), kanto::read_pkg_ver(dir).id.c_str());
        std::printf("compare (host view of guest): %d\n", (int)kanto::compare_versions(kanto::read_pkg_ver(dir), kanto::read_pkg_ver(gdir)));
    }
    if (!host.state().code.empty()) {
        kanto::LaunchOpts go = ho;
        go.dir = gdir; go.rom = kanto::rom_path(gdir);
        go.host = false; go.code = host.state().code; go.saves = scratch + "\\saves_guest";
        kanto::stage_saves(vault, go.saves, false);
        if (!guest.start(go, &err)) std::printf("guest start: %s\n", err.c_str());
        else {
            for (int t = 0; t < secs * 10; ++t) {
                host.update(); guest.update(); Sleep(100);
                if (host.state().phase == kanto::Phase::Connected && guest.state().phase == kanto::Phase::Connected) break;
            }
            bool both = host.state().phase == kanto::Phase::Connected && guest.state().phase == kanto::Phase::Connected;
            std::printf("host=%s guest=%s\n", kanto::phase_name(host.state().phase), kanto::phase_name(guest.state().phase));
            if (both) {
                pump(host, guest, 6000);   // let coop.lua find the ROM
                if (std::getenv("KT_LIVE_GUEST_DELAY") || std::getenv("KT_LIVE_GUEST_PKG")) {
                    pump(host, guest, 25000);
                    std::printf("barrier host=%d \"%s\" legacy=%d together=%d | guest=%d \"%s\" legacy=%d together=%d\n", (int)host.state().barrier_waiting, host.state().barrier_text.c_str(), (int)host.state().legacy_peer, (int)host.state().together,
                                (int)guest.state().barrier_waiting, guest.state().barrier_text.c_str(), (int)guest.state().legacy_peer, (int)guest.state().together);
                    host.send("barrier status\n"); pump(host, guest, 1500);
                }
                host.trade(1); guest.trade(1);   // no party yet at the title screen: expect the usage / no-valid-slot notes, not a crash
                pump(host, guest, 2000);
                std::printf("trade note host: %s\n", host.state().trade.note.c_str());
                rc = 0;
            }
        }
    }
    std::printf("--- host log\n");
    for (auto& l : host.log()) std::printf("%s\n", l.c_str());
    std::printf("--- guest log\n");
    for (auto& l : guest.log()) std::printf("%s\n", l.c_str());
    /* the game is not there to write a journal: drop one in each staged folder to prove it reaches the vault */
    { std::ofstream(ho.saves + "\\" + kanto::journal_name(true)) << "1 COMMITTED host aa bb"; }
    guest.stop(true); host.stop(true);
    kanto::Session::drain_stops();
    int nh = kanto::flush_saves(ho.saves, vault, true), ng = kanto::flush_saves(scratch + "\\saves_guest", vault, false);
    std::printf("vault flush: host %d file(s), guest %d file(s)\n", nh, ng);
    for (auto& e : fs::directory_iterator(winutil::to_wide(vault))) std::printf("  vault: %s (%llu bytes)\n", winutil::to_utf8(e.path().filename().wstring()).c_str(), (unsigned long long)fs::file_size(e.path()));
    if (relay_proc) { TerminateProcess(relay_proc, 0); CloseHandle(relay_proc); }
    std::printf(rc == 0 ? "RESULT: both partners connected\n" : "RESULT: FAILED\n");
    return rc;
}
