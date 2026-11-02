#include "app.h"
#include "cheevos_data.h"
#include "cheevos_login.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <thread>

/* ---------- RetroAchievements in the hub ----------
 * The runner (retrorun, src/retrorun/cheevos.cpp) does the real work and writes a local cache; the hub only reads it:
 *   - Settings > Account: sign in once with your own RA username + password. The password is sent to RA's server once
 *     (src/cheevos/cheevos_login.cpp) and is never stored or logged; only the returned token is kept in userdata.
 *   - details modal: "Achievements" section with progress (achievements_ui_section)
 *   - Home: "Recent unlocks" strip (achievements_home_strip)
 * Offline play only for now: the launch args are never added to online sessions (launch.cpp spawn). Headless runs never touch
 * the network; HUB_CHEEVOS_CFG / HUB_CHEEVOS_DIR point the hub at a mocked config and cache for screenshots. */

namespace {

std::string cfg_path() {
    if (const char* e = std::getenv("HUB_CHEEVOS_CFG")) return e;
    return hub::data_dir() + "retroachievements.json";
}
std::string cache_dir() {
    if (const char* e = std::getenv("HUB_CHEEVOS_DIR")) return e;
    return hub::data_dir() + "cheevos";
}

bool supported(hub::System s) {
    switch (s) {
        case hub::System::NES: case hub::System::SNES: case hub::System::GB: case hub::System::GBC:
        case hub::System::GBA: case hub::System::NDS: case hub::System::PS1: return true;
        default: return false;
    }
}

struct LoginJob {
    std::atomic<bool> done{false};
    bool ok = false;
    std::string err;
    cheevos::Config cfg;
};

struct State {
    cheevos::Config cfg;
    bool loaded = false;
    char user[64] = {0}, pass[128] = {0};
    std::string err;
    std::shared_ptr<LoginJob> job;
    double reload_at = 0;
    std::vector<cheevos::Recent> recent;
    std::map<std::string, std::pair<double, cheevos::GameCache>> caches;
    std::map<std::string, bool> has_cache;
};
State RA;

void load_cfg() {
    if (RA.loaded) return;
    RA.loaded = true;
    cheevos::load_config(cfg_path(), &RA.cfg);
}
void save_cfg() { cheevos::save_config(cfg_path(), RA.cfg); }

bool net_allowed() { return !g_headless || shot_net(); }

void start_login() {
    if (RA.job || !net_allowed() || !RA.user[0] || !RA.pass[0]) return;
    auto job = std::make_shared<LoginJob>();
    RA.job = job;
    RA.err.clear();
    std::string user = RA.user, pass = RA.pass;
    std::memset(RA.pass, 0, sizeof RA.pass);   // the password lives only in this worker's copy, then is gone
    job->cfg = RA.cfg;
    std::thread([job, user, pass]() mutable {
        job->ok = cheevos::login_with_password(user, pass, cheevos::real_http(), &job->cfg, &job->err);
        std::fill(pass.begin(), pass.end(), '\0');
        job->done = true;
    }).detach();
}

void poll_login() {
    if (!RA.job || !RA.job->done) return;
    if (RA.job->ok) {
        RA.cfg.username = RA.job->cfg.username;
        RA.cfg.token = RA.job->cfg.token;
        RA.cfg.enabled = true;
        save_cfg();
        RA.err.clear();
        RA.user[0] = 0;
        set_toast("Signed in to RetroAchievements as " + RA.cfg.username, true, 5.0);
    } else {
        RA.err = RA.job->err;
    }
    RA.job.reset();
}

const cheevos::GameCache* cache_for(const std::string& key) {
    double now = ImGui::GetTime();
    auto it = RA.caches.find(key);
    if (it == RA.caches.end() || now - it->second.first > 4.0) {
        cheevos::GameCache g;
        bool ok = cheevos::load_cache(cache_dir(), key, &g);
        auto& slot = RA.caches[key];
        slot.first = now;
        if (ok) slot.second = std::move(g);
        RA.has_cache[key] = ok;
        it = RA.caches.find(key);
    }
    return RA.has_cache[key] ? &it->second.second : nullptr;
}

void text_col(ImU32 c, const std::string& s) {
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::TextUnformatted(s.c_str());
    ImGui::PopStyleColor();
}

}  // namespace

/* " --ra-cfg X --ra-cache Y" for offline retrorun launches when the player enabled RetroAchievements and is signed in. */
std::string achievements_launch_args() {
    load_cfg();
    if (!RA.cfg.enabled || !RA.cfg.signed_in()) return "";
    return " --ra-cfg " + quote(cfg_path()) + " --ra-cache " + quote(cache_dir());
}

