// Console test for the native update applier: pure logic, apply_files (incl. rollback) and an
// integration dry-run of `RetroHub.exe --apply-update` against scratch dirs (non-ASCII path,
// locked-file rollback, wait-for-pid). Never shows a window and never relaunches anything
// (--no-relaunch). Usage: hub_apply_test <path to RetroHub.exe>
#include "update_apply.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace fs = std::filesystem;
using namespace hubupdate::apply;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); g_fail++; } } while (0)

static void put(const fs::path& p, const std::string& s) {
    std::error_code ec; fs::create_directories(p.parent_path(), ec);
    std::ofstream o(p, std::ios::binary | std::ios::trunc); o << s;
}
static std::string get(const fs::path& p) {
    std::ifstream i(p, std::ios::binary); std::stringstream ss; ss << i.rdbuf(); return ss.str();
}
static bool has(const fs::path& p) { std::error_code ec; return fs::exists(p, ec); }

/* staged: new version of everything; install: old version + user data that must survive. */
static void make_scene(const fs::path& root, fs::path* staged, fs::path* install) {
    std::error_code ec; fs::remove_all(root, ec);
    *staged = root / L"staged"; *install = root / L"RetroHub";
    put(*staged / L"RetroHub.exe", "new-hub");
    put(*staged / L"BattleShip.exe", "new-bs");
    put(*staged / L"0new.txt", "brand new");
    put(*staged / L"a.txt", "a-new");
    put(*staged / L"b.dat", "b-new");
    put(*staged / L"sub\\deep\\c.txt", "c-new");
    put(*staged / L"z.txt", "z-new");
    put(*staged / L"BattleShip.o2r", "o2r-from-zip");        // must be skipped
    put(*staged / L"userdata\\hub.json", "zip-userdata");     // must be skipped
    put(*staged / L"logs\\x.log", "zip-log");                 // must be skipped
    put(*install / L"RetroHub.exe", "old-hub");
    put(*install / L"BattleShip.exe", "old-bs");
    put(*install / L"a.txt", "a-old");
    put(*install / L"b.dat", "b-old");
    put(*install / L"z.txt", "z-old");
    put(*install / L"BattleShip.o2r", "user-o2r");
    put(*install / L"userdata\\hub.json", "user-settings");
    put(*install / L"userdata\\saves\\mario.sav", "user-save");
    put(*install / L"extra-user-file.txt", "keep me");        // never deleted
}

static void check_untouched(const fs::path& install) {
    CHECK(get(install / L"BattleShip.o2r") == "user-o2r");
    CHECK(get(install / L"userdata\\hub.json") == "user-settings");
    CHECK(get(install / L"userdata\\saves\\mario.sav") == "user-save");
    CHECK(get(install / L"extra-user-file.txt") == "keep me");
    CHECK(!has(install / L"logs\\x.log"));
}
static void check_old(const fs::path& install) {
    CHECK(get(install / L"RetroHub.exe") == "old-hub");
    CHECK(get(install / L"BattleShip.exe") == "old-bs");
    CHECK(get(install / L"a.txt") == "a-old");
    CHECK(get(install / L"b.dat") == "b-old");
    CHECK(get(install / L"z.txt") == "z-old");
    CHECK(!has(install / L"0new.txt"));
    CHECK(!has(install / L"sub"));  // new dirs' files removed (dir may remain, files must not)
}
static void check_new(const fs::path& install) {
    CHECK(get(install / L"RetroHub.exe") == "new-hub");
    CHECK(get(install / L"BattleShip.exe") == "new-bs");
    CHECK(get(install / L"0new.txt") == "brand new");
    CHECK(get(install / L"a.txt") == "a-new");
    CHECK(get(install / L"b.dat") == "b-new");
    CHECK(get(install / L"sub\\deep\\c.txt") == "c-new");
    CHECK(get(install / L"z.txt") == "z-new");
}

