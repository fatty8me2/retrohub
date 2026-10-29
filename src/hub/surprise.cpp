/* Surprise me (Wave 3 L7). A dice button in the Home top bar picks a random OWNED game that has not been played for 14+ days
 * (weighted toward never-played games and favorites; see surprise_logic.h), spins a short slot-machine reel of covers and
 * then opens that game's details. A / Enter skips the spin, B / Esc cancels.
 * Screenshot aid: HUB_SHOT_SURPRISE=<seed> starts one spin at ~1.5 s (headless only). */
#include "app.h"
#include "homefun.h"
#include "surprise_logic.h"

namespace {

namespace su = hubui::surprise;

constexpr double kSpinSecs = 2.7;   // reel runs this long
constexpr double kHoldSecs = 1.1;   // the winner stays up this long, then its details open
constexpr int kReelLen = 26;

struct Spin {
    enum Ph { Idle, Spinning, Landed } ph = Idle;
    bool want_open = false;
    std::vector<std::string> keys;   // candidate game keys (index = candidate index)
    std::vector<int> reel;
    int target = -1;
    double t0 = 0, landed_at = 0;
    bool all_recent = false;
    int64_t last_played = 0;
    std::string last_key;   // not suggested twice in a row
} SU;

std::mt19937& rng() {
    static std::mt19937 g = [] {
        const char* s = g_headless ? std::getenv("HUB_SHOT_SURPRISE") : nullptr;
        if (s && *s) return std::mt19937((unsigned)std::atoi(s));
        std::random_device rd;
        return std::mt19937(rd());
    }();
    return g;
}

/* Games that can be suggested: owned and launchable (a game that still needs its files is skipped). */
void candidates(std::vector<su::Cand>* cs, std::vector<std::string>* keys) {
    for (auto& e : A.lib) {
        if (!e.issue.empty() && e.issue != "extract") continue;
        const hub::GameState& gs = gstate(e.key);
        su::Cand c;
        c.key = e.key;
        c.last_played = (int64_t)gs.last_played;
        c.favorite = gs.favorite;
        cs->push_back(c);
        keys->push_back(e.key);
    }
}

bool start_spin() {
    std::vector<su::Cand> cs;
    std::vector<std::string> keys;
    candidates(&cs, &keys);
    if (cs.empty()) { push_toast("Add some games first, then I can pick one for you.", ui::ACCENT, 5.0); return false; }
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    const su::Pick p = su::pick(cs, (int64_t)unix_now(), uni(rng()), SU.last_key);
    if (p.index < 0) return false;
    SU.keys = keys;
    SU.target = p.index;
    SU.reel = su::make_reel((int)keys.size(), p.index, kReelLen, rng()());
    SU.all_recent = p.all_recent;
    SU.last_played = cs[p.index].last_played;
    SU.last_key = keys[p.index];
    SU.ph = Spin::Spinning;
    SU.t0 = ImGui::GetTime();
    SU.want_open = true;
    shot_trace("surprise", "pick " + keys[p.index] + (p.all_recent ? " all-recent" : ""));
    return true;
}

void draw_cover(ImDrawList* dl, const hub::GameEntry& e, ImVec2 p0, ImVec2 p1, float rad, float alpha) {
    ArtRef art = art_for_entry(e);
    TileSpec ts;
    ts.title = e.title;
    entry_colors(e, &ts.c0, &ts.c1);
    ui::shadow(dl, p0, p1, rad, 1.0f * alpha);
    if (art.tex) ui::draw_art(dl, art.tex, art.w, art.h, p0, p1, rad, 1.f, alpha);
    else draw_no_art(dl, ts, p0, p1, rad, 1.f);
    dl->AddRect(p0, p1, ui::scale_alpha(ui::HAIRLINE, alpha), rad, 0, 1.f);
}

void finish(bool open_details) {
    if (open_details && SU.target >= 0 && SU.target < (int)SU.keys.size()) {
        const std::string key = SU.keys[SU.target];
        if (find_entry(key)) {
            A.sel_game = key;
            A.sel_ghost.clear();
            A.show_details = true;
            shot_trace("surprise", "open " + key);
        }
    } else shot_trace("surprise", "cancel");
    SU.ph = Spin::Idle;
    ImGui::CloseCurrentPopup();
}

void draw_popup() {
    if (SU.ph == Spin::Idle) return;
    ImGuiIO& io = ImGui::GetIO();
    const float w = std::min(sp(96.f), io.DisplaySize.x * 0.92f);
    ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(3.f), sp(3.f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.5f));
    const bool open = ImGui::BeginPopupModal("##surprise", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    if (!open) { SU.ph = Spin::Idle; return; }

    const double now = ImGui::GetTime();
    double t = now - SU.t0;
    const bool skip = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false);
    if (ui::modal_back()) { finish(false); ImGui::EndPopup(); return; }
    if (SU.ph == Spin::Spinning && skip) t = kSpinSecs;
    if (SU.ph == Spin::Spinning && t >= kSpinSecs) { SU.ph = Spin::Landed; SU.landed_at = now - std::max(0.0, t - kSpinSecs); }
    if (SU.ph == Spin::Landed && (now - SU.landed_at >= kHoldSecs || (skip && now - SU.landed_at > 0.15))) { finish(true); ImGui::EndPopup(); return; }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushFont(ui::F.title);
    ImGui::TextUnformatted("Surprise me");
    ImGui::PopFont();
    const hub::GameEntry* winner = SU.target >= 0 && SU.target < (int)SU.keys.size() ? find_entry(SU.keys[SU.target]) : nullptr;
    std::string line = "Spinning the covers...";
    if (SU.ph == Spin::Landed && winner) {
        if (SU.all_recent) line = "You've played everything lately, so here is one anyway.";
        else if (SU.last_played <= 0) line = "You haven't played this one yet.";
        else {
            const int64_t days = ((int64_t)unix_now() - SU.last_played) / 86400;
            line = days >= 60 ? "You haven't played this in " + std::to_string(days / 30) + " months." : "You haven't played this in " + std::to_string(days) + " days.";
        }
    }
    text_wrapped_disabled(line);
    ImGui::Dummy(ImVec2(0, sp(1.f)));

