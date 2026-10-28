/* Live Dolphin speed test over the hub's DSU pad. HEADLESS: Dolphin runs on a private hidden desktop (CreateDesktop), muted, with a
 * throwaway user dir; nothing is shown and no input is injected. Uses the real code path: prepare_dolphin (ini writers) +
 * dolphin_speed_start/send (virtual DSU pad) + dolphin_speed_read_percent (Dolphin's own window-title speed).
 * A tiny homebrew .dol (an endless loop, generated here) is booted, so no game files are needed.
 *
 * usage: hub_dolphin_speed_live --dolphin <dir with Dolphin.exe> --work <scratch dir> [--video Null] [--settle 4]
 * exit: 0 pass, 1 fail, 77 skipped (no Dolphin / no title speed available on this machine). */
#include "emulators.h"
#include "logic.h"
#include "controls.h"
#include "dolphin_speed.h"
#include "dolphin_speed_logic.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using hubqm::DolphinKey;

static void put32(std::vector<uint8_t>& b, size_t off, uint32_t v) { for (int i = 0; i < 4; ++i) b[off + i] = (uint8_t)(v >> (24 - 8 * i)); }

/* Minimal GameCube .dol: one text section at 0x80003100 containing `b .` (branch to self) + nops. Header = 0x100 bytes of big-endian fields. */
static void write_spin_dol(const fs::path& p) {
    std::vector<uint8_t> d(0x100 + 0x40, 0);
    put32(d, 0x00, 0x100);          /* text[0] file offset */
    put32(d, 0x48, 0x80003100);     /* text[0] load address */
    put32(d, 0x90, 0x40);           /* text[0] size */
    put32(d, 0xE0, 0x80003100);     /* entry point (bss address/size at 0xD8/0xDC stay 0) */
    /* Publishes the PowerPC timebase at 0x80004000 forever, with the magic "RHMK" at 0x80004004 so the test can find Dolphin's guest RAM. */
    const uint32_t code[] = {
        0x3CE08000,  /* lis  r7,0x8000 */
        0x3D005248,  /* lis  r8,0x5248 */   0x61084D4B,  /* ori  r8,r8,0x4D4B */
        0x91074004,  /* stw  r8,0x4004(r7) */
        0x7CCC42E6,  /* loop: mftb r6 */    0x90C74000,  /* stw  r6,0x4000(r7) */
        0x4BFFFFF8,  /* b loop */
    };
    for (int i = 0; i < 7; ++i) put32(d, 0x100 + 4 * i, code[i]);
    for (int i = 7; i < 16; ++i) put32(d, 0x100 + 4 * i, 0x60000000);   /* nop */
    std::ofstream(p, std::ios::binary).write((const char*)d.data(), (std::streamsize)d.size());
}

static std::string rd(const fs::path& p) { std::ifstream f(p, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), {}); }
static void wr(const fs::path& p, const std::string& s) { std::ofstream(p, std::ios::binary) << s; }

/* Find the guest-RAM view in Dolphin's address space (a large MEM_MAPPED region holding the "RHMK" marker at offset 0x4004). */
static uintptr_t find_guest_ram(HANDLE proc) {
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t a = 0; VirtualQueryEx(proc, (LPCVOID)a, &mbi, sizeof mbi) == sizeof mbi; a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_MAPPED || mbi.RegionSize < 0x1800000) continue;
        uint8_t m[4] = {};
        SIZE_T n = 0;
        if (ReadProcessMemory(proc, (LPCVOID)((uintptr_t)mbi.BaseAddress + 0x4004), m, 4, &n) && n == 4 && !memcmp(m, "RHMK", 4)) return (uintptr_t)mbi.BaseAddress;
    }
    return 0;
}
static bool read_tb(HANDLE proc, uintptr_t ram, uint32_t* tb) {
    uint8_t b[4] = {};
    SIZE_T n = 0;
    if (!ReadProcessMemory(proc, (LPCVOID)(ram + 0x4000), b, 4, &n) || n != 4) return false;
    *tb = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];   /* guest RAM is big endian */
    return true;
}
/* Guest speed in percent over `secs` of wall time: guest timebase ticks / wall time (hubds::tb_speed_percent). */
static double measure(HANDLE proc, uintptr_t ram, double secs) {
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    uint32_t t0 = 0, t1 = 0;
    if (!read_tb(proc, ram, &t0)) return -1;
    QueryPerformanceCounter(&a);
    Sleep((DWORD)(secs * 1000));
    if (!read_tb(proc, ram, &t1)) return -1;
    QueryPerformanceCounter(&b);
    return hubds::tb_speed_percent(t0, t1, (double)(b.QuadPart - a.QuadPart) / (double)f.QuadPart);
}

