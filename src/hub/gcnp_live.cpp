/* Manual live driver (not a ctest): two Dolphins in separate user dirs on a private invisible desktop, host + guest
 * through gcnp::start (direct mode on 127.0.0.1 or traversal), prints both status files for N seconds.
 * usage: gcnp_live <dolphin.exe> <scratch dir> <iso dir> <game id> <seconds> [traversal] */
#include "netplay_dolphin.h"

#include <windows.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static std::string slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
static void put(const std::string& p, const std::string& t) { std::ofstream f(p, std::ios::binary); f << t; }

int main(int argc, char** argv) {
    if (argc < 6) { printf("usage\n"); return 2; }
    std::string exe = argv[1], scratch = argv[2], iso = argv[3];
    int secs = atoi(argv[5]);
    bool trav = argc > 6 && std::string(argv[6]) == "traversal";
    gcnp::Spec s;
    s.game_id = argv[4]; s.players = 2;
    if (trav) s.mode = "traversal"; else { s.mode = "direct"; s.addr = "127.0.0.1"; s.port = 2626; }
    std::string ua = scratch + "\\liveA", ub = scratch + "\\liveB";
    CreateDirectoryA(ua.c_str(), nullptr); CreateDirectoryA(ub.c_str(), nullptr);
    CreateDirectoryA((ua + "\\Config").c_str(), nullptr); CreateDirectoryA((ub + "\\Config").c_str(), nullptr);
    gcnp::IniParams hp; hp.host = true; hp.nickname = "HostA"; hp.iso_dir = iso; hp.mute = true;
    gcnp::IniParams gp; gp.host = false; gp.nickname = "GuestB"; gp.iso_dir = iso; gp.mute = true;
    put(ua + "\\Config\\Dolphin.ini", gcnp::ini_patch(slurp(ua + "\\Config\\Dolphin.ini"), s, hp));
    gcnp::Run ra, rb;
    if (!gcnp::start(true, exe, ua, s, 0, scratch + "\\workA", "rhgc_live", &ra)) { printf("host start failed %s\n", ra.error.c_str()); return 1; }
    if (trav) {
        std::string code;
        for (int i = 0; i < 120 && code.empty(); ++i) { Sleep(500); code = gcnp::read_code(ra); }
        printf("host code: %s\n", code.c_str());
        if (code.empty()) { printf("%s\n", gcnp::read_status(ra).c_str()); return 1; }
        s.host_code = code;
    } else Sleep(8000);
    put(ub + "\\Config\\Dolphin.ini", gcnp::ini_patch(slurp(ub + "\\Config\\Dolphin.ini"), s, gp));
    if (!gcnp::start(false, exe, ub, s, 1, scratch + "\\workB", "rhgc_live", &rb)) { printf("guest start failed\n"); return 1; }
    for (int t = 0; t < secs; t += 5) Sleep(5000);
    printf("--- host\n%s\n--- guest\n%s\n", gcnp::read_status(ra).c_str(), gcnp::read_status(rb).c_str());
    for (DWORD pid : {ra.dolphin_pid, rb.dolphin_pid, ra.driver_pid, rb.driver_pid}) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (h) { TerminateProcess(h, 0); CloseHandle(h); }
    }
    return 0;
}