    // the reel
    const float avail = ImGui::GetContentRegionAvail().x;
    const float cw = std::min(sp(15.f), avail / 4.2f), ch = cw / kTileAspect, gap = sp(2.f);
    const float vh = ch * 1.35f;
    const ImVec2 v0 = ImGui::GetCursorScreenPos(), v1(v0.x + avail, v0.y + vh);
    const float cx = (v0.x + v1.x) * 0.5f, cy = (v0.y + v1.y) * 0.5f;
    dl->AddRectFilled(v0, v1, ui::BG, sp(1.5f));
    dl->PushClipRect(v0, v1, true);
    const float pos = SU.ph == Spin::Spinning ? su::spin_pos(t, kSpinSecs, (int)SU.reel.size()) : (float)((int)SU.reel.size() - 1);
    const int first = std::max(0, (int)std::floor(pos - avail / (cw + gap) * 0.5f) - 1);
    const int last = std::min((int)SU.reel.size() - 1, (int)std::ceil(pos + avail / (cw + gap) * 0.5f) + 1);
    for (int i = first; i <= last; ++i) {
        const float d = (float)i - pos;
        const float nearness = std::max(0.f, 1.f - std::fabs(d));
        const float sc = 1.f + 0.28f * nearness;
        const float x = cx + d * (cw + gap);
        const ImVec2 p0(x - cw * sc * 0.5f, cy - ch * sc * 0.5f), p1(x + cw * sc * 0.5f, cy + ch * sc * 0.5f);
        const int ci = SU.reel[(size_t)i];
        if (ci < 0 || ci >= (int)SU.keys.size()) continue;
        const hub::GameEntry* e = find_entry(SU.keys[ci]);
        if (!e) continue;
        draw_cover(dl, *e, p0, p1, sp(1.5f), 0.55f + 0.45f * nearness);
        if (SU.ph == Spin::Landed && i == (int)SU.reel.size() - 1) {
            const float pulse = 0.5f + 0.5f * std::sin((float)(now - SU.landed_at) * 10.f);
            dl->AddRect(p0 - ImVec2(sp(0.5f), sp(0.5f)), p1 + ImVec2(sp(0.5f), sp(0.5f)), ui::scale_alpha(ui::ACCENT, 0.6f + 0.4f * pulse), sp(1.75f), 0, 2.5f);
        }
    }
    // the "pay line": two soft edge fades and a marker under the centre
    ui::hfade_round_rect(dl, v0, ImVec2(v0.x + avail * 0.22f, v1.y), ui::BG, 0.f, 0, 1.f, 0.f);
    ui::hfade_round_rect(dl, ImVec2(v1.x - avail * 0.22f, v0.y), v1, ui::BG, 0.f, 0, 0.f, 1.f);
    dl->PopClipRect();
    dl->AddTriangleFilled(ImVec2(cx - sp(1.f), v1.y + sp(0.25f)), ImVec2(cx + sp(1.f), v1.y + sp(0.25f)), ImVec2(cx, v1.y - sp(0.75f)), ui::ACCENT);
    dl->AddRect(v0, v1, ui::HAIRLINE, sp(1.5f), 0, 1.f);
    ImGui::Dummy(ImVec2(avail, vh + sp(0.5f)));

