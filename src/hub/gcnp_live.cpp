/* Manual live driver (not a ctest): host + guest Dolphin in separate user dirs on a private invisible desktop, connected
 * ONLY through hubnp3ds::Runner forwarders over a hubnet::Session (guest Dolphin -> 127.0.0.1:<forwarder> -> room
 * server/relay -> host forwarder -> host Dolphin). Honors HUBNET_FORCE_RELAY=1.
 * usage: gcnp_live <dolphin.exe> <scratch dir> <iso dir> <game id> <seconds> <server url> */
#include <winsock2.h>
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "netplay_3ds_run.h"
#include "netplay_dolphin.h"
#include "session.h"

static std::string slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
static void put(const std::string& p, const std::string& t) { std::ofstream f(p, std::ios::binary); f << t; }
static void mk(const std::string& p) { CreateDirectoryA(p.c_str(), nullptr); }

int main(int argc, char** argv) {
    if (argc < 7) { printf("usage: gcnp_live dolphin.exe scratch isodir gameid seconds server\n"); return 2; }
    std::string exe = argv[1], scratch = argv[2], iso = argv[3], server = argv[6];
    int secs = atoi(argv[5]);
    gcnp::Spec s;
    s.game_id = argv[4]; s.players = 2; s.mode = "direct"; s.addr = "127.0.0.1"; s.port = 27123;
    hubnp3ds::Plan plan; plan.port = s.port; plan.expected = 2;
    hubnp3ds::Runner host, guest;
    std::string err;
    if (!host.start_host_forward(server, "GCHXKE", 1, plan, "gc", &err)) { printf("host fw: %s\n", err.c_str()); return 1; }
    if (!guest.start_guest(server, "GCHXKE", 1, plan, 1, &err, "gc")) { printf("guest fw: %s\n", err.c_str()); return 1; }
    for (int i = 0; i < 120 && !(host.ready() && guest.ready()); ++i) {
        host.update(); guest.update(); Sleep(250);
        if (host.failed() || guest.failed()) { printf("forwarder failed: %s / %s\n", host.status().c_str(), guest.status().c_str()); return 1; }
    }
    printf("forwarders: host=%s guest=%s (guest local port %d)\n", host.status().c_str(), guest.status().c_str(), guest.connect_port());
    if (!host.ready() || !guest.ready()) return 1;

    std::string ua = scratch + "\\liveA", ub = scratch + "\\liveB";
    for (auto& d : {ua, ub}) { mk(d); mk(d + "\\Config"); }
    gcnp::IniParams hp; hp.host = true; hp.nickname = "HostA"; hp.iso_dir = iso; hp.mute = true;
    gcnp::IniParams gp; gp.host = false; gp.nickname = "GuestB"; gp.iso_dir = iso; gp.mute = true;
    put(ua + "\\Config\\Dolphin.ini", gcnp::ini_patch(slurp(ua + "\\Config\\Dolphin.ini"), s, hp));
    gcnp::Spec gs = s; gs.port = guest.connect_port();
    put(ub + "\\Config\\Dolphin.ini", gcnp::ini_patch(slurp(ub + "\\Config\\Dolphin.ini"), gs, gp));

    gcnp::Run ra, rb;
    if (!gcnp::start(true, exe, ua, s, 0, scratch + "\\workA", "rhgc_live", "", &ra)) { printf("host start failed %s\n", ra.error.c_str()); return 1; }
    for (int i = 0; i < 160 && gcnp::read_status(ra).find("stage:netplay-open") == std::string::npos; ++i) { host.update(); guest.update(); Sleep(250); }
    if (!gcnp::start(false, exe, ub, gs, 1, scratch + "\\workB", "rhgc_live", "", &rb)) { printf("guest start failed\n"); return 1; }
    for (int t = 0; t < secs * 4; ++t) { host.update(); guest.update(); Sleep(250); }
    printf("--- host\n%s\n--- guest\n%s\n", gcnp::read_status(ra).c_str(), gcnp::read_status(rb).c_str());
    printf("datagrams host out/in %llu/%llu, guest out/in %llu/%llu\n", host.datagrams_out(), host.datagrams_in(), guest.datagrams_out(), guest.datagrams_in());
    for (DWORD pid : {ra.dolphin_pid, rb.dolphin_pid, ra.driver_pid, rb.driver_pid}) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (h) { TerminateProcess(h, 0); CloseHandle(h); }
    }
    return 0;
}
