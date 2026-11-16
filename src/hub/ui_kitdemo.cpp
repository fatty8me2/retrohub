#include "app.h"
#include "ui_controls.h"
#include "ui_sheet.h"

/* Headless demo of the UI kit (screenshots only): HUB_SHOT_KITDEMO=sheet|pane|dialog, optional HUB_SHOT_KITTAB=0..3 (default 3,
 * Settings). Fake content only; nothing is saved, launched or sent. See docs/QA_SHOTS.md. */

namespace {

enum class Mode { Off, Sheet, Pane, Dialog };
Mode g_mode = Mode::Off;
bool g_open = false, g_ask = false;
int g_tab = 3, g_pick = 4;

/* fake per-game settings */
int v_res = 2, v_fps = 1, v_ahead = 1, v_aspect = 0, v_filter = 2, v_ff = 2, v_rwlen = 1, v_states = 0, v_oc = 0, v_cache = 1, v_lat = 64;
bool v_wide = true, v_fc = false, v_rewind = true, v_turbo = false, v_rumble = true, v_exp = true;

const char* kTabs[] = {"About", "Saves", "Screenshots (3)", "Settings"};
const char* kRes[] = {"Match window", "Native (1x)", "2x (720p)", "3x (1080p)", "4x (1440p)"};
const char* kFps[] = {"Original 30", "Smooth 60 (steady 30 + frame generation)"};
const char* kAspect[] = {"Original 4:3", "Stretch to window", "Widescreen 16:9"};
const char* kFilter[] = {"None", "Sharp pixels", "CRT scanlines", "LCD grid", "Soft"};
const char* kFF[] = {"2x", "3x", "4x", "6x", "8x"};
const char* kRw[] = {"10 seconds", "30 seconds", "1 minute", "2 minutes"};
const char* kStates[] = {"Ask before overwriting", "Always overwrite", "Keep every save (numbered)"};
const char* kOC[] = {"Off", "1.5x", "2x", "3x"};
const char* kCache[] = {"Small", "Normal", "Large (uses more memory)"};
const char* kGames[] = {"Star Courier 64", "Bumper Kart Deluxe", "Pocket Monsters Gold", "Island Party 2", "Tennis Aces", "Dino Valley",
                        "Sky Racer Turbo", "Castle Quest", "Puzzle Drop", "Wave Rider", "Ghost Manor", "Ninja Path", "Rally Cross",
                        "Moon Base Alpha", "Super Strikers", "Jungle Jam", "Robot Arena", "Cosmic Golf", "Pixel Knights", "Ocean Odyssey",
                        "Night Drive", "Block Builders", "Hero Academy", "Lava Run"};
const char* kSys[] = {"Nintendo 64", "Nintendo 64", "Game Boy Color", "Nintendo 64", "GameCube", "SNES"};

void header() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool compact = ImGui::GetIO().DisplaySize.y < sp(95.f);   // short window or big UI size: the body keeps most of the height
    const float art = compact ? sp(7.f) : sp(12.f), bh = compact ? sp(4.5f) : sp(5.f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float avail = ImGui::GetContentRegionAvail().x;
    ui::gradient_round_rect(dl, p, p + ImVec2(art * kTileAspect, art), ui::ACCENT, mix(ui::ACCENT, ui::PANEL3, 0.6f), sp(1.5f));
    ImFont* ft = compact ? ui::F.h2 : ui::F.title;
    const ImVec2 ini = ft->CalcTextSizeA(ft->FontSize, FLT_MAX, 0.f, "SC");
    dl->AddText(ft, ft->FontSize, p + ImVec2((art * kTileAspect - ini.x) * 0.5f, (art - ini.y) * 0.5f), with_alpha(IM_COL32_WHITE, 0.8f), "SC");
    const float tx = art * kTileAspect + sp(2.5f);
    const float btn_w = sp(17.f) + ui::button_width("Play Online", ui::Icon::Globe) + ui::button_width("Favorite", ui::Icon::Star) + sp(3.f);
    const bool side = compact && tx + sp(28.f) + btn_w <= avail;   // compact and wide enough: the buttons sit on the title's line
    ImGui::SetCursorScreenPos(p + ImVec2(tx, side ? (art - ft->FontSize - ImGui::GetFontSize() - sp(0.5f)) * 0.5f : 0.f));
    ImGui::BeginGroup();
    ImGui::PushFont(ft);
    ImGui::TextUnformatted("Star Courier 64");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
    ImGui::TextUnformatted("Nintendo 64  \xC2\xB7  1-4 players  \xC2\xB7  Online  \xC2\xB7  1998");
    ImGui::PopStyleColor();
    float bottom = p.y + art;
    if (side) {
        ImGui::EndGroup();
        bottom = std::max(bottom, ImGui::GetItemRectMax().y);
        ImGui::SetCursorScreenPos(ImVec2(p.x + avail - btn_w, p.y + (art - bh) * 0.5f));
        ImGui::BeginGroup();
    } else ImGui::Dummy(ImVec2(0.f, sp(0.25f)));
    ui::FlowRow row;
    row.next(sp(17.f));
    ui::button("Play", ImVec2(sp(17.f), bh), true, true, ui::Icon::Play);
    ImGui::SetItemDefaultFocus();
    row.next(ui::button_width("Play Online", ui::Icon::Globe));
    ui::button("Play Online", ImVec2(0, bh), false, true, ui::Icon::Globe);
    row.next(ui::button_width("Favorite", ui::Icon::Star));
    ui::button("Favorite", ImVec2(0, bh), false, true, ui::Icon::Star);
    ImGui::EndGroup();
    bottom = std::max(bottom, ImGui::GetItemRectMax().y);
    ImGui::SetCursorScreenPos(ImVec2(p.x, bottom + sp(compact ? 1.f : 1.5f)));
    ui::tab_bar("game_tabs", kTabs, 4, &g_tab);
}

void body() {
    if (g_tab == 0) {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextUnformatted("Deliver parcels across a cartoon solar system in this fast arcade racer. Four friends can race on one screen, "
                               "or online in a room.");
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
        for (int i = 0; i < 6; ++i)
            ImGui::TextUnformatted("Every course hides three shortcut gates. Finding them all unlocks the mirror cups and a secret courier.");
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        return;
    }
    if (g_tab == 1) {
        ImGui::TextUnformatted("Save data: 1 file, 2 KB   3 save states + resume point");
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
        ImGui::TextUnformatted("Synced with your other PCs 4 minutes ago.");
        ImGui::PopStyleColor();
        ui::button("Save history", ImVec2(0, 0), false, true, ui::Icon::Cloud);
        return;
    }
    if (g_tab == 2) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float w = sp(24.f), h = w * 9.f / 16.f;
        for (int i = 0; i < 3; ++i) {
            if (i) ImGui::SameLine();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ui::gradient_round_rect(dl, p, p + ImVec2(w, h), mix(ui::PANEL3, ui::ACCENT, 0.25f * (i + 1)), ui::PANEL2, sp(1.25f));
            ImGui::Dummy(ImVec2(w, h));
        }
        return;
    }
    ui::option_group_begin("kitdemo_settings");
    if (ui::section("Graphics")) {
        ui::RowOpts o;
        o.help = "Higher looks sharper and needs a faster PC.";
        ui::dropdown("Internal resolution", &v_res, kRes, 5, o);
        ui::RowOpts f;
        f.pill = "overridden";
        ui::dropdown("Frame rate", &v_fps, kFps, 2, f);
        ui::RowOpts a;
        a.help = "Shows each frame a little early to cut input lag. 0 is off.";
        ui::stepper_int("Frames ahead (smoother on some games)", &v_ahead, 0, 4, 1, "%d", a);
        ui::dropdown("Aspect ratio", &v_aspect, kAspect, 3);
        ui::dropdown("Screen filter", &v_filter, kFilter, 5);
        ui::RowOpts w;
        w.help = "Shows more of the world. Some menus may look stretched.";
        ui::toggle("Widescreen hack", &v_wide, w);
        ui::toggle("Show frame counter", &v_fc);
    }
    if (ui::section("Gameplay")) {
        ui::stepper("Fast-forward speed", &v_ff, kFF, 5);
        ui::RowOpts r;
        r.pill = "host decides online";
        ui::toggle("Rewind", &v_rewind, r);
        ui::RowOpts rl;
        rl.enabled = v_rewind;
        ui::stepper("Rewind length", &v_rwlen, kRw, 4, rl);
        ui::toggle("Turbo buttons", &v_turbo);
        ui::toggle("Controller rumble", &v_rumble);
        ui::dropdown("Save states", &v_states, kStates, 3);
    }
    ui::SectionOpts adv;
    adv.default_open = false;
    adv.summary = "4 options";
    if (ui::section("Advanced", adv)) {
        ui::dropdown("CPU overclock (may break timing in some games)", &v_oc, kOC, 4);
        ui::dropdown("Texture cache", &v_cache, kCache, 3);
        ui::stepper_int("Audio latency", &v_lat, 32, 256, 32, "%d ms");
        ui::toggle("Expansion Pak", &v_exp);
    }
    ui::option_group_end();
}

