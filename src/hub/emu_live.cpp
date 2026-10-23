// Console driver that runs the exact RetroHub 3DS / Wii U launch path without the UI:
//   find/install emulator -> unpack zip -> write config -> save copy-in -> spawn (job object) -> wait -> save copy-back.
// Usage: hub_emu_live <game file> [--fullscreen] [--seconds N] [--shots <dir>] [--pads N]   (--pads: GameCube: pretend N XInput pads, 0 = none)
// Set RETROHUB_NO_EXISTING_EMU=1 to ignore any Cemu install outside RetroHub's own data dir.
#include "winutil.h"
#include "emulators.h"
#include "games.h"
#include "library.h"
#include "savestore.h"
#include "logic.h"

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static std::wstring W(const std::string& s) { return winutil::to_wide(s); }
static std::string slurp(const std::string& p) {
    std::ifstream f(W(p), std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static void spit(const std::string& p, const std::string& t) {
    std::ofstream f(W(p), std::ios::binary | std::ios::trunc);
    f << t;
}

static std::vector<DWORD> job_pids(HANDLE job) {
    std::vector<DWORD> out;
    char buf[sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + 64 * sizeof(ULONG_PTR)];
    auto* l = (JOBOBJECT_BASIC_PROCESS_ID_LIST*)buf;
    if (QueryInformationJobObject(job, JobObjectBasicProcessIdList, l, sizeof buf, nullptr))
        for (DWORD i = 0; i < l->NumberOfAssignedProcesses; ++i) out.push_back((DWORD)l->ProcessIdList[i]);
    return out;
}
struct WinFind { std::vector<DWORD> pids; std::vector<HWND> wins; };
static BOOL CALLBACK enum_cb(HWND h, LPARAM lp) {
    auto* f = (WinFind*)lp;
    if (!IsWindowVisible(h)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    for (DWORD p : f->pids) if (p == pid) { f->wins.push_back(h); break; }
    return TRUE;
}
static void save_bmp(const std::string& path, HWND h) {
    RECT r;
    GetWindowRect(h, &r);
    int w = r.right - r.left, ht = r.bottom - r.top;
    if (w <= 0 || ht <= 0) return;
    HDC sc = GetDC(nullptr), mem = CreateCompatibleDC(sc);
    HBITMAP bm = CreateCompatibleBitmap(sc, w, ht);
    SelectObject(mem, bm);
    BitBlt(mem, 0, 0, w, ht, sc, r.left, r.top, SRCCOPY);   // screen copy: the window is brought to the front first (D3D windows come out black with PrintWindow)
    BITMAPINFOHEADER bi{sizeof bi, w, -ht, 1, 24, BI_RGB};
    int stride = (w * 3 + 3) & ~3;
    std::vector<unsigned char> px((size_t)stride * ht);
    GetDIBits(mem, bm, 0, ht, px.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);
    BITMAPFILEHEADER fh{0x4D42, (DWORD)(sizeof fh + sizeof bi + px.size()), 0, 0, sizeof fh + sizeof bi};
    bi.biHeight = -ht;
    std::ofstream f(W(path), std::ios::binary);
    f.write((char*)&fh, sizeof fh);
    f.write((char*)&bi, sizeof bi);
    f.write((char*)px.data(), px.size());
    DeleteObject(bm);
    DeleteDC(mem);
    ReleaseDC(nullptr, sc);
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::string game_path, shots;
    bool fs = false;
    int fake_pads = -1;
    int seconds = 60;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--fullscreen") fs = true;
        else if (a == "--seconds" && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (a == "--shots" && i + 1 < argc) shots = argv[++i];
        else if (a == "--pads" && i + 1 < argc) fake_pads = atoi(argv[++i]);
        else game_path = a;
    }
    hub::GameEntry e;
    if (!hub::identify_game(game_path, &e)) { std::printf("not a recognized game: %s\n", game_path.c_str()); return 2; }
    std::printf("game: %s key=%s system=%s issue='%s'\n", e.title.c_str(), e.key.c_str(), hub::system_short(e.system), e.issue.c_str());
    hubemu::Kind kind = hubemu::kind_for(e.system);
    ULONGLONG t0 = GetTickCount64();
    auto stamp = [&]() { return (double)(GetTickCount64() - t0) / 1000.0; };

    if (e.system == hub::System::N3DS && e.issue == "extract" && hub::extracted_3ds_path(e).empty()) {
        std::printf("[%.1fs] would ask to unpack into %s (%llu bytes)\n", stamp(), hub::extract_3ds_dir(e).c_str(),
                    (unsigned long long)hub::zip_3ds_unpacked_size(e));
        std::string out, err;
        if (!hub::extract_3ds_zip(e, [](uint64_t, uint64_t) { return true; }, &out, &err)) { std::printf("UNPACK FAILED: %s\n", err.c_str()); return 3; }
        std::printf("[%.1fs] unpacked -> %s\n", stamp(), out.c_str());
    }
    hubemu::Install inst = hubemu::find(kind, "", e.path);
    if (inst.exe.empty()) {
        std::printf("[%.1fs] %s not installed: downloading...\n", stamp(), hubemu::name(kind));
        std::string err;
        int lastphase = -1;
        if (!hubemu::install_blocking(kind, hubemu::managed_root(), &err, [&](int ph, uint64_t, uint64_t) {
                if (ph != lastphase) { lastphase = ph; std::printf("  phase %d at %.1fs\n", ph, stamp()); }
            })) { std::printf("INSTALL FAILED: %s\n", err.c_str()); return 4; }
        inst = hubemu::find(kind, "", e.path);
    }
    std::printf("[%.1fs] emulator: %s (%s, managed=%d)\n", stamp(), inst.exe.c_str(), inst.how.c_str(), (int)inst.managed);
    if (inst.exe.empty() || !inst.managed) { std::printf("refusing: not a managed install\n"); return 5; }
    if (hubemu::is_running(inst.exe)) { std::printf("already running, abort\n"); return 6; }

    if (kind == hubemu::Kind::Azahar) hubemu::prepare_azahar(inst.dir);
    else if (kind == hubemu::Kind::Dolphin) {
        hubemu::DolphinPads pads = hubemu::detect_dolphin_pads();
        if (fake_pads >= 0) { for (int i = 0; i < 4; ++i) pads.xinput[i] = i < fake_pads; pads.adapter = false; }
        std::printf("pads: xinput=%d%d%d%d adapter=%d\n", pads.xinput[0], pads.xinput[1], pads.xinput[2], pads.xinput[3], (int)pads.adapter);
        hubemu::prepare_dolphin(inst.dir, pads);
    } else hubemu::prepare_cemu_managed(inst.dir);
    // test-only: mute the emulator
    if (kind == hubemu::Kind::Dolphin) {
        std::string ini = inst.dir + "\\User\\Config\\Dolphin.ini";
        spit(ini, hubui::ini_set(slurp(ini), "DSP", "Volume", "0"));
    } else if (kind == hubemu::Kind::Azahar) {
        std::string ini = inst.dir + "\\user\\config\\qt-config.ini";
        std::string t = slurp(ini);
        if (t.find("[Audio]") == std::string::npos) t += "\n[Audio]\nvolume\\default=false\nvolume=0\n";
        spit(ini, t);
    } else {
        std::string x = inst.dir + "\\portable\\settings.xml";
        std::string t = slurp(x);
        if (t.find("<Audio>") == std::string::npos) {
            size_t p = t.rfind("</content>");
            if (p != std::string::npos) t.insert(p, "\t<Audio>\n\t\t<api>3</api>\n\t\t<TVVolume>0</TVVolume>\n\t\t<PadVolume>0</PadVolume>\n\t\t<InputVolume>0</InputVolume>\n\t</Audio>\n");
            spit(x, t);
        }
    }

    std::string game = (e.issue == "extract") ? hub::extracted_3ds_path(e) : e.path;
    std::string args = kind == hubemu::Kind::Azahar ? hubemu::azahar_args(game, fs)
                       : kind == hubemu::Kind::Dolphin ? hubemu::dolphin_args(game, fs, hubemu::dolphin_card_root(inst.dir, e.key) + "\\GCI")
                                                       : hubemu::cemu_args(game, fs);
    std::string battery = hub::saves::battery_dir(hub::system_short(e.system), e.key);
    hubemu::SaveSession sess = hubemu::begin_session(e, inst, battery, "");
    std::printf("[%.1fs] begin_session active=%d err='%s' battery=%s\n", stamp(), (int)sess.active, sess.error.c_str(), battery.c_str());
    std::printf("cmd: %s %s\n", inst.exe.c_str(), args.c_str());

    std::wstring cmd = L"\"" + W(inst.exe) + L"\" " + W(args);
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(W(inst.exe).c_str(), buf.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, W(inst.dir).c_str(), &si, &pi)) {
        std::printf("CreateProcess failed %lu\n", GetLastError());
        return 7;
    }
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    ULONGLONG ls = GetTickCount64();
    std::printf("[%.1fs] launched pid %lu\n", stamp(), pi.dwProcessId);

    HWND firstwin = nullptr;
    int nshot = 0;
    for (int s = 0; s < seconds; ++s) {
        Sleep(1000);
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) { std::printf("[%.1fs] emulator exited early (code?)\n", stamp()); break; }
        WinFind f;
        f.pids = job_pids(job);
        EnumWindows(enum_cb, (LPARAM)&f);
        if (!firstwin && !f.wins.empty()) { firstwin = f.wins[0]; std::printf("[%.1fs] first visible window after %.1fs\n", stamp(), (GetTickCount64() - ls) / 1000.0); }
        if (!shots.empty() && s % 10 == 9) {
            HWND best = nullptr; LONG area = 0;
            for (HWND h : f.wins) { RECT r; GetWindowRect(h, &r); LONG a = (r.right - r.left) * (r.bottom - r.top); if (a > area) { area = a; best = h; } }
            if (best) {
                char title[256]; GetWindowTextA(best, title, sizeof title);
                RECT r; GetWindowRect(best, &r);
                char p[512]; std::snprintf(p, sizeof p, "%s\\shot_%02d_%ds.bmp", shots.c_str(), nshot++, s + 1);
                ShowWindow(best, SW_SHOWNORMAL);
                SetWindowPos(best, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                SetForegroundWindow(best);
                Sleep(500);
                save_bmp(p, best);
                LONG style = GetWindowLongW(best, GWL_STYLE);
                HMONITOR mon = MonitorFromWindow(best, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi{sizeof mi};
                GetMonitorInfoW(mon, &mi);
                bool covers = r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
                std::printf("[%.1fs] shot %s  window '%s' %ldx%ld at (%ld,%ld) caption=%d covers_monitor=%d\n", stamp(), p, title, r.right - r.left, r.bottom - r.top,
                            r.left, r.top, (int)((style & WS_CAPTION) == WS_CAPTION), (int)covers);
            }
        }
    }
    // graceful close
    {
        WinFind f;
        f.pids = job_pids(job);
        EnumWindows(enum_cb, (LPARAM)&f);
        for (HWND h : f.wins) PostMessageW(h, WM_CLOSE, 0, 0);
        std::printf("[%.1fs] sent WM_CLOSE to %zu windows\n", stamp(), f.wins.size());
    }
    bool graceful = false;
    for (int i = 0; i < 40; ++i) {
        Sleep(500);
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION ji{};
        QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &ji, sizeof ji, nullptr);
        if (ji.ActiveProcesses == 0) { graceful = true; break; }
    }
    if (!graceful) { std::printf("not exited after 20s: terminating job (our own processes)\n"); TerminateJobObject(job, 1); Sleep(1000); }
    std::printf("[%.1fs] job empty, graceful=%d\n", stamp(), (int)graceful);
    hubemu::end_session(sess);
    std::printf("[%.1fs] end_session err='%s' summary='%s'\n", stamp(), sess.error.c_str(), sess.summary.c_str());
    return 0;
}
