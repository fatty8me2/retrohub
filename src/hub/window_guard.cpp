/* The ONLY place in the hub UI that raises, restores or shows the hub window. See popup_guard.h for the rule. */
#include "app.h"
#include "popup_guard.h"

#include <shellapi.h>

static bool os_busy() {
    /* Cached: asked at most twice a second. QUNS_BUSY / RUNNING_D3D_FULL_SCREEN / PRESENTATION_MODE = another app owns the screen. */
    static ULONGLONG last = 0;
    static bool busy = false;
    ULONGLONG now = GetTickCount64();
    if (now - last < 500 && last) return busy;
    last = now;
    QUERY_USER_NOTIFICATION_STATE s = QUNS_ACCEPTS_NOTIFICATIONS;
    busy = SUCCEEDED(SHQueryUserNotificationState(&s)) &&
           (s == QUNS_BUSY || s == QUNS_RUNNING_D3D_FULL_SCREEN || s == QUNS_PRESENTATION_MODE);
    return busy;
}

bool game_active() {
    if (g_headless) return A.proc != nullptr;
    return hubguard::game_active(A.proc != nullptr || A.job_h != nullptr, os_busy());
}

bool hub_has_focus() { return g_headless || (A.win && (SDL_GetWindowFlags(A.win) & SDL_WINDOW_INPUT_FOCUS) != 0); }

bool prompt_allowed() { return hubguard::may_open_prompt(game_active(), hub_has_focus(), g_headless); }

bool hub_surface() {
    if (!A.win || g_headless) return false;
    if (!hubguard::may_surface(game_active())) return false;   // a game is up: stay out of the way (the bell shows the request)
    SDL_RestoreWindow(A.win);
    SDL_RaiseWindow(A.win);
    return true;
}

void hub_show_noactivate() {
    if (!A.hwnd || g_headless) return;
    ShowWindow(A.hwnd, SW_SHOWMINNOACTIVE);   // in the taskbar, never in front, never focused
}
