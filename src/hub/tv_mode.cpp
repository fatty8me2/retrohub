/* TV mode (L5): borderless fullscreen + larger UI scale + hero/sidebar preset. Not a second UI: it only changes ui::S, the window
 * and a few layout numbers; pad navigation and the OSK are untouched. */
#include "app.h"
#include "../net/username.h"
#include "tv_mode.h"
#include "tv_logic.h"
#include "friends_ui.h"
#include "popup_guard.h"

namespace tvmode {

static bool g_active = false;
static int g_pending = -1;           // -1 none, 0 leave, 1 enter
static float g_base_s = 1.f;
static hubtv::WinRect g_saved;
static bool g_headless_mode = false;
static int g_shot_h = 0;
static bool g_prev_game = false;

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunName = L"RetroHub";

bool active() { return g_active; }
bool force_icon_sidebar() { return g_active; }

static void save_rect(SDL_Window* win) {
    if (!win || g_headless_mode) return;
    Uint32 fl = SDL_GetWindowFlags(win);
    if (fl & SDL_WINDOW_FULLSCREEN_DESKTOP) return;   // already fullscreen: keep what we saved
    g_saved.maximized = (fl & SDL_WINDOW_MAXIMIZED) != 0;
    /* Restore-then-maximize only to read the normal rect; never while a game is up (the maximized rect is saved instead). */
    const bool can_restore = g_saved.maximized && !game_active() && hub_has_focus();
    if (can_restore) SDL_RestoreWindow(win);    // popup-guard: ok (guarded: no game, hub focused) read the normal rect, then maximize again on leave
    SDL_GetWindowPosition(win, &g_saved.x, &g_saved.y);
    SDL_GetWindowSize(win, &g_saved.w, &g_saved.h);
    g_saved.valid = true;
    if (can_restore) SDL_MaximizeWindow(win);
}

static int display_h(SDL_Window* win) {
    if (g_headless_mode) return g_shot_h;
    int di = win ? SDL_GetWindowDisplayIndex(win) : 0;
    SDL_Rect r;
    if (di < 0) di = 0;
    return SDL_GetDisplayBounds(di, &r) == 0 ? r.h : 1080;
}

float init(SDL_Window* win, bool headless, float base_scale, bool cmd_flag, bool start_setting, int shot_h) {
    g_headless_mode = headless;
    g_base_s = base_scale;
    g_shot_h = shot_h;
    if (!hubtv::start_in_tv(cmd_flag, start_setting)) return base_scale;
    g_active = true;
    if (!headless && win) {
        save_rect(win);
        if (!game_active())   // started while a game is up (autostart / update relaunch): no fullscreen grab
            SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);   // popup-guard: ok (guarded by game_active)
    }
    return hubtv::tv_scale(base_scale, display_h(win));
}

void toggle() { g_pending = g_active ? 0 : 1; }

void handle_event(const SDL_Event& e, bool want_text) {
    if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F11 && !e.key.repeat && !want_text) toggle();
}

static void rebuild_fonts(float s) {
    ui::S = s;
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ui::setup_fonts(s);
    ui::setup_theme(s);
    ImGui_ImplSDLRenderer2_DestroyDeviceObjects();   // the next NewFrame rebuilds the font texture
}

void frame_begin(SDL_Window* win, SDL_Renderer*) {
    if (g_pending < 0) return;
    const bool on = g_pending == 1;
    g_pending = -1;
    if (on == g_active) return;
    g_active = on;
    if (on) {
        save_rect(win);
        if (!g_headless_mode) SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);   // popup-guard: ok (F11 pressed in the focused hub)
        rebuild_fonts(hubtv::tv_scale(g_base_s, display_h(win)));
    } else {
        if (!g_headless_mode) {
            SDL_SetWindowFullscreen(win, 0);   // popup-guard: ok (F11 pressed in the focused hub)
            if (g_saved.valid) {
                SDL_SetWindowSize(win, g_saved.w, g_saved.h);
                SDL_SetWindowPosition(win, g_saved.x, g_saved.y);
                if (g_saved.maximized) SDL_MaximizeWindow(win);
            }
        }
        rebuild_fonts(g_base_s);
    }
    if (win) SDL_SetWindowMinimumSize(win, (int)(960 * g_base_s), (int)(600 * g_base_s));
}

