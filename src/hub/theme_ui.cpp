/* Themes: owns the active choice, applies it to the ui:: palette, and draws the Settings picker.
 * Pure data and colour math live in theme.h (tested by hub_theme_test). */
#include "app.h"
#include "prefs.h"
#include "cloud_engine.h"
#include "art.h"

namespace {
hubtheme::Choice g_choice;        // saved choice
int g_preview = -1;               // preset hovered/focused in the picker this frame (-1 none)
int g_applied_preset = -1;
uint32_t g_cover_raw = 0;         // average colour of the hero's cover (0 = unknown)
SDL_Texture* g_cover_tex = nullptr;
float g_acc[3] = {-1.f, 0.f, 0.f};   // animated accent (rgb 0..255), -1 = unset
uint32_t g_applied_accent = 0;

hubtheme::Palette effective(int preset, bool cover) {
    hubtheme::Palette p = hubtheme::preset(preset);
    if (cover && g_cover_raw) p.c[hubtheme::S_ACCENT] = hubtheme::readable_accent(g_cover_raw, p.c[hubtheme::S_BG]);
    return p;
}

void persist() {
    const std::string root = hub::saves::vault_dir();
    hub::prefs::Prefs p;
    hub::prefs::load(root, &p);
    if (hub::prefs::set_theme(&p, hubtheme::format_choice(g_choice)) && hub::prefs::save(root, p) && !g_headless)
        hub::saves::cloud_account_prefs_changed();
}
}  // namespace

void theme_init() {
    std::string s;
    if (const char* e = std::getenv("HUB_THEME"); e && *e) s = e;
    else {
        hub::prefs::Prefs p;
        hub::prefs::load(hub::saves::vault_dir(), &p);
        s = p.theme;
    }
    g_choice = hubtheme::parse_choice(s);
    g_applied_preset = -1;
}

void theme_hero_art(SDL_Texture* tex) {
    if (tex == g_cover_tex) return;
    g_cover_tex = tex;
    uint32_t c = hubart::average_color(tex);
    if (c) g_cover_raw = c;
}

void theme_tick() {
    static double last_poll = 0;
    if (!g_headless && !std::getenv("HUB_THEME") && ImGui::GetTime() - last_poll > 5.0) {   // another PC may have changed the synced choice
        last_poll = ImGui::GetTime();
        hub::prefs::Prefs p;
        if (hub::prefs::load(hub::saves::vault_dir(), &p) && !p.theme.empty()) g_choice = hubtheme::parse_choice(p.theme);
    }
    int eff = g_preview >= 0 ? g_preview : g_choice.preset;
    g_preview = -1;
    hubtheme::Palette p = effective(eff, g_choice.cover);
    uint32_t want = p.c[hubtheme::S_ACCENT];
    float dt = ImGui::GetIO().DeltaTime;
    bool first = g_acc[0] < 0.f;
    float k = g_headless || first ? 1.f : 1.f - std::exp(-dt * 6.f);
    float tgt[3] = {(float)hubtheme::cr(want), (float)hubtheme::cg(want), (float)hubtheme::cb(want)};
    for (int i = 0; i < 3; ++i) g_acc[i] = first ? tgt[i] : g_acc[i] + (tgt[i] - g_acc[i]) * k;
    uint32_t cur = hubtheme::rgba((int)(g_acc[0] + 0.5f), (int)(g_acc[1] + 0.5f), (int)(g_acc[2] + 0.5f));
    if (eff == g_applied_preset && cur == g_applied_accent) return;
    p.c[hubtheme::S_ACCENT] = cur;
    ui::apply_palette(p);
    g_applied_preset = eff;
    g_applied_accent = cur;
}

void theme_settings_row() {
    using namespace hubtheme;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    settings_label("Theme");
    float cw = sp(15.f), ch = sp(9.5f), gap = sp(1.f);
    float avail = ImGui::GetContentRegionAvail().x;
    int per_row = std::max(1, (int)((avail + gap) / (cw + gap)));
    for (int i = 0; i < P_COUNT; ++i) {
        if (i % per_row) ImGui::SameLine(0, gap);
        Palette pal = effective(i, g_choice.cover && i == g_choice.preset);
        ImGui::PushID(i);
        ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + cw, p0.y + ch);
        bool clicked = ImGui::InvisibleButton("theme", ImVec2(cw, ch));
        bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        if (hot) g_preview = i;
        if (clicked) { g_choice.preset = i; persist(); }
        bool sel = i == g_choice.preset;
        float r = sp(1.25f);
        dl->AddRectFilled(p0, p1, pal.c[S_BG], r);
        dl->AddRectFilled(p0, ImVec2(p0.x + cw * 0.22f, p1.y), pal.c[S_SIDEBAR], r, ImDrawFlags_RoundCornersLeft);
        ImVec2 q0(p0.x + cw * 0.28f, p0.y + ch * 0.14f), q1(p1.x - cw * 0.06f, p0.y + ch * 0.48f);
        dl->AddRectFilled(q0, q1, pal.c[S_PANEL], sp(0.6f));
        dl->AddText(ImVec2(q0.x + sp(0.6f), q0.y + sp(0.3f)), pal.c[S_TEXT], "Aa");
        dl->AddRectFilled(ImVec2(q0.x, q1.y + ch * 0.06f), ImVec2(q0.x + cw * 0.3f, q1.y + ch * 0.2f), pal.c[S_ACCENT], sp(0.5f));
        dl->AddRectFilled(ImVec2(q0.x + cw * 0.34f, q1.y + ch * 0.06f), ImVec2(q1.x, q1.y + ch * 0.2f), pal.c[S_PANEL3], sp(0.5f));
        dl->AddText(ImVec2(p0.x + sp(0.8f), p1.y - ui::F.cap->FontSize - sp(0.4f)), pal.c[S_TEXT2], preset_name(i));
        dl->AddRect(p0, p1, sel ? ui::ACCENT : ui::HAIRLINE, r, 0, sel ? 2.f * S : 1.f);
        if (ImGui::IsItemFocused()) ui::focus_ring(dl, p0, p1, r);
        ImGui::PopID();
    }
    if (ImGui::Checkbox("Accent from cover", &g_choice.cover)) persist();
    text_wrapped_disabled("Tints the highlight colour from the cover of the game on the Home hero. Hover a theme to preview it.");
}
