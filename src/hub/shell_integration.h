#pragma once
/* Windows shell integration for RetroHub: taskbar identity, window icon, Start Menu / Desktop shortcuts.
 * Call order in main(): shell_set_app_id() as early as possible (before any window), then after the SDL
 * window exists shell_set_window_icon(win) and shell_ensure_start_menu_shortcut(). */
#include <string>

struct SDL_Window;

#define RETROHUB_APP_ID L"RetroHub.Launcher"

void shell_set_app_id();                       // SetCurrentProcessExplicitAppUserModelID(L"RetroHub.Launcher")
void shell_set_window_icon(SDL_Window* win);   // big + small WM_SETICON from the embedded IDI_APPICON
void shell_ensure_start_menu_shortcut();       // %APPDATA%\...\Start Menu\Programs\RetroHub.lnk (only rewritten if missing/stale)
void shell_ensure_desktop_shortcut(bool create);   // create=true: make Desktop\RetroHub.lnk if missing/stale; false: remove ours
void shell_ensure_url_protocol();              // HKCU\Software\Classes\retrohub: "retrohub://join/<CODE>" -> RetroHub.exe "%1" (invite.h)

/* Registration policy: only a real installed copy may touch the user's Start Menu / Desktop. A build tree, a git
 * worktree, a headless/screenshot/test run or RETROHUB_NO_SHELL=1 must never register shortcuts. */
bool shell_path_looks_like_dev_build(const std::wstring& exe_path, bool (*dir_has)(const std::wstring& dir, const wchar_t* name) = nullptr);
bool shell_registration_allowed();   // checks env (RETROHUB_NO_SHELL, HUB_SCREENSHOT) + this exe's location

/* One-click favorites: a .lnk that runs `RetroHub.exe --play <game key>`.
 * shell_shortcut_dir(): HUB_SHORTCUT_DIR (scratch override for tests/screenshots) if set, else the Desktop when
 * shell_registration_allowed(), else "" (= refuse). icon_path may be an .ico; "" uses RetroHub's own icon. */
std::wstring shell_shortcut_dir();
std::wstring shell_game_shortcut_name(const std::wstring& title);   // file-name-safe "<title>.lnk"
bool shell_write_game_shortcut(const std::wstring& lnk_path, const std::wstring& exe_path, const std::wstring& game_key,
                               const std::wstring& title, const std::wstring& icon_path);
/* Writes a 128x128 .ico (32-bit BMP entry) from RGBA pixels, scaled to fit. */
bool shell_write_ico(const std::wstring& ico_path, const unsigned char* rgba, int w, int h);

/* Lower-level, path-parameterised (used by the unit test so it never touches the real Start Menu). */
struct ShellLinkInfo {
    std::wstring target, workdir, icon, description, app_id;
    int icon_index = 0;
};
/* Creates/updates `lnk_path` for exe `exe_path`. Returns true if the file was (re)written, false if it was already
 * up to date or on failure (*ok tells which). */
bool shell_write_shortcut(const std::wstring& lnk_path, const std::wstring& exe_path, bool* ok = nullptr);
bool shell_read_shortcut(const std::wstring& lnk_path, ShellLinkInfo* out);
