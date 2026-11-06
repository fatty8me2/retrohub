/* rh-helper tests: the pure allowlist / name / command-line rules, plus the real exe's exit codes for rejected input.
 * Usage: rh_helper_test <path to rh-helper.exe> <path to data/helpers> */
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include "rh_helper_logic.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
namespace fs = std::filesystem;
using namespace rhhelper;

static DWORD run_exit(const fs::path& exe, const std::wstring& args) {
    std::wstring cmd = L"\"" + exe.wstring() + L"\" " + args;
    STARTUPINFOW si{}; si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return 0xFFFF;
    WaitForSingleObject(pi.hProcess, 20000);
    DWORD code = 0xFFFE;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return code;
}

int main(int argc, char** argv) {
    /* allowlist: exactly the shipped names, case-sensitive */
    CHECK(script_allowed("dolphin_netplay_driver.ps1") && script_allowed("n3ds_join.ps1"));
    CHECK(!script_allowed("") && !script_allowed("evil.ps1") && !script_allowed("N3DS_JOIN.PS1") && !script_allowed("n3ds_join.ps1 "));
    CHECK(!script_allowed("n3ds_join") && !script_allowed("n3ds_join.ps1.txt") && !script_allowed("n3ds_join.ps1."));
    /* path tricks never reach the allowlist */
    for (const char* bad : {"..\\n3ds_join.ps1", "../n3ds_join.ps1", "data\\helpers\\n3ds_join.ps1", "C:\\n3ds_join.ps1", "C:n3ds_join.ps1",
                            "\\\\srv\\share\\n3ds_join.ps1", "n3ds_join.ps1:stream", ".", "..", "n3ds_join.ps1\\", "/n3ds_join.ps1",
                            "n3ds_join.ps1\"", "n3ds_join.ps1\n"})
        CHECK(!is_plain_file_name(bad) && !script_allowed(bad) && script_path(L"C:\\RetroHub\\", bad).empty());
    CHECK(is_plain_file_name("n3ds_join.ps1"));
    /* every allowlisted script exists next to the sources and is a plain name */
    for (const char* a : kAllowedScripts) CHECK(is_plain_file_name(a) && (argc < 3 || fs::exists(fs::path(argv[2]) / a)));
    /* resolved only below <exe dir>\data\helpers\ */
    CHECK(script_path(L"C:\\RetroHub\\", "n3ds_join.ps1") == L"C:\\RetroHub\\data\\helpers\\n3ds_join.ps1");
    CHECK(script_path(L"C:\\RetroHub", "n3ds_join.ps1") == L"C:\\RetroHub\\data\\helpers\\n3ds_join.ps1");

    /* command line: `<exe> run <script> <tail>`, tail untouched */
    std::string n; std::wstring t;
    CHECK(parse_run(L"\"C:\\Program Files\\RetroHub\\rh-helper.exe\" run n3ds_join.ps1 -AzPid 42 -Status \"C:\\a b\\s.txt\" -Pw \"\"", &n, &t) &&
          n == "n3ds_join.ps1" && t == L"-AzPid 42 -Status \"C:\\a b\\s.txt\" -Pw \"\"");
    CHECK(parse_run(L"rh-helper.exe run n3ds_join.ps1", &n, &t) && n == "n3ds_join.ps1" && t.empty());
    CHECK(parse_run(L"rh-helper.exe   run   \"n3ds_join.ps1\"   -A 1", &n, &t) && t == L"-A 1");
    CHECK(!parse_run(L"rh-helper.exe", &n, &t) && !parse_run(L"rh-helper.exe run", &n, &t) && !parse_run(L"rh-helper.exe exec n3ds_join.ps1", &n, &t));
    CHECK(!parse_run(L"rh-helper.exe run \x00E9vil.ps1", &n, &t));
    CHECK(parse_run(L"rh-helper.exe run ..\\n3ds_join.ps1", &n, &t) && !script_allowed(n));

    /* the real exe */
    if (argc >= 2 && fs::exists(argv[1])) {
        const fs::path exe = argv[1];
        CHECK(run_exit(exe, L"") == 2 && run_exit(exe, L"run") == 2 && run_exit(exe, L"walk n3ds_join.ps1") == 2);
        CHECK(run_exit(exe, L"run evil.ps1") == 3 && run_exit(exe, L"run ..\\n3ds_join.ps1") == 3 && run_exit(exe, L"run C:\\Windows\\win.ini") == 3);
        CHECK(run_exit(exe, L"run \"data\\helpers\\n3ds_join.ps1\"") == 3);
        /* allowed name, but no data\helpers\ next to this copy of the exe: refuses (4), never looks anywhere else */
        const fs::path tmp = fs::temp_directory_path() / "rh_helper_test_dir";
        std::error_code ec;
        fs::remove_all(tmp, ec); fs::create_directories(tmp, ec);
        fs::copy_file(exe, tmp / "rh-helper.exe", fs::copy_options::overwrite_existing, ec);
        CHECK(!ec && run_exit(tmp / "rh-helper.exe", L"run n3ds_join.ps1 -AzPid 1") == 4);
        fs::remove_all(tmp, ec);
    }
    std::printf(fails ? "rh_helper_test FAILED\n" : "rh_helper_test OK\n");
    return fails ? 1 : 0;
}
