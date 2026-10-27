/* Central "never pop up over the user's game" policy (pure logic, unit-tested by hub_popup_guard_test).
 *
 * HARD RULE: while any game or emulator runs (hub session active, or another full-screen / busy app is in front), RetroHub must not
 * raise, focus, restore, flash-with-activation or show any window, modal or prompt above it. Every OS call that can do so goes through
 * a helper that asks these functions first (see window_guard.cpp); test_popup_guard_lint scans src/hub and fails on a raw call. */
#pragma once

namespace hubguard {

/* A game is "active" when the hub owns a running session (process or job object) OR the OS reports another app owning the screen
 * (full-screen game / presentation / busy; SHQueryUserNotificationState). The second part also covers a game started by a previous
 * RetroHub run or by hand. */
inline bool game_active(bool hub_session_alive, bool os_fullscreen_or_busy) { return hub_session_alive || os_fullscreen_or_busy; }

/* Raise / restore / show / focus / MessageBox: only with no game active. With a game active the caller flashes the taskbar instead. */
inline bool may_surface(bool game_active_) { return !game_active_; }

/* In-window prompts (update prompt, "what's new", invite join, rejoin): wait until no game is running AND the hub already has the focus.
 * Headless screenshot / QA runs have no real window, so they always pass. */
inline bool may_open_prompt(bool game_active_, bool hub_has_focus, bool headless) { return headless || (!game_active_ && hub_has_focus); }

/* After a game exits (TV mode re-fullscreen, focus the hub): only touch the window when the hub is the focused window already. */
inline bool may_restore_after_exit(bool hub_has_focus) { return hub_has_focus; }

/* Startup / relaunch after an update: show the window without activation (minimised) when a game or another busy app is up front. */
inline bool start_quiet(bool background_flag, bool game_active_) { return background_flag || game_active_; }

/* Foreground window classes of the desktop shell: if the update relaunch finds one of these in front, nobody is gaming. */
inline bool is_shell_class(const wchar_t* c) {
    static const wchar_t* k[] = {L"Shell_TrayWnd", L"Progman", L"WorkerW", L"CabinetWClass"};
    if (!c) return false;
    for (const wchar_t* s : k) {
        const wchar_t* a = c; const wchar_t* b = s;
        while (*a && *a == *b) { ++a; ++b; }
        if (!*a && !*b) return true;
    }
    return false;
}

}  // namespace hubguard
