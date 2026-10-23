/* Manual live driver (not a ctest): two Azahar instances on a private invisible desktop join ONE azahar-room through
 * hubnp3ds::Runner (host: azahar-room + host forwarder; guest: guest forwarder), over a hubnet::Session on the given
 * room server. Honors HUBNET_FORCE_RELAY=1. Everything is muted, runs from scratch copies, and only our own PIDs are killed.
 *
 * usage: n3ds_live <azahar install dir> <scratch dir> <game file> <seconds> <server url> [room port]
 * Output: forwarder state, per-instance Azahar log excerpts, room log, datagram counters, windows seen on the hidden desktop. */
#include <winsock2.h>
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "netplay_3ds_join.h"
#include "netplay_3ds_run.h"
#include "session.h"

static std::string slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
static void put(const std::string& p, const std::string& t) { std::ofstream f(p, std::ios::binary); f << t; }
static void mk(const std::string& p) { CreateDirectoryA(p.c_str(), nullptr); }
static std::wstring W(const std::string& s) { return std::wstring(s.begin(), s.end()); }

static std::wstring g_desktop = L"rh3ds_live";

struct Proc { HANDLE h = nullptr; DWORD pid = 0; };

static bool spawn_hidden(const std::string& cmd, const std::string& cwd, Proc* out) {
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.lpDesktop = &g_desktop[0];
    PROCESS_INFORMATION pi{};
    std::wstring c = W(cmd);
    if (!CreateProcessW(nullptr, &c[0], nullptr, nullptr, FALSE, 0, nullptr, W(cwd).c_str(), &si, &pi)) return false;
    out->h = pi.hProcess; out->pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    return true;
}

static BOOL CALLBACK enum_win(HWND h, LPARAM) {
    wchar_t t[256] = L"", cls[128] = L"";
    GetWindowTextW(h, t, 256);
    GetClassNameW(h, cls, 128);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (IsWindowVisible(h) && t[0]) printf("  window pid=%lu class=%ls title=%ls\n", pid, cls, t);
    return TRUE;
}

static std::string grep_lines(const std::string& text, const std::vector<std::string>& keys, int maxn) {
    std::istringstream is(text);
    std::string l, out;
    int n = 0;
    while (std::getline(is, l) && n < maxn) {
        for (auto& k : keys)
            if (l.find(k) != std::string::npos) { out += "    " + l + "\n"; ++n; break; }
    }
    return out;
}

static std::string prep_instance(const std::string& src, const std::string& dst) {
    std::string cmd = "robocopy \"" + src + "\" \"" + dst + "\" /E /NFL /NDL /NJH /NJS /NP >nul";
    system(cmd.c_str());
    mk(dst + "\\user"); mk(dst + "\\user\\config"); mk(dst + "\\user\\sdmc"); mk(dst + "\\user\\log");
    /* muted (Null audio, volume 0), no confirm-on-close / update prompts */
    put(dst + "\\user\\config\\qt-config.ini",
        "[Audio]\noutput_type\\default=false\noutput_type=1\nvolume\\default=false\nvolume=0\n"
        "[UI]\nconfirmClose\\default=false\nconfirmClose=false\ncheck_for_update_on_start\\default=false\ncheck_for_update_on_start=false\n"
        "Updater\\check_for_update_on_start\\default=false\nUpdater\\check_for_update_on_start=false\n"
        "firstStart\\default=false\nfirstStart=false\nshowConsole\\default=false\nshowConsole=false\n");
    return dst + "\\azahar.exe";
}