void reset_dialog() {
    ui::DialogOpts o;
    o.subtitle = "Star Courier 64";
    if (ui::dialog_begin("kitdemo_reset", "Reset this game's settings?", &g_ask, ui::DialogSize::Medium, o)) {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextUnformatted("Every setting on this page goes back to the default for Nintendo 64 games. Your saves, save states, "
                               "screenshots and play time stay as they are.");
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
        const char* what[] = {"Internal resolution, frame rate and frames ahead", "Aspect ratio and screen filter", "Widescreen hack and frame counter",
                              "Fast-forward speed, rewind and turbo buttons", "Controller rumble and save state behaviour",
                              "CPU overclock, texture cache and audio latency", "The Expansion Pak choice"};
        for (const char* w : what) ImGui::BulletText("%s", w);
        ImGui::TextUnformatted("Settings you changed for all Nintendo 64 games in Settings stay as they are, and other games are not touched.");
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ui::dialog_buttons();
        if (ui::button("Reset", ImVec2(0, 0), true)) g_ask = false;
        ImGui::SameLine();
        if (ui::button("Cancel")) g_ask = false;
        ImGui::SetItemDefaultFocus();
        ui::dialog_end();
    }
}

void footer() {
    static const ui::KeyHint sheet_hints[] = {{"A", "Enter", "Select"}, {"B", "Esc", "Close"}, {"LB", "", "Tabs"}, {"LT", "", "Page up"}, {"RT", "", "Page down"}};
    static const ui::KeyHint pane_hints[] = {{"A", "Enter", "Select"}, {"LB", "", "Tabs"}, {"LT", "", "Page up"}, {"RT", "", "Page down"}};
    const ui::KeyHint* hints = g_mode == Mode::Pane ? pane_hints : sheet_hints;
    const int nh = g_mode == Mode::Pane ? 4 : 5;
    const float hw = ui::hint_bar_width(hints, nh);
    const float x_end = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    ui::FlowRow row;
    row.next(ui::button_width("Reset to defaults", ui::Icon::Refresh));
    if (ui::button("Reset to defaults", ImVec2(0, sp(4.5f)), false, true, ui::Icon::Refresh)) g_ask = true;
    if (g_mode != Mode::Pane) {
        row.next(ui::button_width("Close", ui::Icon::Close));
        if (ui::button("Close", ImVec2(0, sp(4.5f)), false, true, ui::Icon::Close)) g_open = false;
    }
    /* hints at the right end of the button row (as many as fit: a short window keeps its height for the body) */
    const ImVec2 last = ImGui::GetItemRectMax();
    const float hy = ImGui::GetItemRectMin().y + (sp(4.5f) - ui::F.cap->FontSize - sp(0.75f)) * 0.5f;
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(std::max(last.x + sp(3.f), x_end - hw), hy));
    ui::hint_bar(hints, nh);
    reset_dialog();   // opened from inside the sheet, so it stacks on top of it
}

