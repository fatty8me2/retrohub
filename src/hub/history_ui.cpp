#include "app.h"
#include "homefun.h"

/* ---------- Save history (Wave 2 G) and "Your stats" (Wave 2 H) ----------
 * History: one unified list per game on the Saves page (local snapshots + cloud versions), with Restore / Undo restore / Export.
 * Stats: the card at the top of Home. Logic lives in src/saves/history.* and playstats.* (unit tested). */

namespace {

namespace hs = hub::saves;
using namespace hubui::cloudui;

template <class T> struct Task {
    std::atomic<bool> done{false};
    T val{};
};
using Versions = std::vector<std::pair<std::string, std::vector<hs::VersionInfo>>>;

struct Panel {
    bool open = false;
    hs::GameId id;
    std::string title;
    std::string hist_dir, battery_dir;
    double listed_at = -10;
    std::vector<hs::HistEntry> local;
    hs::HistEntry undo;
    bool has_undo = false;
    std::shared_ptr<Task<Versions>> cloud;
    std::shared_ptr<Task<std::pair<bool, std::string>>> op;   // a cloud restore in flight
    std::string result;
    bool ok = true;
} P;

int64_t now_s() { return (int64_t)std::time(nullptr); }

void reload_local() {
    P.listed_at = ImGui::GetTime();
    P.local = hs::history_list(P.hist_dir);
    P.has_undo = hs::history_undo_info(P.hist_dir, &P.undo);
}

void reload_cloud() {
    P.cloud = std::make_shared<Task<Versions>>();
    auto t = P.cloud;
    if (!A.st.cloud_enabled || (g_headless && !std::getenv("HUB_SHOT_CLOUD2") && !std::getenv("HUB_SHOT_CLOUD_REAL"))) { t->done = true; return; }
    hs::GameId id = P.id;
    std::thread([t, id]() { t->val = hs::cloud_versions(id); t->done = true; }).detach();
}

std::string title_for(const std::string& key, const std::string& fallback) {
    for (auto& e : A.lib) if (e.key == key) return e.title;
    return fallback;
}

}  // namespace

void history_ui_open(const std::string& system, const std::string& key) {
    P = Panel();
    P.open = true;
    P.id = {system, key};
    P.title = title_for(key, key);
    P.hist_dir = hs::game_history_dir(system, key);
    P.battery_dir = hs::battery_dir(system, key);
    reload_cloud();
}

