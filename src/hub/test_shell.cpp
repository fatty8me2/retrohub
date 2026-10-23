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

    {   // one-click favorite shortcut (scratch dir only)
        std::wstring glnk = dir + L"\\Mario Kart_ 64.lnk";
        CHECK(shell_game_shortcut_name(L"Super: Mario/Kart?") == L"Super  Mario Kart.lnk");
        CHECK(shell_game_shortcut_name(L"  ") == L"Game.lnk");
        unsigned char px[4 * 4 * 4];
        for (int k = 0; k < 16; ++k) { px[k*4] = 200; px[k*4+1] = 50; px[k*4+2] = 20; px[k*4+3] = 255; }
        std::wstring ico = dir + L"\\g.ico";
        CHECK(shell_write_ico(ico, px, 4, 4));
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        CHECK(GetFileAttributesExW(ico.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow == 22 + 40 + 128*128*4 + 128*128/8);
        CHECK(shell_write_game_shortcut(glnk, exe, L"n64-abc", L"Mario Kart 64", ico));
        ShellLinkInfo g;
        CHECK(shell_read_shortcut(glnk, &g));
        CHECK(_wcsicmp(g.target.c_str(), exe.c_str()) == 0);
        CHECK(_wcsicmp(g.icon.c_str(), ico.c_str()) == 0);
        CHECK(g.description == L"Play Mario Kart 64 with RetroHub");
        IShellLinkW* sl = nullptr; IPersistFile* pf = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) {
            sl->QueryInterface(IID_PPV_ARGS(&pf));
            wchar_t args[256] = {};
            CHECK(pf && SUCCEEDED(pf->Load(glnk.c_str(), STGM_READ)) && SUCCEEDED(sl->GetArguments(args, 256)) && std::wstring(args) == L"--play n64-abc");
            if (pf) pf->Release();
            sl->Release();
        }
        DeleteFileW(glnk.c_str()); DeleteFileW(ico.c_str());
        SetEnvironmentVariableW(L"HUB_SHORTCUT_DIR", L"C:\\scratch");
        CHECK(shell_shortcut_dir() == L"C:\\scratch");
        SetEnvironmentVariableW(L"HUB_SHORTCUT_DIR", nullptr);
        CHECK(shell_shortcut_dir().empty());   // dev build: never the real Desktop
    }
    {   // registration policy
        auto none = [](const std::wstring&, const wchar_t*) { return false; };
        CHECK(!shell_path_looks_like_dev_build(L"C:\\Users\\u\\Desktop\\smashClassic\\RetroHub.exe", none));
        CHECK(!shell_path_looks_like_dev_build(L"C:\\Program Files\\RetroHub\\RetroHub.exe", none));
        CHECK(shell_path_looks_like_dev_build(L"C:\\x\\wt\\version-check\\build\\bin\\Release\\RetroHub.exe", none));
        CHECK(shell_path_looks_like_dev_build(L"C:\\x\\RetroHub\\build\\bin\\RetroHub.exe", none));
        CHECK(shell_path_looks_like_dev_build(L"C:\\x\\foo\\bin\\Release\\RetroHub.exe", none));
        CHECK(shell_path_looks_like_dev_build(L"C:\\x\\app\\RetroHub.exe", [](const std::wstring& d, const wchar_t* n) {
            return d == L"C:\\x" && std::wstring(n) == L".git"; }));
        SetEnvironmentVariableA("RETROHUB_NO_SHELL", "1");
        CHECK(!shell_registration_allowed());
        SetEnvironmentVariableA("RETROHUB_NO_SHELL", nullptr);
        SetEnvironmentVariableA("HUB_SCREENSHOT", "x.bmp");
        CHECK(!shell_registration_allowed());
        SetEnvironmentVariableA("HUB_SCREENSHOT", nullptr);
        CHECK(!shell_registration_allowed());   // this test exe lives in a build tree
    }
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