/* Details modal. Draws nothing unless the player is signed in and the system is supported. */
void achievements_ui_section(const hub::GameEntry& e) {
    load_cfg();
    if (!RA.cfg.enabled || !RA.cfg.signed_in() || !supported(e.system)) return;
    const cheevos::GameCache* g = cache_for(e.key);
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    ImGui::PushFont(ui::F.bold);
    ImGui::TextUnformatted("Achievements");
    ImGui::PopFont();
    if (!g) { text_wrapped_disabled("Play this game once to load its achievements."); return; }
    if (!g->total) { text_wrapped_disabled("RetroAchievements has no achievements for this game."); return; }
    ImGui::SameLine(0, sp(1.5f));
    text_col(ui::TEXT2, cheevos::progress_text(*g) + "  -  " + std::to_string(g->points_unlocked) + " of " + std::to_string(g->points_total) + " points");
    {   // progress bar
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x, h = sp(0.75f);
        float f = std::min(1.f, (float)g->unlocked / (float)g->total);
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ui::PANEL3, h * 0.5f);
        if (f > 0) dl->AddRectFilled(p, ImVec2(p.x + w * f, p.y + h), f >= 1.f ? ui::GOOD : ui::ACCENT, h * 0.5f);
        ImGui::Dummy(ImVec2(w, h + sp(0.5f)));
    }
    std::vector<const cheevos::Ach*> order;
    for (const auto& a : g->achs) order.push_back(&a);
    std::stable_sort(order.begin(), order.end(), [](const cheevos::Ach* x, const cheevos::Ach* y) {
        if ((x->unlock_time > 0) != (y->unlock_time > 0)) return x->unlock_time > 0;
        return x->unlock_time > y->unlock_time;
    });
    const size_t kMax = 6;
    ImGui::PushTextWrapPos(0.f);
    for (size_t i = 0; i < order.size() && i < kMax; i++) {
        const cheevos::Ach& a = *order[i];
        bool got = a.unlock_time > 0;
        text_col(got ? ui::GOOD : ui::TEXT3, got ? "+" : "-");
        ImGui::SameLine(0, sp(1.f));
        std::string line = a.title + "  (" + std::to_string(a.points) + ")";
        if (got && a.hardcore) line += "  hardcore";
        text_col(got ? ui::TEXT : ui::TEXT2, line);
        if (!a.desc.empty()) {
            ImGui::Indent(sp(2.f));
            text_col(ui::TEXT3, a.desc);
            ImGui::Unindent(sp(2.f));
        }
    }
    ImGui::PopTextWrapPos();
    if (order.size() > kMax) text_wrapped_disabled("+ " + std::to_string(order.size() - kMax) + " more");
}

/* Home: newest unlocks as small cards. Returns false when there is nothing to show. */
bool achievements_home_strip(float avail, float bleed) {
    load_cfg();
    if (!RA.cfg.enabled || !RA.cfg.signed_in()) return false;
    double now = ImGui::GetTime();
    if (now >= RA.reload_at) { RA.recent = cheevos::load_recent(cache_dir()); RA.reload_at = now + 4.0; }
    if (RA.recent.empty()) return false;
    shelf_header("Recent unlocks", "", bleed, (int)RA.recent.size());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cw = sp(36.f), ch = sp(10.f), gap = sp(2.f);
    int fit = std::max(1, (int)((avail - bleed) / (cw + gap)));
    ImVec2 o = ImGui::GetCursorScreenPos();
    o.x += bleed;
    for (int i = 0; i < fit && i < (int)RA.recent.size(); i++) {
        const cheevos::Recent& r = RA.recent[i];
        ImVec2 a(o.x + i * (cw + gap), o.y), b(a.x + cw, a.y + ch);
        dl->AddRectFilled(a, b, ui::PANEL, sp(1.25f));
        dl->AddRect(a, b, ui::HAIRLINE, sp(1.25f));
        dl->AddRectFilled(a, ImVec2(a.x + sp(0.5f), b.y), ui::WARN, sp(0.25f));
        float tx = a.x + sp(1.5f), ty = a.y + sp(1.f);
        ImFont* f = ui::F.bold;
        dl->PushClipRect(a, b, true);
        dl->AddText(f, f->FontSize, ImVec2(tx, ty), ui::TEXT, r.title.c_str());
        dl->AddText(ImVec2(tx, ty + f->FontSize + sp(0.5f)), ui::TEXT2, r.game_title.c_str());
        std::string meta = std::to_string(r.points) + " pts" + (r.hardcore ? "  hardcore" : "") + "  -  " + fmt_time((uint64_t)r.time);
        dl->AddText(ImVec2(tx, ty + f->FontSize + sp(0.5f) + ImGui::GetFontSize() + sp(0.5f)), ui::TEXT3, meta.c_str());
        dl->PopClipRect();
    }
    ImGui::Dummy(ImVec2(0, ch + sp(1.5f)));
    return true;
}

/* Settings > Account section. */
void achievements_account_section() {
    load_cfg();
    poll_login();
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    settings_label("RetroAchievements", "Earn achievements in NES, SNES, Game Boy, GBA, DS and PlayStation games (offline play). Sign in with your own "
                                         "RetroAchievements account: the password is sent to retroachievements.org once and is never saved, only a login token is kept on this PC.");
    if (RA.cfg.signed_in()) {
        ImGui::Text("Signed in as %s", RA.cfg.username.c_str());
        if (ImGui::Checkbox("Show my achievements and earn them in games", &RA.cfg.enabled)) save_cfg();
        if (ImGui::Checkbox("Hardcore mode", &RA.cfg.hardcore)) save_cfg();
        text_wrapped_disabled("Hardcore blocks loading save states, rewind, cheats, fast-forward and slow motion (you see a message when you try). "
                              "Achievements earned in hardcore count extra on RetroAchievements.");
        if (button("Sign out", ImVec2(0, 0))) { RA.cfg = cheevos::Config(); save_cfg(); RA.recent.clear(); RA.caches.clear(); }
    } else {
        bool busy = RA.job != nullptr;
        ImGui::SetNextItemWidth(sp(26.f));
        osk::input("##ra_user", "RetroAchievements username", RA.user, sizeof RA.user);
        ImGui::SetNextItemWidth(sp(26.f));
        bool enter = osk::input("##ra_pass", "Password", RA.pass, sizeof RA.pass, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
        if (button(busy ? "Signing in..." : "Sign in", ImVec2(0, 0), true, !busy && net_allowed() && RA.user[0] && RA.pass[0]) || (enter && !busy)) start_login();
        if (!RA.err.empty()) {
            ImGui::PushTextWrapPos(0.f);
            text_col(COL_BAD, RA.err);
            ImGui::PopTextWrapPos();
        }
    }
}
