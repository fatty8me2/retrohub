#include "hint_popup.h"

#include <algorithm>
#include <cmath>

namespace starthint {

static const wchar_t* kCls = L"RetroHubStartHint";

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

/* The card is an OWNED popup of the game window: not topmost, so it stays exactly as high as the game (it follows the game's z-order,
 * hides with it and dies with it) and can never sit above another app. Created visible but never activated (WS_EX_NOACTIVATE), and
 * layered, so nothing is drawn until the first UpdateLayeredWindow. There is no ShowWindow and no z-order change anywhere: the popup
 * guard lint covers this directory. */
bool Popup::create(HWND owner) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kCls;
    RegisterClassW(&wc);   // fails harmlessly when already registered
    hwnd_ = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_LAYERED, kCls, L"", WS_POPUP | WS_VISIBLE,
                            0, 0, bmp_.w, bmp_.h, owner, nullptr, wc.hInstance, nullptr);
    return hwnd_ != nullptr;
}

void Popup::close() {
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
    if (state_ != Done) state_ = Done;
}

void Popup::tick(HWND target, bool kb, bool runner) {
    if (state_ == Done) return;
    /* Only a runner (n64run) draws its card over its OWN game window, like any on-screen display. A caller that is not the game
     * (the hub, over Dolphin / PCSX2 / Azahar / Cemu / mGBA / BattleShip) must never draw over somebody else's running game: it shows
     * the same tips in its own window while the game is still starting instead (src/hub/main.cpp, draw_launch_tips). */
    if (!runner) { state_ = Done; return; }
    ULONGLONG now = GetTickCount64();
    if (!t_wait_) t_wait_ = now;
    if (state_ == Waiting) {
        if (now - t_wait_ > (ULONGLONG)kGiveUpWaitMs) { state_ = Done; return; }
        RECT cr{};
        if (!target || !IsWindow(target) || !IsWindowVisible(target) || IsIconic(target) || !GetClientRect(target, &cr)) return;
        int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
        if (cw < 320 || ch < 200) return;
        scale_ = scale_for_height(ch);
        bmp_ = render(pick_mode(kb, runner), scale_);
        if (bmp_.px.empty() || !create(target)) { state_ = Done; return; }
        state_ = Showing;
        t_start_ = now;
    }
    long long el = (long long)(now - t_start_);
    RECT cr{};
    if (finished(el) || !target || !IsWindow(target) || !GetClientRect(target, &cr)) { close(); return; }
    if (IsIconic(target) || !IsWindowVisible(target)) return;   // an owned window is hidden by the system with its owner
    POINT tl{0, 0};
    ClientToScreen(target, &tl);
    int x, y;
    placement(cr.right - cr.left, cr.bottom - cr.top, bmp_, &x, &y);
    // premultiplied copy for UpdateLayeredWindow
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = bmp_.w;
    bi.bmiHeader.biHeight = -bmp_.h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC sdc = GetDC(nullptr), mdc = CreateCompatibleDC(sdc);
    void* bits = nullptr;
    HBITMAP bm = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bm && bits) {
        uint32_t* d = (uint32_t*)bits;
        for (size_t i = 0; i < bmp_.px.size(); i++) {
            uint32_t p = bmp_.px[i];
            uint32_t a = p >> 24;
            d[i] = (a << 24) | ((((p >> 16) & 255) * a / 255) << 16) | ((((p >> 8) & 255) * a / 255) << 8) | ((p & 255) * a / 255);
        }
        HGDIOBJ old = SelectObject(mdc, bm);
        POINT dst{tl.x + x, tl.y + y}, src{0, 0};
        SIZE sz{bmp_.w, bmp_.h};
        BLENDFUNCTION bf{AC_SRC_OVER, 0, (BYTE)std::lround(alpha_at(el) / kMaxAlpha * 255.f), AC_SRC_ALPHA};
        UpdateLayeredWindow(hwnd_, sdc, &dst, &sz, mdc, &src, 0, &bf, ULW_ALPHA);   // position, size and opacity in one call; the z-order is left alone
        SelectObject(mdc, old);
        DeleteObject(bm);
    }
    DeleteDC(mdc);
    ReleaseDC(nullptr, sdc);
    MSG msg;
    while (PeekMessageW(&msg, hwnd_, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}

}  // namespace starthint
