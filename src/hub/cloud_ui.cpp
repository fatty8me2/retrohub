#include "app.h"

/* ---------- Cloud saves v2 UI (docs/CLOUD_SAVES_V2.md section 2, Lane U) ----------
 * Everything here talks to the engine only through cloud_engine.h. Pure rules (labels, tones, dialog text) live in
 * cloud_ui_logic.h and are unit tested. Screenshot aids (no network):
 *   HUB_SHOT_CLOUD2=ok|mixed|offline|over|attention|paused|fresh   fake engine data (cloud_engine_stub.cpp)
 *   HUB_SHOT_CLOUD_OPEN=<system/key>        expand that game's history panel
 *   HUB_SHOT_CLOUD_DIALOG=conflict|lease|checking|pulled|offline   launch-time dialogs / notices
 *   HUB_SHOT_CLOUD_CODE=1                   reveal the recovery code (Account section) */

namespace {

using namespace hubui::cloudui;
namespace hs = hub::saves;

ImU32 tone_col(Tone t) {
    switch (t) {
        case Tone::Good: return ui::GOOD;
        case Tone::Busy: return ui::ACCENT;
        case Tone::Warn: return ui::WARN;
        case Tone::Bad: return ui::BAD;
        default: return ui::TEXT2;
    }
}
ImU32 chip_bg(Tone t) { return t == Tone::Muted ? (ImU32)ui::PANEL3 : ui::mix(tone_col(t), IM_COL32_BLACK, 0.5f); }

void draw_chip(const Chip& c) { ui::pill(c.text.c_str(), chip_bg(c.tone), c.tone == Tone::Muted ? (ImU32)ui::TEXT : IM_COL32_WHITE); }


/* Cached engine state; the engine calls are cheap but the list is copied, so refresh a few times a second. */
struct Cache {
    double t = -10;
    hs::CloudAccountInfo acct;
    std::vector<hs::GameCloudInfo> games;
    std::map<std::string, std::string> titles;   // gkey -> title
    bool applied_pause = false;
    int conflicts_told = 0;
    size_t vault_n = (size_t)-1;
} C;

void refresh_cache(bool force = false) {
    double now = ImGui::GetTime();
    static int seen_gen = 0;
    if (seen_gen != cloud_bg_generation()) { seen_gen = cloud_bg_generation(); force = true; }   // a background cloud job finished
    if (!force && now - C.t < 0.4) return;
    C.t = now;
    C.acct = hs::cloud_account();
    C.games = hs::cloud_games();
    if (C.vault_n != A.vault_rows.size()) {
        C.vault_n = A.vault_rows.size();
        C.titles.clear();
        for (auto& r : A.vault_rows) C.titles[r.m.system + "/" + r.m.key] = r.m.title.empty() ? r.m.key : r.m.title;
    }
    std::sort(C.games.begin(), C.games.end(), game_before);
}
std::string system_label(const hs::GameId& id) { return hub::system_name(hub::system_from_short(id.system)); }
const std::vector<CatalogName>& catalog_names() {
    static std::vector<CatalogName> v;
    if (v.empty()) for (auto& g : hub::catalog()) v.push_back({g.id, g.title, g.system});
    return v;
}
/* Library title, else the vault's, else the catalog / a "<System> game" fallback. Never a raw key like "zelda-ocarina". */
std::string title_of(const hs::GameId& id) {
    std::string known;
    for (auto& e : A.lib) if (e.key == id.key) { known = e.title; break; }
    if (known.empty()) { auto it = C.titles.find(id.gkey()); if (it != C.titles.end()) known = it->second; }
    return friendly_title(known, id.system, id.key, catalog_names(), system_label(id));
}
/* Engine toasts are written with the raw game key; swap in the title. */
std::string retitle(std::string text, const hs::GameId& id) {
    if (id.key.empty()) return text;
    const std::string t = title_of(id);
    for (size_t p = 0; (p = text.find(id.key, p)) != std::string::npos; p += t.size()) text.replace(p, id.key.size(), t);
    return text;
}
int64_t now_s() { return (int64_t)std::time(nullptr); }

/* ---- async helpers (never block the UI thread; detached so exit never waits on the network) ---- */
template <class T> struct Task {
    std::atomic<bool> done{false};
    T val{};
};
using Versions = std::vector<std::pair<std::string, std::vector<hs::VersionInfo>>>;

struct History {
    std::string gkey;
    std::shared_ptr<Task<Versions>> load;
    std::shared_ptr<Task<std::pair<bool, std::string>>> restore;
    std::string result;
    bool result_ok = true;
    bool confirm_remove = false;
} H;
std::string g_open;   // expanded game (gkey)

/* One background cloud action at a time (pin / remove / resolve): each does HTTP, so it never runs on the frame thread. */
bool job_busy() { return cloud_bg_busy(""); }
void run_job(const hs::GameId& id, std::function<void()> fn) {
    if (job_busy()) return;   // one at a time; the buttons are disabled meanwhile
    cloud_bg_run(id.gkey(), std::move(fn));
}

void open_history(const hs::GameId& id) {
    H = History();
    H.gkey = id.gkey();
    H.load = std::make_shared<Task<Versions>>();
    auto t = H.load;
    if (g_headless && !std::getenv("HUB_SHOT_CLOUD2") && !std::getenv("HUB_SHOT_CLOUD_REAL")) { t->done = true; return; }
    std::thread([t, id]() { t->val = hs::cloud_versions(id); t->done = true; }).detach();
}

/* ---- launch-time dialogs ---- */
struct ConflictDlg {
    bool want_open = false, open = false;
    hs::GameId id;
    std::string title;
    std::vector<hs::ConflictItem> items;
    std::function<void(hs::ConflictChoice)> done;
} CD;
struct LeaseDlg {
    bool want_open = false;
    hs::GameId id;
    std::string title, by;
    int64_t since = 0;
    std::function<void(hs::LeaseChoice)> done;
} LD;
struct Checking { bool on = false; std::string title; double t0 = 0; } CK;

/* ---- Saves page pieces ---- */
void quota_bar(float w) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = sp(1.25f), r = h * 0.5f;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ui::PANEL3, r);
    const float f = quota_fraction(C.acct);
    if (f > 0.f) dl->AddRectFilled(p, ImVec2(p.x + std::max(h, w * f), p.y + h), tone_col(quota_tone(C.acct)), r);
    ImGui::Dummy(ImVec2(w, h));
}