ui::SheetParts parts() {
    ui::SheetParts p;
    p.header = header;
    p.body = body;
    p.footer = footer;
    return p;
}

void desk() {
    ImGuiIO& io = ImGui::GetIO();
    static bool first = true;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    if (first) { ImGui::SetNextWindowFocus(); first = false; }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(3.f), sp(3.f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::Begin("##kitdemo_desk", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    const float pad = sp(3.f), gap = sp(2.f);
    const float list_w = std::max(sp(28.f), std::min(sp(44.f), io.DisplaySize.x * 0.3f));
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float h = io.DisplaySize.y - pad * 2.f;
    /* left: the game list (a kit scroll area) */
    ImGui::BeginGroup();
    ImGui::PushFont(ui::F.h2);
    ImGui::TextUnformatted("Library");
    ImGui::PopFont();
    if (ui::scroll_begin("desk_list", ImVec2(list_w, h - ImGui::GetCursorScreenPos().y + o.y))) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (int i = 0; i < (int)(sizeof kGames / sizeof *kGames); ++i) {
            ImGui::PushID(i);
            const float rh = sp(6.f);
            const bool pr = ImGui::InvisibleButton("##g", ImVec2(ImGui::GetContentRegionAvail().x, rh), ImGuiButtonFlags_EnableNav);
            const bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && io.NavVisible;
            ui::item_sound(foc, pr);
            if (pr) g_pick = i;
            const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
            if (i == g_pick) dl->AddRectFilled(p0, p1, with_alpha(ui::ACCENT, 0.22f), sp(1.25f));
            else if (hov || foc) dl->AddRectFilled(p0, p1, ui::veil(0.06f), sp(1.25f));
            dl->AddText(ui::F.bold, ui::F.bold->FontSize, p0 + ImVec2(sp(1.5f), sp(0.75f)), ui::TEXT, kGames[i]);
            dl->AddText(ui::F.cap, ui::F.cap->FontSize, p0 + ImVec2(sp(1.5f), sp(0.75f) + ui::F.bold->FontSize + sp(0.25f)), ui::TEXT2, kSys[i % 6]);
            if (foc) ui::focus_ring(dl, p0 + ImVec2(sp(0.25f), sp(0.25f)), p1 - ImVec2(sp(0.25f), sp(0.25f)), sp(1.25f));
            ImGui::PopID();
        }
    }
    ui::scroll_end();
    ImGui::EndGroup();
    /* right: the selected game's page in a pane */
    ui::SheetOpts so;
    so.body_key = kTabs[g_tab];
    ui::sheet("kitdemo_pane", ui::SheetHost::Pane, ImVec2(o.x + list_w + gap, o.y), ImVec2(io.DisplaySize.x - pad - (o.x + list_w + gap), h), nullptr, parts(), so);
    ImGui::End();
}

}  // namespace