int main(int argc, char** argv) {
    std::string dolphin, work, video = "Null";
    int settle = 4;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--dolphin")) dolphin = argv[i + 1];
        else if (!strcmp(argv[i], "--work")) work = argv[i + 1];
        else if (!strcmp(argv[i], "--video")) video = argv[i + 1];
        else if (!strcmp(argv[i], "--settle")) settle = std::atoi(argv[i + 1]);
    }
    fs::path exe = fs::path(dolphin) / "Dolphin.exe";
    if (dolphin.empty() || work.empty() || !fs::exists(exe)) { std::printf("SKIP: no Dolphin.exe at '%s'\n", dolphin.c_str()); return 77; }
    fs::path w = fs::absolute(work);
    if (w.string().size() < 8) { std::printf("refusing work dir '%s'\n", w.string().c_str()); return 1; }
    std::error_code ec;
    fs::remove_all(w, ec);
    fs::create_directories(w / "User" / "Config");

    /* the real ini writers, then test-only overrides: quiet, chosen video backend */
    hubemu::DolphinPads pads;   /* no pads: SI devices off */
    hubemu::prepare_dolphin(w.string(), pads, hubctl::default_profile("gc"));
    fs::path ini = w / "User" / "Config" / "Dolphin.ini";
    std::string t = rd(ini);
    t = hubui::ini_set(t, "Core", "GFXBackend", video);
    t = hubui::ini_set(t, "DSP", "Backend", "No Audio Output");
    t = hubui::ini_set(t, "DSP", "Volume", "0");
    wr(ini, t);
    if (std::getenv("DSPEED_LOG")) wr(w / "User" / "Config" / "Logger.ini", "[Options]\nWriteToFile = True\nVerbosity = 5\n[Logs]\nCONTROLLERINTERFACE = True\n");   /* debugging only */
    wr(w / "spin.dol", "");
    write_spin_dol(w / "spin.dol");
    std::printf("Hotkeys.ini:\n%s\nDSUClient.ini:\n%s\n", rd(w / "User" / "Config" / "Hotkeys.ini").c_str(), rd(w / "User" / "Config" / "DSUClient.ini").c_str());

    const bool sniff = std::getenv("DSPEED_SNIFF") != nullptr;   /* debugging: leave port 26761 to an external listener, just boot Dolphin for 15 s */
    if (!sniff && !dolphin_speed_start()) { std::printf("FAIL: DSU port %d busy\n", hubds::kPort); return 1; }

    std::wstring deskname = L"rhspeed_" + std::to_wstring(GetCurrentProcessId());
    HDESK desk = CreateDesktopW(deskname.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desk) { std::printf("FAIL: CreateDesktop %lu\n", GetLastError()); return 1; }
    SetThreadDesktop(desk);   /* this (window-less) thread now enumerates the hidden desktop's windows */

    std::wstring cmd = L"\"" + exe.wstring() + L"\" -u \"" + (w / "User").wstring() + L"\" -b -e \"" + (w / "spin.dol").wstring() + L"\" -C \"Dolphin.Core.GFXBackend=" +
                       std::wstring(video.begin(), video.end()) + L"\" -C \"Dolphin.DSP.Backend=No Audio Output\"";
    STARTUPINFOW si{}; si.cb = sizeof si; si.lpDesktop = (LPWSTR)deskname.c_str();
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, exe.parent_path().c_str(), &si, &pi)) { std::printf("FAIL: CreateProcess %lu\n", GetLastError()); return 1; }
    std::vector<unsigned long> pids = {pi.dwProcessId};
    std::printf("Dolphin pid %lu on hidden desktop\n", pi.dwProcessId);

    int fails = 0;
    uintptr_t ram = 0;
    if (sniff) { Sleep(15000); TerminateProcess(pi.hProcess, 0); return 0; }
    for (int i = 0; i < 240 && !ram; ++i) {   /* up to 60 s to boot and publish the marker */
        Sleep(250);
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) { std::printf("FAIL: Dolphin exited early\n"); fails = 1; break; }
        ram = find_guest_ram(pi.hProcess);
    }
    if (!fails && !ram) {
        std::printf("SKIP: guest RAM marker not found in Dolphin's memory after 60 s\n");
        TerminateProcess(pi.hProcess, 0); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); dolphin_speed_stop();
        return 77;
    }
    if (!fails) {
        double base = measure(pi.hProcess, ram, 3.0);
        std::printf("boot: guest RAM at %p, speed %.1f%%, DSU client connected=%d, foreground window of this thread's desktop: none (hidden)\n", (void*)ram, base, dolphin_speed_connected() ? 1 : 0);
        if (!hubds::speed_matches(10, (int)(base + 0.5))) { std::printf("FAIL: baseline is not ~100%%\n"); ++fails; }
        struct Phase { const char* name; DolphinKey key; int n; int tenths; bool check; };
        const Phase phases[] = {
            {"1x -> 0.5x (5 decreases)", DolphinKey::Decrease, 5, 5, true},
            {"0.5x -> 0.1x floor (4 decreases)", DolphinKey::Decrease, 4, 1, true},
            {"floor: 3 more decreases stay at 0.1x", DolphinKey::Decrease, 3, 1, true},
            {"0.1x -> 1x (9 increases)", DolphinKey::Increase, 9, 10, true},
            {"1x -> 2x (10 increases)", DolphinKey::Increase, 10, 20, true},
            {"2x -> 1x restore (10 decreases)", DolphinKey::Decrease, 10, 10, true},
        };
        std::printf("%-42s %6s %8s %9s %s\n", "phase", "target", "presses", "measured", "result");
        for (const Phase& ph : phases) {
            std::vector<int> vks(ph.n, hubqm::dolphin_scan(hubqm::dolphin_vk(ph.key)));
            volatile int done = -99;
            DWORD t0 = GetTickCount();
            dolphin_speed_send(vks, [&](int d) { done = d; });
            while (done == -99 && GetTickCount() - t0 < 30000) Sleep(20);
            Sleep(1500);
            double got = measure(pi.hProcess, ram, (double)settle);
            bool ok = done == ph.n && (!ph.check || (got >= 0 && hubds::speed_matches(ph.tenths, (int)(got + 0.5))));
            std::printf("%-42s %5d%% %4d/%-3d %8.1f%% %s\n", ph.name, ph.tenths * 10, done, ph.n, got, ok ? "ok" : "FAIL");
            if (!ok) ++fails;
        }
    }

    /* close Dolphin: WM_CLOSE to its windows on the hidden desktop, then (only our own pid) terminate if it lingers */
    struct Ctx { DWORD pid; } c{pi.dwProcessId};
    EnumWindows([](HWND h, LPARAM l) -> BOOL { DWORD p = 0; GetWindowThreadProcessId(h, &p); if (p == ((Ctx*)l)->pid) PostMessageW(h, WM_CLOSE, 0, 0); return TRUE; }, (LPARAM)&c);
    if (WaitForSingleObject(pi.hProcess, 8000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    dolphin_speed_stop();
    std::printf(fails ? "RESULT: FAIL (%d)\n" : "RESULT: PASS\n", fails);
    return fails ? 1 : 0;
}
