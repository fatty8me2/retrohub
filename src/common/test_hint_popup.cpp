/* Runs on a private (hidden) desktop: the runner's tips card must be no-activate / click-through, OWNED by the game window and NOT
 * topmost (it can never sit above another app), must not change the foreground window, must disappear after its timeline, and a
 * caller that is not the game's own runner (the hub, over somebody else's emulator) must never get a window at all.
 * Never touches the user's desktop. */
#include <cstdio>

#include "hint_popup.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static HWND find_popup() { return FindWindowW(L"RetroHubStartHint", nullptr); }

int main() {
    HDESK dk = CreateDesktopW(L"rh_hint_test", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!dk) { std::printf("starthint popup test: cannot create a hidden desktop, skipped\n"); return 0; }
    SetThreadDesktop(dk);
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"HintTestGame";
    RegisterClassW(&wc);
    HWND game = CreateWindowExW(0, L"HintTestGame", L"game", WS_OVERLAPPEDWINDOW, 100, 100, 960, 600, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(game, SW_SHOW);
    SetForegroundWindow(game);
    HWND fg = GetForegroundWindow();

    {   // a caller that does not own the game (the hub over an emulator): finishes at once, never creates a window
        starthint::Popup hub;
        for (int i = 0; i < 5; i++) { hub.tick(game, false, false); CHECK(find_popup() == nullptr); }
        CHECK(hub.done());
        CHECK(GetForegroundWindow() == fg);
    }

    starthint::Popup p;
    ULONGLONG t0 = GetTickCount64();
    bool shown = false, fg_changed = false;
    while (!p.done() && GetTickCount64() - t0 < 9000) {
        p.tick(game, false, true);
        HWND h = find_popup();
        if (h && IsWindowVisible(h)) {
            if (!shown) {
                shown = true;
                LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
                CHECK(ex & WS_EX_NOACTIVATE); CHECK(ex & WS_EX_TRANSPARENT); CHECK(ex & WS_EX_TOOLWINDOW); CHECK(ex & WS_EX_LAYERED);
                CHECK(!(ex & WS_EX_TOPMOST));                       // never topmost: it cannot sit above another app
                CHECK(GetWindow(h, GW_OWNER) == game);               // owned by the game window: follows its z-order, dies with it
                RECT r, gr;
                GetWindowRect(h, &r);
                POINT c{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
                CHECK(SendMessageW(h, WM_NCHITTEST, 0, MAKELPARAM(c.x, c.y)) == HTTRANSPARENT);
                GetClientRect(game, &gr);
                POINT tl{0, 0};
                ClientToScreen(game, &tl);
                CHECK(r.left >= tl.x && r.left < tl.x + 100 && r.bottom <= tl.y + gr.bottom);   // bottom-left corner of the game
            }
            CHECK(!(GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOPMOST));
            if (GetForegroundWindow() != fg) fg_changed = true;
        }
        Sleep(50);
    }
    CHECK(shown);
    CHECK(!fg_changed);
    CHECK(GetForegroundWindow() == fg);
    CHECK(p.done() && find_popup() == nullptr);
    DestroyWindow(game);
    std::printf(fails ? "hint_popup_test: %d FAILED\n" : "hint_popup_test: ok (owned, not topmost, foreground unchanged; hub callers get no window)\n", fails);
    return fails ? 1 : 0;
}