int main(int argc, char** argv) {
    if (argc < 6) { printf("usage: n3ds_live azahar_dir scratch game seconds server [port]\n"); return 2; }
    std::string azdir = argv[1], scratch = argv[2], game = argv[3], server = argv[5];
    int secs = atoi(argv[4]);
    hubnp3ds::Plan plan;
    plan.port = argc > 6 ? atoi(argv[6]) : 24990;
    plan.expected = 2;
    const char* code = "TDSLVE";
    mk(scratch);

    std::string azA = scratch + "\\azA", azB = scratch + "\\azB", work = scratch + "\\room";
    mk(work);
    std::string exeA = prep_instance(azdir, azA), exeB = prep_instance(azdir, azB);
    std::string room_exe = hubnp3ds::room_exe_for(exeA);
    printf("room exe: %s\n", room_exe.c_str());

    hubnp3ds::Runner host, guest;
    std::string err;
    if (!host.start_host(server, code, 1, plan, room_exe, work, &err)) { printf("host start: %s\n", err.c_str()); return 1; }
    if (!guest.start_guest(server, code, 1, plan, 1, &err)) { printf("guest start: %s\n", err.c_str()); return 1; }
    for (int i = 0; i < 120 && !(host.ready() && guest.ready()); ++i) {
        host.update(); guest.update(); Sleep(250);
        if (host.failed() || guest.failed()) { printf("forwarder failed: %s / %s\n", host.status().c_str(), guest.status().c_str()); return 1; }
    }
    printf("forwarders: host=%s guest=%s (guest local port %d, room port %d)\n", host.status().c_str(), guest.status().c_str(), guest.connect_port(), host.connect_port());
    if (!host.ready() || !guest.ready()) return 1;

    CreateDesktopW(g_desktop.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    std::string mpA = hubnp3ds::multiplayer_arg(hubnp3ds::nickname("HostA", 0), plan.password, "127.0.0.1", host.connect_port());
    std::string mpB = hubnp3ds::multiplayer_arg(hubnp3ds::nickname("GuestB", 1), plan.password, "127.0.0.1", guest.connect_port());
    Proc pa, pb;
    const char* join_delay = getenv("N3DS_JOIN_DELAY");
    if (!spawn_hidden("\"" + exeA + "\" -w \"" + game + "\"", azA, &pa)) { printf("azahar A start failed\n"); return 1; }
    printf("azahar A pid %lu args: %s\n", pa.pid, mpA.c_str());
    std::string stA, stB;
    hubnp3ds::start_join_driver(pa.pid, scratch, "HostA_1", "", "127.0.0.1", host.connect_port(), g_desktop, &stA);
    for (int i = 0; i < 40; ++i) { host.update(); guest.update(); Sleep(250); }   // stagger so logs are readable
    if (!spawn_hidden("\"" + exeB + "\" -w \"" + game + "\"", azB, &pb)) { printf("azahar B start failed\n"); return 1; }
    printf("azahar B pid %lu\n", pb.pid);
    { std::string wb = scratch + "\\drvB"; hubnp3ds::start_join_driver(pb.pid, wb, "GuestB_2", "", "127.0.0.1", guest.connect_port(), g_desktop, &stB); }
    (void)join_delay;

    for (int t = 0; t < secs * 4; ++t) {
        host.update(); guest.update(); Sleep(250);
        if (t % 40 == 39) printf("t=%ds dgrams host out/in %llu/%llu guest out/in %llu/%llu\n", (t + 1) / 4, host.datagrams_out(), host.datagrams_in(), guest.datagrams_out(), guest.datagrams_in());
    }
    printf("--- windows on hidden desktop\n");
    if (HDESK d = OpenDesktopW(g_desktop.c_str(), 0, FALSE, GENERIC_ALL)) { EnumDesktopWindows(d, enum_win, 0); CloseDesktop(d); }
    printf("--- join driver A: %s\n--- join driver B: %s\n", slurp(stA).c_str(), slurp(stB).c_str());
    printf("--- datagrams: host out/in %llu/%llu, guest out/in %llu/%llu\n", host.datagrams_out(), host.datagrams_in(), guest.datagrams_out(), guest.datagrams_in());
    std::vector<std::string> keys = {"oom", "ultiplayer", "UDS", "Network", "network", "Error", "Critical", "member", "Connected", "Joined", "joined"};
    printf("--- room log\n%s", grep_lines(slurp(work + "\\azahar-room.log"), {"", }, 40).c_str());
    printf("--- azahar A log\n%s", grep_lines(slurp(azA + "\\user\\log\\azahar_log.txt"), keys, 40).c_str());
    printf("--- azahar B log\n%s", grep_lines(slurp(azB + "\\user\\log\\azahar_log.txt"), keys, 40).c_str());
    for (Proc* p : {&pa, &pb}) if (p->h) { TerminateProcess(p->h, 0); CloseHandle(p->h); }
    host.stop(); guest.stop();
    return 0;
}