/* Runs a command line hidden; returns exit code or -1. */
static int run_hidden(std::wstring cmd, DWORD* pidOut = nullptr, bool wait = true) {
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
    if (pidOut) *pidOut = pi.dwProcessId;
    DWORD code = 0;
    if (wait) { WaitForSingleObject(pi.hProcess, 60000); GetExitCodeProcess(pi.hProcess, &code); }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return wait ? (int)code : 0;
}

int main(int argc, char** argv) {
    // ---- pure helpers ----
    CHECK(is_excluded(L"userdata\\hub.json"));
    CHECK(is_excluded(L"UserData/saves/x.sav"));
    CHECK(is_excluded(L"BattleShip.o2r"));
    CHECK(is_excluded(L"sub\\battleship.O2R"));
    CHECK(is_excluded(L"logs\\updater.log"));
    CHECK(is_excluded(L"anything.log"));
    CHECK(is_excluded(L"updates\\update.zip"));
    CHECK(is_excluded(L"saves\\a.bin"));
    CHECK(is_excluded(L"game.sav"));
    CHECK(!is_excluded(L"RetroHub.exe"));
    CHECK(!is_excluded(L"deps\\n64run.exe"));
    CHECK(!is_excluded(L"userdata"));            // a plain file with that name is not the folder
    CHECK(!is_excluded(L"docs\\userdata.txt"));
    CHECK(is_safe_rel(L"a\\b.txt"));
    CHECK(!is_safe_rel(L"..\\evil.txt"));
    CHECK(!is_safe_rel(L"a\\..\\..\\evil.txt"));
    CHECK(!is_safe_rel(L"C:\\evil.txt"));
    CHECK(!is_safe_rel(L"\\evil.txt"));
    CHECK(!is_safe_rel(L""));
    CHECK(path_is_under(L"C:\\Games\\RetroHub", L"c:\\games\\retrohub\\deps\\x.exe"));
    CHECK(path_is_under(L"C:\\Games\\RetroHub\\", L"C:\\Games\\RetroHub\\x.exe"));
    CHECK(!path_is_under(L"C:\\Games\\RetroHub", L"C:\\Games\\RetroHub2\\x.exe"));
    CHECK(!path_is_under(L"C:\\Games\\RetroHub", L"C:\\Games\\RetroHub"));

    wchar_t tb[MAX_PATH + 1]; DWORD tn = GetTempPathW(MAX_PATH, tb);
    fs::path scratch = fs::path(std::wstring(tb, tn)) / (L"retrohub_apply_test_" + std::to_wstring(GetCurrentProcessId()));
    fs::path root = scratch / L"Jeux Vid\u00e9o \u65e5\u672c";   // non-ASCII (accent + CJK)
    fs::path staged, install;

    // ---- list_staged ----
    make_scene(root / L"list", &staged, &install);
    auto files = list_staged(staged.wstring());
    CHECK(files.size() == 7);
    bool leaked = false;
    for (auto& f : files) if (is_excluded(f)) leaked = true;
    CHECK(!leaked);

    // ---- apply_files: success, non-ASCII path ----
    make_scene(root / L"ok", &staged, &install);
    Options o; o.staged = staged.wstring(); o.install = install.wstring(); o.backup = (root / L"ok" / L"bk").wstring();
    Result r = apply_files(o);
    CHECK(r.ok); CHECK(r.copied == 7);
    check_new(install); check_untouched(install);
    CHECK(!has(root / L"ok" / L"bk"));    // backup removed on success

    // ---- apply_files: locked file => full rollback ----
    make_scene(root / L"lock", &staged, &install);
    o.staged = staged.wstring(); o.install = install.wstring(); o.backup = (root / L"lock" / L"bk").wstring();
    o.retries = 1; o.retry_ms = 0;
    HANDLE lk = CreateFileW((install / L"b.dat").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(lk != INVALID_HANDLE_VALUE);
    r = apply_files(o);
    CHECK(!r.ok); CHECK(r.rolled_back); CHECK(!r.error.empty());
    CloseHandle(lk);
    check_old(install); check_untouched(install);
    CHECK(!has(install / L"sub\\deep\\c.txt"));

    // ---- mid-copy failure: dest truncated, then copy fails => original bytes restored ----
    make_scene(root / L"mid", &staged, &install);
    o.staged = staged.wstring(); o.install = install.wstring(); o.backup = (root / L"mid" / L"bk").wstring();
    o.copy_hook = [](const std::wstring& from, const std::wstring& to) -> bool {
        if (to.size() >= 5 && to.compare(to.size() - 5, 5, L"b.dat") == 0) {
            std::ofstream t(fs::path(to), std::ios::binary | std::ios::trunc); t << "PARTIAL";
            return false;
        }
        return CopyFileW(from.c_str(), to.c_str(), FALSE) != FALSE;
    };
    r = apply_files(o);
    o.copy_hook = nullptr;
    CHECK(!r.ok); CHECK(r.rolled_back);
    CHECK(get(install / L"b.dat") == "b-old");
    check_old(install); check_untouched(install);

    // ---- is_safe_rel rejects ':' (alternate data streams) ----
    CHECK(!is_safe_rel(L"a.txt:evil")); CHECK(is_safe_rel(L"sub\a.txt"));

    // ---- missing required exe ----
    make_scene(root / L"req", &staged, &install);
    fs::remove(staged / L"BattleShip.exe");
    o.staged = staged.wstring(); o.install = install.wstring(); o.backup.clear();
    r = apply_files(o);
    CHECK(!r.ok); CHECK(get(install / L"RetroHub.exe") == "old-hub");

    // ---- integration: RetroHub.exe --apply-update (needs the exe) ----
    if (argc >= 2) {
        std::wstring exe = fs::path(argv[1]).wstring();
        CHECK(has(exe));
        auto cmdFor = [&](const fs::path& st, const fs::path& in, DWORD pid, const std::wstring& extra) {
            return L"\"" + exe + L"\" --apply-update \"" + st.wstring() + L"\" \"" + in.wstring() + L"\" " +
                   std::to_wstring(pid) + L" --no-relaunch " + extra;
        };
        // success into "...\Jeux Vidéo ...\RetroHub"
        make_scene(root / L"i_ok", &staged, &install);
        int rc = run_hidden(cmdFor(staged, install, 0, L"--version v9.9.9"));
        CHECK(rc == 0);
        check_new(install); check_untouched(install);
        CHECK(!has(staged));                                  // staged dir cleaned
        CHECK(has(install / L"userdata\\logs\\updater.log")); // log lives in userdata\logs
        // locked file => exit 1 and everything restored
        make_scene(root / L"i_lock", &staged, &install);
        lk = CreateFileW((install / L"b.dat").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(lk != INVALID_HANDLE_VALUE);
        rc = run_hidden(cmdFor(staged, install, 0, L"--retries 1 --retry-ms 0"));
        CloseHandle(lk);
        CHECK(rc == 1);
        check_old(install); check_untouched(install);
        // waits for the given pid to exit (ping runs ~2s), then applies
        make_scene(root / L"i_wait", &staged, &install);
        DWORD pid = 0;
        std::wstring ping = L"ping -n 3 127.0.0.1";
        run_hidden(L"cmd /c " + ping, &pid, false);
        auto t0 = std::chrono::steady_clock::now();
        rc = run_hidden(cmdFor(staged, install, pid, L""));
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(rc == 0); CHECK(secs >= 0.8);
        check_new(install);
        // usage error
        CHECK(run_hidden(L"\"" + exe + L"\" --apply-update onlyone") == 2);
    } else {
        std::printf("note: no RetroHub.exe path given, integration cases skipped\n");
    }

    std::error_code ec; fs::remove_all(scratch, ec);
    std::printf(g_fail ? "hub_apply_test: %d FAILED\n" : "hub_apply_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