void account_panel() {
    const bool on = A.st.cloud_enabled;
    begin_panel("cloud_acct");
    section_title(A.st.username_claimed && !A.st.name.empty() ? "Cloud saves for " + A.st.name : std::string("Cloud saves"));   // the unique username, once claimed
    Chip head = account_headline(C.acct, on);
    ImGui::PushFont(ui::F.h2);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(tone_col(head.tone == Tone::Muted ? Tone::Muted : head.tone)));
    ImGui::TextUnformatted(head.text.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    if (on && C.acct.known && C.acct.cap > 0) {
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        const float w = std::min(sp(60.f), ImGui::GetContentRegionAvail().x);
        quota_bar(w);
        ImGui::PushFont(ui::F.bold);
        ImGui::TextUnformatted(quota_text(C.acct).c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextDisabled("used");
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        text_wrapped_disabled("Your regular saves always back up. Save states and big memory cards use the rest of the space.");
    }
    if (on && C.acct.needs_attention) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(COL_BAD));
        ImGui::TextWrapped("%s", attention_help(C.acct.attention).c_str());
        ImGui::PopStyleColor();
    }
    {   /* over the limit: which games are local only */
        std::vector<std::string> names;
        for (auto& g : C.games) if ((g.state == hs::CloudState::LocalOnly || g.state == hs::CloudState::Synced) && g.local_only == hs::LocalOnlyReason::OverCap) names.push_back(title_of(g.id));
        std::string t = on ? over_limit_text(C.acct, names) : std::string();
        if (!t.empty()) {
            ImGui::Dummy(ImVec2(0, sp(0.5f)));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(COL_WARN));
            ImGui::TextWrapped("%s", t.c_str());
            ImGui::PopStyleColor();
        }
    }
    if (on && C.acct.known && !C.acct.device_name.empty())
        text_wrapped_disabled("This PC: " + C.acct.device_name + (C.acct.devices > 1 ? "  -  " + std::to_string(C.acct.devices) + " PCs use this account" : std::string()));
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    const bool busy = A.job.running;
    const bool can_sync = (on || !A.st.sync_folder.empty()) && !busy && !A.proc;
    if (button(busy && A.job.kind == SaveJob::Sync ? "Syncing..." : "Sync now", ImVec2(sp(18.f), sp(5.5f)), true, can_sync, ui::Icon::Refresh)) {
        if (on) hs::cloud_sync_all_now();
        if (!A.st.sync_folder.empty()) start_sync(true);
        if (on) set_toast("Syncing your saves...", true, 3.0);
    }
    if (A.proc && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("A game is running. Saves sync automatically when it closes.");
    if (!A.sync_result.empty()) {
        ImGui::SameLine();
        ImGui::BeginGroup();
        result_line(A.sync_result, A.sync_result_ok);
        ImGui::EndGroup();
    }
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    bool paused = !on;
    if (ImGui::Checkbox("Pause cloud sync", &paused)) {
        A.st.cloud_enabled = !paused;
        hs::cloud_set_paused(paused);
        mark_dirty();
        refresh_cache(true);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Advanced: saves stay on this PC only while paused");
    if (on && ImGui::Checkbox("Back up screenshots", &A.st.backup_shots)) { hs::set_backup_screenshots(A.st.backup_shots); mark_dirty(); }
    if (on) { ImGui::SameLine(); ImGui::TextDisabled("They share the space used by save states"); }
    {
        bool st = A.st.cloud_states;
        if (ImGui::Checkbox("Back up save states to the cloud", &st)) {
            A.st.cloud_states = st;
            hs::cloud_set_sync_states(st);
            mark_dirty();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Resume points follow you between PCs. Uses the shared 50 MB space; turn off to keep states on this PC.");
    }
    end_panel();
}

void history_block(const hs::GameCloudInfo& g) {
    ImGui::Indent(sp(1.5f));
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    ImGui::PushFont(ui::F.bold);
    ImGui::TextUnformatted("Version history");
    ImGui::PopFont();
    if (!H.load || !H.load->done) {
        ImGui::TextDisabled("Loading...");
    } else if (H.load->val.empty()) {
        ImGui::TextDisabled("No earlier versions yet. Older copies appear here after the save changes.");
    } else {
        for (auto& f : H.load->val) {
            ImGui::PushID(f.first.c_str());
            ImGui::TextDisabled("%s", f.first.c_str());
            int shown = 0;
            for (size_t i = 0; i < f.second.size() && shown < 3; ++i, ++shown) {
                const auto& v = f.second[i];
                ImGui::PushID((int)i);
                std::string line = (i == 0 ? "Current: " : "Earlier: ") + when_text(v.ts ? v.ts : v.mtime) + (v.device_name.empty() ? "" : " on " + v.device_name) + ", " + kb_text(v.size);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(line.c_str());
                if (i > 0) {
                    ImGui::SameLine();
                    const bool rb = H.restore && !H.restore->done;
                    if (button("Restore previous", ImVec2(0, 0), false, !rb && !g_headless && !A.proc)) {
                        H.restore = std::make_shared<Task<std::pair<bool, std::string>>>();
                        auto t = H.restore;
                        hs::GameId id = g.id;
                        std::string path = f.first;
                        int ver = v.ver;
                        std::thread([t, id, path, ver]() { std::string e; bool ok = hs::cloud_restore_version(id, path, ver, &e); t->val = {ok, e}; t->done = true; }).detach();
                        H.result.clear();
                    }
                }
                ImGui::PopID();
            }
            ImGui::PopID();
        }
        if (A.proc) text_wrapped_disabled("Close the running game before restoring a save.");
    }
    if (H.restore && H.restore->done) {
        H.result_ok = H.restore->val.first;
        H.result = H.result_ok ? "Restored. The old version is back and syncing." : "Could not restore: " + (H.restore->val.second.empty() ? std::string("try again later") : H.restore->val.second);
        H.restore.reset();
    }
    result_line(H.result, H.result_ok);
    if (job_busy()) { ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Working on it..."); }
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    const bool pinned = g.pinned;
    if (button(pinned ? "Stop keeping in cloud" : "Keep in cloud", ImVec2(0, 0), false, !job_busy())) { hs::GameId id = g.id; run_job(id, [id, pinned]() { hs::cloud_set_pin(id, !pinned); }); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Kept games are the last to be moved off the cloud when space runs out");
    ImGui::SameLine();
    const bool lo = g.state == hs::CloudState::LocalOnly && g.local_only == hs::LocalOnlyReason::User;
    if (button(lo ? "Back up to cloud again" : "Keep on this PC only", ImVec2(0, 0), false, true)) { hs::cloud_set_local_only(g.id, !lo); refresh_cache(true); }
    ImGui::SameLine();
    if (!H.confirm_remove) {
        if (button("Remove from cloud", ImVec2(0, 0), false, true)) H.confirm_remove = true;
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Delete the cloud copy? Saves on this PC stay.");
        ImGui::SameLine();
        if (button("Remove", ImVec2(0, 0), true, !job_busy())) { hs::GameId id = g.id; run_job(id, [id]() { hs::cloud_remove_game(id); }); H.confirm_remove = false; g_open.clear(); }
        ImGui::SameLine();
        if (button("Keep it", ImVec2(0, 0), false, true)) H.confirm_remove = false;
    }
    ImGui::Unindent(sp(1.5f));
}

void games_panel() {
    section_title("Games in the cloud (" + std::to_string(C.games.size()) + ")");
    begin_panel("cloud_games");
    if (A.st.cloud_enabled && C.games.empty()) {
        /* while the account is still being read the "Cloud saves" box above already says "Checking your cloud saves...": say it once */
        if (C.acct.known)
            ImGui::TextDisabled(!C.acct.online ? "Offline: your cloud games show up here once you are back online."
                                               : "Nothing yet. Play a game and its saves are backed up here automatically.");
        else
            ImGui::TextDisabled("Your games show up here in a moment.");
    } else if (!A.st.cloud_enabled) {
        ImGui::TextDisabled("%s. Turn it back on above to see your games here.", paused_text(C.acct.queued_games).c_str());
    }
    const int64_t now = now_s();
    if (!A.st.cloud_enabled) { end_panel(); return; }
    static bool only_attn = false;
    const int attn = attention_count(C.games);
    if (!C.games.empty()) {
        if (button(only_attn ? "Show all games" : ("Needs attention (" + std::to_string(attn) + ")").c_str(), ImVec2(0, 0), only_attn || attn > 0)) only_attn = !only_attn;
        if (attn > 0) { ImGui::SameLine(); pill((std::to_string(attn) + (attn == 1 ? " game needs you" : " games need you")).c_str(), ui::mix(ui::BAD, IM_COL32_BLACK, 0.5f)); }
        if (only_attn && attn == 0) ImGui::TextDisabled("Nothing needs attention. All your saves are in step.");
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
    }   // paused: do not list rows that would all say "Waiting to sync"
    for (size_t i = 0; i < C.games.size(); ++i) {
        const hs::GameCloudInfo& g = C.games[i];
        if (only_attn && !needs_attention(g)) continue;
        ImGui::PushID(g.id.gkey().c_str());
        if (i) { ImGui::Separator(); }
        ImGui::Dummy(ImVec2(0, sp(0.25f)));
        ImGui::AlignTextToFramePadding();
        ImGui::PushFont(ui::F.bold);
        ImGui::TextUnformatted(title_of(g.id).c_str());
        ImGui::PopFont();
        ImGui::SameLine(0, sp(1.f));
        ImGui::TextDisabled("%s", system_label(g.id).c_str());
        ImGui::SameLine(0, sp(1.f));
        draw_chip(game_chip(g, C.acct));
        ImGui::PushTextWrapPos(0.f);
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
        ImGui::TextWrapped("%s", game_detail(g, C.acct, now).c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        if (g.state == hs::CloudState::Conflict && !g.conflicts.empty()) {
            ImGui::TextDisabled("%s", side_line(g.conflicts[0].local, false).c_str());
            ImGui::TextDisabled("%s", side_line(g.conflicts[0].cloud, true).c_str());
        }
        ImGui::Dummy(ImVec2(0, sp(0.25f)));
        if (g.state == hs::CloudState::Conflict) {
            if (button("Resolve...", ImVec2(0, 0), true, !A.proc && !job_busy())) {
                hs::GameId id = g.id;
                cloud_ui_ask_conflict(id, title_of(id), g.conflicts, [id](hs::ConflictChoice c) { run_job(id, [id, c]() { hs::cloud_resolve_conflict(id, c); }); });
            }
            ImGui::SameLine();
        }
        const bool open = g_open == g.id.gkey();
        if (button(open ? "Hide history" : "History and options", ImVec2(0, 0), false, true)) {
            if (open) g_open.clear();
            else { g_open = g.id.gkey(); open_history(g.id); }
        }
        ImGui::SameLine();
        if (button("Save history", ImVec2(0, 0), false, true)) history_ui_open(g.id.system, g.id.key);
        if (open) history_block(g);
        ImGui::PopID();
    }
    end_panel();
}

}  // namespace

/* ======================= public ======================= */

void cloud_ui_tick() {
    refresh_cache();
    if (!C.applied_pause) {
        C.applied_pause = true;
        if (!A.st.cloud_enabled) hs::cloud_set_paused(true);
        hs::cloud_set_sync_states(A.st.cloud_states);
    }
    hs::CloudEvent ev;
    int guard = 8;
    while (guard-- > 0 && hs::cloud_pop_event(&ev)) {
        ToastSpec t = event_toast(ev);
        if (t.show) push_toast(retitle(t.text, ev.game), t.good ? COL_GOOD : COL_WARN, t.secs);
        if (ev.kind == hs::CloudEvent::ConflictFound) C.conflicts_told = hubui::cloudui::conflict_count(C.games);   // that toast already said it
    }
    if (C.acct.known && A.st.cloud_enabled) {   // a conflict found in the background (not at a launch): say so once per new one
        const int n = hubui::cloudui::conflict_count(C.games);
        if (n > C.conflicts_told && A.page != Page::Saves) push_toast(conflict_banner(n) + " Open Saves.", COL_WARN, 9.0);
        C.conflicts_told = n;
    }
}

int cloud_ui_conflict_count() { return A.st.cloud_enabled ? hubui::cloudui::conflict_count(C.games) : 0; }

void cloud_ui_saves_page() {
    cloud_ui_tick();
    if (const int nc = cloud_ui_conflict_count()) {   // plain both-sides-edited conflicts, not only "decide later"
        begin_panel("cloud_conflicts");
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(COL_BAD));
        ImGui::PushFont(ui::F.bold);
        ImGui::TextWrapped("%s", conflict_banner(nc).c_str());
        ImGui::PopFont();
        ImGui::PopStyleColor();
        text_wrapped_disabled("Nothing was overwritten. Use Resolve below; you are also asked when you start the game.");
        end_panel();
        ImGui::Dummy(ImVec2(0, 2 * S));
    }
    account_panel();
    ImGui::Dummy(ImVec2(0, 4 * S));
    games_panel();
    ImGui::Dummy(ImVec2(0, 4 * S));
}

void cloud_ui_checking(const std::string& title, bool on) {
    CK.on = on;
    CK.title = title;
    CK.t0 = ImGui::GetTime();
}

void cloud_ui_prelaunch_notice(const hs::PrelaunchResult& r) {
    std::string s = prelaunch_notice(r);
    if (!s.empty()) push_toast(s, r.offline ? COL_WARN : COL_GOOD, 6.0);
}

void cloud_ui_ask_conflict(const hs::GameId& g, const std::string& title, const std::vector<hs::ConflictItem>& items,
                           std::function<void(hs::ConflictChoice)> done) {
    CD.id = g;
    CD.title = title;
    CD.items = items;
    CD.done = std::move(done);
    CD.want_open = true;
}

void cloud_ui_ask_lease(const hs::GameId& g, const std::string& title, const std::string& held_by, int64_t since,
                        std::function<void(hs::LeaseChoice)> done) {
    LD.id = g;
    LD.title = title;
    LD.by = held_by;
    LD.since = since;
    LD.done = std::move(done);
    LD.want_open = true;
}

void cloud_ui_overlays() {
    cloud_ui_tick();
    ImGuiIO& io = ImGui::GetIO();
    /* "Getting your save from the cloud..." strip */
    if (CK.on) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        ImFont* f = ui::F.bold;
        std::string t = checking_text();
        ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.f, t.c_str());
        float w = ts.x + sp(7.f), h = ts.y + sp(2.5f);
        ImVec2 p0((io.DisplaySize.x - w) * 0.5f, io.DisplaySize.y * 0.82f - h), p1(p0.x + w, p0.y + h);
        ui::shadow(dl, p0, p1, sp(1.5f), 1.f);
        dl->AddRectFilled(p0, p1, ui::PANEL2, sp(1.5f));
        dl->AddRect(p0, p1, ui::HAIRLINE, sp(1.5f), 0, 1.f);
        const float a = (float)ImGui::GetTime() * 5.f;
        ImVec2 c(p0.x + sp(2.2f), (p0.y + p1.y) * 0.5f);
        for (int i = 0; i < 3; ++i) dl->AddCircleFilled(ImVec2(c.x + i * sp(0.9f) - sp(0.9f), c.y + std::sin(a + i * 0.9f) * sp(0.35f)), sp(0.28f), ui::ACCENT, 10);
        dl->AddText(f, f->FontSize, ImVec2(p0.x + sp(4.2f), p0.y + sp(1.25f)), ui::TEXT, t.c_str());
        if (!CK.title.empty()) {
            ImFont* s = ui::F.cap;
            ImVec2 us = s->CalcTextSizeA(s->FontSize, FLT_MAX, 0.f, CK.title.c_str());
            dl->AddText(s, s->FontSize, ImVec2((p0.x + p1.x - us.x) * 0.5f, p1.y + sp(0.5f)), ui::TEXT2, CK.title.c_str());
        }
        if (A.cloud_gate.waiting) {   // waiting for the previous session's cloud work: offer a way out
            ImFont* s = ui::F.cap;
            const char* ct = "Cancel (Esc)";
            ImVec2 cs = s->CalcTextSizeA(s->FontSize, FLT_MAX, 0.f, ct);
            ImVec2 q0((p0.x + p1.x - cs.x) * 0.5f, p1.y + sp(2.6f)), q1(q0.x + cs.x, q0.y + cs.y);
            const bool hov = ImGui::IsMouseHoveringRect(ImVec2(q0.x - sp(1.f), q0.y - sp(0.5f)), ImVec2(q1.x + sp(1.f), q1.y + sp(0.5f)), false);
            dl->AddText(s, s->FontSize, q0, hov ? ui::TEXT : ui::ACCENT, ct);
            if ((hov && ImGui::IsMouseClicked(0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) cloud_gate_cancel();
        }
    }
    /* conflict dialog */
    if (CD.want_open) { CD.want_open = false; ImGui::OpenPopup("Saves differ##cloud_conflict"); }
    ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(3.f), sp(3.f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.5f));
    if (ImGui::BeginPopupModal("Saves differ##cloud_conflict", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        auto finish = [&](hs::ConflictChoice c) {
            auto cb = CD.done;
            CD.done = nullptr;
            ImGui::CloseCurrentPopup();
            if (cb) cb(c);
        };
        if (ui::modal_back()) finish(hs::ConflictChoice::DecideLater);
        ImGui::PushTextWrapPos(std::min(sp(58.f), io.DisplaySize.x * 0.85f));
        ImGui::PushFont(ui::F.title);
        ImGui::TextUnformatted(conflict_summary(CD.title).c_str());
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        text_wrapped_disabled("Both this PC and the cloud have a newer save than the last time they matched.");
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        for (size_t i = 0; i < CD.items.size() && i < 3; ++i) {
            const auto& it = CD.items[i];
            ImGui::PushFont(ui::F.bold);
            ImGui::TextUnformatted(it.path.c_str());
            ImGui::PopFont();
            const bool ln = local_is_newer(it);
            ImGui::TextColored(ImColor(ln ? ui::GOOD : ui::TEXT2), "%s%s", side_line(it.local, false).c_str(), ln ? "   (newer)" : "");
            ImGui::TextColored(ImColor(!ln ? ui::GOOD : ui::TEXT2), "%s%s", side_line(it.cloud, true).c_str(), !ln ? "   (newer)" : "");
        }
        if (CD.items.size() > 3) ImGui::TextDisabled("and %zu more files", CD.items.size() - 3);
        ImGui::Dummy(ImVec2(0, sp(1.5f)));
        static hs::ConflictChoice hint = hs::ConflictChoice::UseNewer;
        const hs::ConflictChoice order[] = {hs::ConflictChoice::UseNewer, hs::ConflictChoice::UseLocal, hs::ConflictChoice::UseCloud, hs::ConflictChoice::DecideLater};
        for (int i = 0; i < 4; ++i) {
            if (i) ImGui::SameLine();
            ImGui::PushID(i);
            if (button(conflict_button(order[i]), ImVec2(0, sp(5.5f)), i == 0, true)) finish(order[i]);
            if (i == 0) ImGui::SetItemDefaultFocus();
            if (ImGui::IsItemFocused() || ImGui::IsItemHovered()) hint = order[i];
            ImGui::PopID();
        }
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        text_wrapped_disabled(conflict_hint(hint));
        text_wrapped_disabled("Whichever you do not pick is kept, so nothing is lost.");
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
    /* "open on another PC" dialog */
    if (LD.want_open) { LD.want_open = false; ImGui::OpenPopup("Open on another PC##cloud_lease"); }
    ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Open on another PC##cloud_lease", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        auto finish = [&](hs::LeaseChoice c) {
            auto cb = LD.done;
            LD.done = nullptr;
            ImGui::CloseCurrentPopup();
            if (cb) cb(c);
        };
        if (ui::modal_back()) finish(hs::LeaseChoice::Cancel);
        ImGui::PushTextWrapPos(std::min(sp(50.f), io.DisplaySize.x * 0.85f));
        ImGui::PushFont(ui::F.title);
        ImGui::TextUnformatted("Already playing elsewhere");
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        ImGui::TextWrapped("%s", lease_text(LD.title, LD.by, LD.since).c_str());
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        text_wrapped_disabled("Play anyway keeps both saves if they end up different. Take over makes the other PC step aside.");
        ImGui::Dummy(ImVec2(0, sp(1.5f)));
        if (button(lease_button(hs::LeaseChoice::Cancel), ImVec2(sp(12.f), sp(5.5f)), true, true)) finish(hs::LeaseChoice::Cancel);
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (button(lease_button(hs::LeaseChoice::PlayAnyway), ImVec2(sp(14.f), sp(5.5f)), false, true)) finish(hs::LeaseChoice::PlayAnyway);
        ImGui::SameLine();
        if (button(lease_button(hs::LeaseChoice::TakeOver), ImVec2(sp(14.f), sp(5.5f)), false, true)) finish(hs::LeaseChoice::TakeOver);
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

void cloud_ui_shot_init() {
    const char* d = std::getenv("HUB_SHOT_CLOUD_DIALOG");
    if (const char* o = std::getenv("HUB_SHOT_CLOUD_OPEN")) { g_open = o; hs::GameId id; auto s = g_open.find('/'); id.system = g_open.substr(0, s); id.key = g_open.substr(s + 1); open_history(id); }
    if (std::getenv("HUB_SHOT_CLOUD2")) { A.vault_dirty = false; }
    if (!d) return;
    std::string m = d;
    hs::GameId id{"n64", "mario-kart-64"};
    if (m == "conflict" || m == "lease") {
        // the dialogs need an ImGui frame to open; seed through the ask-APIs on first overlay draw
        static bool once = false;
        (void)once;
        hs::ConflictItem ci;
        ci.path = "battery/mk64.eep";
        ci.local = {true, "a", 412 * 1024, now_s() - 5 * 86400 - 3600, "PC-DESK"};
        ci.cloud = {true, "b", 410 * 1024, now_s() - 3 * 86400, "PC-LIVINGROOM"};
        if (m == "conflict") cloud_ui_ask_conflict(id, "Mario Kart 64", {ci}, nullptr);
        else cloud_ui_ask_lease(id, "Mario Kart 64", "PC-LIVINGROOM", now_s() - 1500, nullptr);
    } else if (m == "checking") {
        CK.on = true;
        CK.title = "Pokemon Emerald";
    } else if (m == "pulled" || m == "offline") {
        hs::PrelaunchResult r;
        r.files_pulled = 2;
        r.offline = m == "offline";
        push_toast(prelaunch_notice(r), r.offline ? COL_WARN : COL_GOOD, 60.0);
        if (m == "pulled") push_toast("Cloud space is full: older save states of Super Smash Bros. Melee, Wind Waker stay on this PC only.", COL_WARN, 60.0);
    }
}

std::string cloud_ui_pill(const hub::GameEntry& e, ImU32* col) {
    if (!A.st.cloud_enabled || is_native(e)) return "";
    hs::GameCloudInfo g;
    hs::GameId id{sys_short(e), e.key};
    for (auto& x : C.games) if (x.id.key == id.key && x.id.system == id.system) { g = x; goto found; }
    return "";
found:
    std::string t = tile_pill(g, C.acct);
    if (t.empty()) return t;
    Chip c = game_chip(g, C.acct);
    if (col) *col = g.state == hs::CloudState::Synced ? IM_COL32(150, 235, 170, 235) : tone_col(c.tone == Tone::Muted ? Tone::Warn : c.tone);
    if (c.tone == Tone::Muted && g.state != hs::CloudState::Synced && col) *col = IM_COL32(255, 255, 255, 190);
    return t;
}

void cloud_ui_game_line(const hub::GameEntry& e) {
    if (!A.st.cloud_enabled || is_native(e)) return;
    hs::GameCloudInfo g;
    hs::GameId id{sys_short(e), e.key};
    if (!hs::cloud_game(id, &g) || g.state == hs::CloudState::Unknown) return;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Cloud:");
    ImGui::SameLine(0, sp(0.75f));
    draw_chip(game_chip(g, C.acct));
    ImGui::SameLine(0, sp(1.f));
    ImGui::TextDisabled("%s", game_detail(g, C.acct, now_s()).c_str());
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
}

void cloud_ui_wizard_body(float w) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ui::icon(dl, ui::Icon::Check, ImVec2(p.x + sp(1.5f), p.y + sp(1.6f)), sp(3.f), ui::GOOD);
    ImGui::Dummy(ImVec2(sp(3.5f), 0));
    ImGui::SameLine();
    ImGui::PushFont(ui::F.h2);
    ImGui::TextColored(ImColor(ui::GOOD), "Cloud saves are on for %s", A.st.name.c_str());
    ImGui::PopFont();
    ImGui::TextWrapped("Your game saves are backed up automatically and follow your account. Play on another PC and they are already there. There is nothing to turn on.");
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    text_wrapped_disabled("To use your saves on another PC, open Settings > Account > Add this account to another PC. "
                          "You can find your cloud status any time on the Saves page.");
    ImGui::PopTextWrapPos();
}