void history_ui_saves_page() {
    if (!P.open) return;
    if (ImGui::GetTime() - P.listed_at > 1.5) reload_local();
    if (P.op && P.op->done) {
        P.ok = P.op->val.first;
        P.result = P.ok ? "Restored. The previous version is kept: press Undo restore to go back."
                        : "Could not restore: " + (P.op->val.second.empty() ? std::string("try again later") : P.op->val.second);
        P.op.reset();
        A.vault_dirty = true;
        reload_local();
        reload_cloud();
    }
    std::vector<hs::CloudVer> cv;
    if (P.cloud && P.cloud->done)
        for (auto& f : P.cloud->val)
            for (size_t i = 0; i < f.second.size(); ++i) {
                const auto& v = f.second[i];
                cv.push_back({f.first, v.ver, v.ts ? v.ts : v.mtime, v.size, v.device_name, i == 0});
            }
    auto rows = hs::history_merge(P.local, cv);
    const bool busy = A.proc != nullptr || (P.op && !P.op->done);

    begin_panel("save_history");
    section_title("Save history: " + P.title);
    text_wrapped_disabled("The last versions of this game's save, from this PC and from the cloud. Restoring keeps an undo copy first.");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    if (rows.empty()) {
        ImGui::TextDisabled(P.cloud && !P.cloud->done ? "Loading..." : "No earlier versions yet. They appear after you play and the save changes.");
    } else if (ImGui::BeginTable("histtbl", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("When", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("From", ImGuiTableColumnFlags_WidthStretch, 2.2f);
        ImGui::TableSetupColumn("Device", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 0.9f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.8f);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < rows.size(); ++i) {
            const hs::HistRow& r = rows[i];
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(when_text(r.ts).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.cloud ? ("Cloud  " + r.path).c_str() : (r.files > 1 ? "This PC  (" + std::to_string(r.files) + " files)" : std::string("This PC")).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.device.empty() ? "-" : r.device.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(kb_text(r.size).c_str());
            ImGui::TableNextColumn();
            if (r.current) {
                ImGui::TextDisabled("Current");
            } else if (button("Restore", ImVec2(0, 0), false, !busy)) {
                P.result.clear();
                if (!r.cloud) {
                    std::string err;
                    P.ok = hs::history_restore(P.hist_dir, P.battery_dir, r.id, now_s(), &err);
                    P.result = P.ok ? "Restored. Press Undo restore to go back." : "Could not restore: " + err;
                    A.vault_dirty = true;
                    reload_local();
                    if (P.ok) set_toast("Restored an earlier save of " + P.title + ".", true, 4.0);
                } else {
                    hs::history_mark_undo(P.hist_dir, P.battery_dir, now_s(), "this PC");
                    P.op = std::make_shared<Task<std::pair<bool, std::string>>>();
                    auto t = P.op;
                    hs::GameId id = P.id;
                    std::string path = r.path;
                    int ver = r.ver;
                    std::thread([t, id, path, ver]() { std::string e; bool ok = hs::cloud_restore_version(id, path, ver, &e); t->val = {ok, e}; t->done = true; }).detach();
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (A.proc) text_wrapped_disabled("Close the running game before restoring a save.");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    if (P.has_undo) {
        if (button("Undo restore", ImVec2(0, 0), true, !busy)) {
            std::string err;
            P.ok = hs::history_undo(P.hist_dir, P.battery_dir, &err);
            P.result = P.ok ? "Put your save back the way it was before the restore." : "Could not undo: " + err;
            A.vault_dirty = true;
            reload_local();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Your save from %s", when_text(P.undo.ts).c_str());
        ImGui::SameLine();
    }
    if (button("Export save...", ImVec2(0, 0), false, !g_headless)) {
        std::wstring nm = L"Save-" + std::wstring(P.id.key.begin(), P.id.key.end()) + L".zip";
        std::string path = pick_save_path(A.hwnd, nm.c_str(), L"Zip archive", L"*.zip", L"zip");
        if (!path.empty()) {
            std::string err;
            P.ok = hs::export_game_zip(hs::game_dir(P.id.system, P.id.key), path, true, &err);
            P.result = P.ok ? "Saved a zip you can share or keep as a backup." : "Could not export: " + err;
        }
    }
    ImGui::SameLine();
    if (button("Close", ImVec2(0, 0))) P.open = false;
    result_line(P.result, P.ok);
    end_panel();
    ImGui::Dummy(ImVec2(0, 4 * S));
}

/* Details modal: one small line + button. */
void history_ui_details_section(const hub::GameEntry& e) {
    static std::string key;
    static double at = -10;
    static size_t n = 0;
    static int64_t newest = 0;
    if (key != e.key || ImGui::GetTime() - at > 3.0) {
        key = e.key;
        at = ImGui::GetTime();
        auto l = hs::history_list(hs::game_history_dir(sys_short(e), e.key));
        n = l.size();
        newest = l.empty() ? 0 : l[0].ts;
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Save history:");
    ImGui::SameLine(0, sp(0.75f));
    ImGui::TextDisabled("%s", n ? (std::to_string(n) + " on this PC, latest " + when_text(newest)).c_str() : "none yet");
    ImGui::SameLine(0, sp(1.f));
    if (button("History...", ImVec2(0, 0))) {
        history_ui_open(sys_short(e), e.key);
        ImGui::CloseCurrentPopup();
        A.page = Page::Saves;
    }
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
}

/* ---------------- stats ---------------- */

namespace {
struct StatsCache {
    double at = -100;
    hs::Stats st;
    bool any = false;
} SC;

std::string hours_text(int64_t s) {
    char b[32];
    if (s < 60) return s <= 0 ? "0 min" : "<1 min";
    if (s < 3600) { std::snprintf(b, sizeof b, "%lld min", (long long)(s / 60)); return b; }
    std::snprintf(b, sizeof b, "%.1f h", s / 3600.0);
    return b;
}

void stat_cell(const char* label, const std::string& value) {
    ImGui::BeginGroup();
    ImGui::PushFont(ui::F.h2);
    ImGui::TextUnformatted(value.c_str());
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::EndGroup();
}
}  // namespace

void stats_ui_card() {
    wrapped_ui_modal();   // L8: RetroHub Wrapped popup (opened by the button below)
    if (ImGui::GetTime() - SC.at > 30.0) {
        SC.at = ImGui::GetTime();
        std::vector<hs::GameTotal> tot;
        for (auto& e : A.lib) {
            if (is_native(e)) continue;
            const hub::GameState& gs = gstate(e.key);
            if (gs.play_seconds) tot.push_back({sys_short(e), e.key, (int64_t)gs.play_seconds});
        }
        std::time_t t = std::time(nullptr);
        std::tm lt{};
        localtime_s(&lt, &t);
        std::tm gm{};
        gmtime_s(&gm, &t);
        int tz = (int)(std::difftime(std::mktime(&lt), std::mktime(&gm)));
        if (lt.tm_isdst > 0) tz += 3600;
        SC.st = hs::compute_stats(tot, hs::playlog_read_all(), (int64_t)t, tz);
        SC.any = SC.st.total_secs > 0;
    }
    if (!SC.any && std::getenv("HUB_SHOT_FAKE_FRIENDS")) {   // screenshot aid: a stats card with fake numbers so the Rivals section shows
        SC.st.total_secs = 90000; SC.st.week_secs = 7200; SC.st.longest.secs = 5400; SC.st.streak_days = 3;
        SC.st.top = {{"n64", "smash64", 50000}, {"gba", "x", 20000}}; SC.st.systems = {{"n64", 60000}, {"gba", 30000}};
        SC.any = true;
    }
    if (!SC.any) return;
    const hs::Stats& st = SC.st;
    const float bleed = sp(1.5f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + bleed);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(sp(2.f), sp(1.f)));
    begin_panel("stats_card", ImVec2(ImGui::GetContentRegionAvail().x - bleed, 0));
    section_title("Your stats");
    ImGui::SameLine(0, sp(2.f));
    if (button("Wrapped", ImVec2(0, 0), false, true, ui::Icon::Star)) wrapped_ui_open();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Your month or year in games");
    const float w = ImGui::GetContentRegionAvail().x;
    const bool wide = w > sp(70.f);
    stat_cell("Total play time", hours_text(st.total_secs));
    ImGui::SameLine(0, sp(5.f));
    stat_cell("This week", hours_text(st.week_secs));
    if (st.longest.secs > 0) {
        ImGui::SameLine(0, sp(5.f));
        stat_cell("Longest session", hours_text(st.longest.secs));
    }
    if (st.streak_days > 1) {
        ImGui::SameLine(0, sp(5.f));
        stat_cell("Day streak", std::to_string(st.streak_days) + " days");
    }
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    ImGui::BeginGroup();
    ImGui::PushFont(ui::F.bold);
    ImGui::TextUnformatted("Most played");
    ImGui::PopFont();
    const float bar_w = wide ? sp(26.f) : sp(14.f);
    for (auto& t : st.top) {
        std::string nm = title_for(t.key, t.key);
        ImGui::TextUnformatted(ui::ellipsize(ui::F.body, ui::F.body->FontSize, nm, sp(wide ? 30.f : 20.f)).c_str());
        ImGui::SameLine(sp(wide ? 32.f : 22.f));
        ImVec2 p = ImGui::GetCursorScreenPos();
        float h = sp(0.8f), f = st.top[0].secs ? (float)t.secs / st.top[0].secs : 0.f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float y = p.y + (ImGui::GetTextLineHeight() - h) * 0.5f;
        dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + bar_w, y + h), ui::PANEL3, h * 0.5f);
        dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + std::max(h, bar_w * f), y + h), ui::ACCENT, h * 0.5f);
        ImGui::Dummy(ImVec2(bar_w, ImGui::GetTextLineHeight()));
        ImGui::SameLine(0, sp(1.f));
        ImGui::TextDisabled("%s", hours_text(t.secs).c_str());
    }
    ImGui::EndGroup();
    if (!st.systems.empty()) {
        if (wide) ImGui::SameLine(0, sp(6.f)); else ImGui::Dummy(ImVec2(0, sp(0.5f)));
        ImGui::BeginGroup();
        ImGui::PushFont(ui::F.bold);
        ImGui::TextUnformatted("By system");
        ImGui::PopFont();
        for (size_t i = 0; i < st.systems.size() && i < 6; ++i) {
            const auto& s = st.systems[i];
            int pct = st.total_secs ? (int)(s.second * 100 / st.total_secs) : 0;
            ImGui::Text("%s", hub::system_name(hub::system_from_short(s.first)));
            ImGui::SameLine(sp(14.f));
            ImGui::TextDisabled("%s  (%d%%)", hours_text(s.second).c_str(), pct);
        }
        ImGui::EndGroup();
    }
    rivals_draw_card();
    end_panel();
    ImGui::PopStyleVar();
    ImGui::Dummy(ImVec2(0, sp(1.f)));
}