void tick(SDL_Window* win, bool game_running) {
    /* Game closed: NEVER raise/restore the hub (it must not jump over whatever the user does next). Only re-assert fullscreen when the
     * hub already has the focus; otherwise it stays as it is and goes fullscreen when the user opens it. */
    if (g_prev_game && !game_running && g_active && !g_headless_mode && win && hubguard::may_restore_after_exit(hub_has_focus()))
        SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);   // popup-guard: ok (hub already focused)
    g_prev_game = game_running;
}

float hero_h(float normal, float avail_h) {
    return hubtv::hero_height(g_active, normal, avail_h, ImGui::GetIO().DisplaySize.y);
}

void draw_corner(float right_x, float top_y, float h) {
    if (!g_active) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    std::time_t t = std::time(nullptr);
    std::tm lt{};
    localtime_s(&lt, &t);
    std::string clk = hubtv::clock_text(lt.tm_hour, lt.tm_min);
    ImFont* f = ui::F.h2;
    ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.f, clk.c_str());
    float x = right_x - ts.x;
    dl->AddText(f, f->FontSize, ImVec2(x, top_y + (h - ts.y) * 0.5f), ui::TEXT, clk.c_str());
    // online friends
    std::vector<const hubnet::Friend*> on;
    for (auto& fr : fr_friends()) if (fr.online()) on.push_back(&fr);
    hubtv::AvatarPlan pl = hubtv::avatar_plan((int)on.size(), 5);
    float d = h * 0.9f, r = d * 0.5f, step = d * 0.78f;
    float ax = x - sp(2.f) - d;
    float cy = top_y + h * 0.5f;
    auto bubble = [&](float bx, ImU32 col, const char* txt) {
        ImVec2 c(bx + r, cy);
        dl->AddCircleFilled(c, r, col, 24);
        dl->AddCircle(c, r, ui::SIDEBAR, 24, 2.f);
        ImVec2 s2 = ui::F.bold->CalcTextSizeA(ui::F.bold->FontSize, FLT_MAX, 0.f, txt);
        dl->AddText(ui::F.bold, ui::F.bold->FontSize, c - s2 * 0.5f, IM_COL32_WHITE, txt);
    };
    if (pl.extra > 0) {
        std::string m = "+" + std::to_string(pl.extra);
        bubble(ax, ui::PANEL3, m.c_str());
        ax -= step;
    }
    for (int i = pl.shown - 1; i >= 0; --i) {
        const hubnet::Friend& fr = *on[i];
        size_t hsh = std::hash<std::string>()(fr.uid);
        float hr, hg, hb;
        ImGui::ColorConvertHSVtoRGB((float)(hsh % 360) / 360.f, 0.55f, 0.75f, hr, hg, hb);
        char init[8] = {hubnet::username_initial(fr.name), 0};
        bubble(ax, IM_COL32((int)(hr * 255), (int)(hg * 255), (int)(hb * 255), 255), init);
        ax -= step;
    }
}

/* ---- Start with Windows (HKCU Run key, off by default) ---- */
static bool autostart_enabled() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return false;
    bool ok = RegQueryValueExW(k, kRunName, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}
static bool set_autostart(bool on) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    bool ok;
    if (on) {
        wchar_t exe[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring v = hubtv::run_value(std::wstring(exe, n));
        ok = RegSetValueExW(k, kRunName, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    } else {
        LONG r = RegDeleteValueW(k, kRunName);
        ok = r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
    }
    RegCloseKey(k);
    return ok;
}

void settings_ui() {
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    settings_label("TV mode");
    bool a = g_active;
    if (ImGui::Checkbox("TV mode (F11)", &a)) toggle();
    if (ImGui::Checkbox("Start RetroHub in TV mode", &A.st.tv_start)) mark_dirty();
    bool w = autostart_enabled();
    if (ImGui::Checkbox("Start with Windows into TV mode", &w)) {
        if (!g_headless_mode && set_autostart(w)) { A.st.tv_windows = w; mark_dirty(); }
    }
    text_wrapped_disabled("A fullscreen 10-foot layout with a larger UI, a taller hero, an icon sidebar, and the clock and online friends in the corner. "
                          "Controller navigation and the on-screen keyboard work as usual. Press F11 to leave.");
}

}  // namespace tvmode
