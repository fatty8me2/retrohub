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

/* Registration policy: only a real installed copy may touch the user's Start Menu / Desktop. A build tree, a git
 * worktree, a headless/screenshot/test run or RETROHUB_NO_SHELL=1 must never register shortcuts. */
bool shell_path_looks_like_dev_build(const std::wstring& exe_path, bool (*dir_has)(const std::wstring& dir, const wchar_t* name) = nullptr);
bool shell_registration_allowed();   // checks env (RETROHUB_NO_SHELL, HUB_SCREENSHOT) + this exe's location

/* Lower-level, path-parameterised (used by the unit test so it never touches the real Start Menu). */
struct ShellLinkInfo {
    std::wstring target, workdir, icon, description, app_id;
    int icon_index = 0;
};
/* Creates/updates `lnk_path` for exe `exe_path`. Returns true if the file was (re)written, false if it was already
 * up to date or on failure (*ok tells which). */
bool shell_write_shortcut(const std::wstring& lnk_path, const std::wstring& exe_path, bool* ok = nullptr);
bool shell_read_shortcut(const std::wstring& lnk_path, ShellLinkInfo* out);
