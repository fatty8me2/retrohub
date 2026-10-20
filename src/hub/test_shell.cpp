/* Console test for shell_integration: writes .lnk files into %TEMP% only (never the real Start Menu). */
#include "shell_integration.h"
#include <windows.h>
#include <shobjidl.h>
#include <objbase.h>
#include <cstdio>
#include <string>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"retrohub_shell_test_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring lnk = dir + L"\\sub\\RetroHub.lnk";   // sub-folder must be created on demand
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring exe = self;

    bool ok = false;
    bool wrote = shell_write_shortcut(lnk, exe, &ok);
    CHECK(ok); CHECK(wrote);
    CHECK(GetFileAttributesW(lnk.c_str()) != INVALID_FILE_ATTRIBUTES);

    ShellLinkInfo i;
    CHECK(shell_read_shortcut(lnk, &i));
    std::wstring wd = exe.substr(0, exe.find_last_of(L"\\/"));
    CHECK(_wcsicmp(i.target.c_str(), exe.c_str()) == 0);
    CHECK(_wcsicmp(i.workdir.c_str(), wd.c_str()) == 0);
    CHECK(_wcsicmp(i.icon.c_str(), exe.c_str()) == 0);
    CHECK(i.description == L"RetroHub - your retro game library");
    CHECK(i.app_id == L"RetroHub.Launcher");
    std::wprintf(L"target=%ls\nworkdir=%ls\nicon=%ls,%d\ndesc=%ls\nAUMID=%ls\n", i.target.c_str(), i.workdir.c_str(), i.icon.c_str(),
                 i.icon_index, i.description.c_str(), i.app_id.c_str());

    wrote = shell_write_shortcut(lnk, exe, &ok);   // up to date -> not rewritten
    CHECK(ok); CHECK(!wrote);

    std::wstring other = dir + L"\\Other.exe";      // different target -> rewritten
    wrote = shell_write_shortcut(lnk, other, &ok);
    CHECK(ok); CHECK(wrote);
    CHECK(shell_read_shortcut(lnk, &i)); CHECK(_wcsicmp(i.target.c_str(), other.c_str()) == 0);

    shell_set_app_id();
    PWSTR cur = nullptr;
    CHECK(SUCCEEDED(GetCurrentProcessExplicitAppUserModelID(&cur)) && cur && std::wstring(cur) == L"RetroHub.Launcher");
    CoTaskMemFree(cur);

    DeleteFileW(lnk.c_str());
    RemoveDirectoryW((dir + L"\\sub").c_str());
    RemoveDirectoryW(dir.c_str());
    std::printf(fails ? "hub_shell_test: %d FAILED\n" : "hub_shell_test: all passed\n", fails);
    return fails ? 1 : 0;
}