    if (winner) {
        ImGui::PushFont(ui::F.h2);
        const std::string shown = SU.ph == Spin::Landed ? ui::ellipsize(ui::F.h2, ui::F.h2->FontSize, winner->title, avail) : std::string("?");
        const ImVec2 ts = ImGui::CalcTextSize(shown.c_str());
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (avail - std::min(ts.x, avail)) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Text, SU.ph == Spin::Landed ? ui::TEXT : ui::TEXT3);
        ImGui::TextUnformatted(shown.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
    ImGui::TextUnformatted(SU.ph == Spin::Spinning ? "A skip    B cancel" : "Opening its details...    B cancel");
    ImGui::PopStyleColor();
    ImGui::EndPopup();
}

/* A five-pip die face drawn inside the button. */
void draw_die(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    const float r = s * 0.5f;
    dl->AddRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), col, r * 0.3f, 0, std::max(1.5f, s * 0.1f));
    const float o = r * 0.5f, pr = std::max(1.2f, s * 0.07f);
    const ImVec2 pips[] = {{-o, -o}, {o, -o}, {0, 0}, {-o, o}, {o, o}};   // the five face
    for (auto& p : pips) dl->AddCircleFilled(ImVec2(c.x + p.x, c.y + p.y), pr, col, 10);
}

}  // namespace

void surprise_ui_button(float size) {
    if (g_headless && SU.ph == Spin::Idle && std::getenv("HUB_SHOT_SURPRISE")) {
        static bool done = false;
        if (!done && SH.el > 1.5 && !A.lib.empty() && !A.scanning) { done = true; start_spin(); }
    }
    const bool clicked = ui::icon_button("surprise", ui::Icon::None, size, SU.ph != Spin::Idle, nullptr);
    const bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    if (ImGui::IsItemHovered() || (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)) ImGui::SetTooltip("Surprise me");
    const ImVec2 c = (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) * 0.5f;
    draw_die(ImGui::GetWindowDrawList(), c, size * 0.42f, SU.ph != Spin::Idle ? IM_COL32_WHITE : (hot ? IM_COL32_WHITE : ui::TEXT2));
    if (clicked && SU.ph == Spin::Idle) start_spin();
    if (SU.want_open) { SU.want_open = false; ImGui::OpenPopup("##surprise"); }
    draw_popup();
}