/* HUB_SHOT_KITDEMO (shot.cpp): false = unknown mode (the shot exits 2). */
bool uikit_demo_init(const char* mode) {
    if (!g_headless || !mode) return true;
    const std::string m = mode;
    if (m == "sheet") g_mode = Mode::Sheet;
    else if (m == "pane") g_mode = Mode::Pane;
    else if (m == "dialog") { g_mode = Mode::Dialog; g_ask = true; }
    else {
        std::fprintf(stderr, "HUB_SHOT_KITDEMO: unknown mode '%s' (sheet, pane, dialog); no screenshot taken\n", mode);
        return false;
    }
    g_open = true;
    if (const char* t = std::getenv("HUB_SHOT_KITTAB")) g_tab = std::max(0, std::min(3, std::atoi(t)));
    return true;
}

/* draw_overlays(): every frame; nothing unless the demo was asked for in a headless run. */
void uikit_demo_draw() {
    if (g_mode == Mode::Off || !g_headless) return;
    /* Open only once the library has drawn and seeded its own focus: page_library's seed_nav_here() sets the nav id even while a
     * modal is open, which would leave the sheet's D-pad focus on a library item behind it. */
    static int settle = 0;
    if (!A.scanned_once || A.nav_seed || ++settle < 3) return;
    if (g_mode == Mode::Pane) { desk(); return; }
    ui::SheetOpts so;
    so.body_key = kTabs[g_tab];
    ui::sheet("kitdemo_page", ui::SheetHost::Overlay, ImVec2(0, 0), ImGui::GetIO().DisplaySize, &g_open, parts(), so);
    if (!g_open) g_mode = Mode::Off;
}
