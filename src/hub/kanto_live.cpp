/* Manual live driver (not a ctest): installs the user's Kanto Together zip into a scratch dir, builds the game from a FireRed ROM, then runs a
 * local relay plus a host and a guest launcher (kanto::Session, mGBA muted) all on a private invisible desktop and waits for
 * ">>> Your partner is connected. <<<" on both sides. Optionally exercises a trade offer round trip.
 * usage: kanto_live <package zip|folder> <scratch dir> <FireRed.gba> <seconds> [relay.mjs] [desktop name] */
#include <winsock2.h>
#include <windows.h>

#include <cstdio>
#include <filesystem>

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

    std::vector<kanto::Package> pk = kanto::find_packages({fs::path(winutil::to_wide(pkgp)).parent_path().string()});
    kanto::Package pkg;
    bool found = false;
    for (auto& p : pk) if (p.path == pkgp) { pkg = p; found = true; }
    if (!found && kanto::inspect_zip(pkgp, &pkg)) found = true;
    if (!found) { std::printf("not a package: %s\n", pkgp.c_str()); return 1; }

    kanto::Prep prep;
    prep.start(pkg, scratch + "\\data", {{rom, "BPRE"}}, desk);
    while (prep.busy()) { std::printf("prep: %s %.0f%%\r", prep.message().c_str(), prep.progress() * 100); Sleep(250); }
    std::printf("\nprep -> %s: %s\n", prep.ok() ? "OK" : "FAILED", prep.message().c_str());
    if (!prep.ok()) return 1;
    std::string dir = prep.dir();

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
    ho.host = true; ho.dir = dir; ho.rom = kanto::rom_path(dir); ho.saves = scratch + "\\saves_host"; ho.sound = false; ho.relay = relay; ho.desktop = desk;
    kanto::Session host, guest;
    std::string err;
    if (!host.start(ho, &err)) { std::printf("host start: %s\n", err.c_str()); return 1; }
    for (int i = 0; i < 100 && host.state().code.empty(); ++i) { host.update(); Sleep(100); }
    std::printf("host code: %s phase=%s\n", host.state().code.c_str(), kanto::phase_name(host.state().phase));
    int rc = 1;
    if (!host.state().code.empty()) {
        kanto::LaunchOpts go = ho;
        go.host = false; go.code = host.state().code; go.saves = scratch + "\\saves_guest";
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
    guest.stop(); host.stop();
    if (relay_proc) { TerminateProcess(relay_proc, 0); CloseHandle(relay_proc); }
    std::printf(rc == 0 ? "RESULT: both partners connected\n" : "RESULT: FAILED\n");
    return rc;
}
