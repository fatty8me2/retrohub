/* Plan 4 Wave 3 L2: completion status + a short note per game (synced prefs.progress), shown in the details modal's Info tab and as a
 * small badge on tiles. Also the facts a smart shelf rule (L1) is matched against. The prefs cache and writes live in customize_ui.cpp
 * (custom_progress / custom_set_status / custom_set_note); this file is the drawing. */
#include "app.h"
#include "gameinfo.h"
#include "osk.h"
#include "progress_ui.h"
#include "wave3_hooks.h"

namespace {

struct NoteState {
    std::string key, base;   // base = the stored note the box was last loaded from
    char buf[1100] = {0};   // 500 characters (up to 2 bytes each for accents) plus slack
};
NoteState NS;

struct YearCache { std::map<std::string, std::pair<int, int>> m; };   // key -> (year, players)

std::pair<int, int> year_players(const hub::GameEntry& e) {
    static YearCache c;
    auto it = c.m.find(e.key);
    if (it != c.m.end()) return it->second;
    gameinfo::Query q;
    if (e.catalog_id != "remix" && e.catalog_id != "kanto") {
        q.system = hub::system_short(e.system);
        q.title = custom_original_title(e);
        q.serial = e.serial;
        if (e.catalog_id == "smash64") q.alt_title = "Super Smash Bros.";
        else if (const GameInfo* g = catalog_for(e)) q.alt_title = g->title;
    }
    std::pair<int, int> r(0, 0);
    if (!q.system.empty()) {
        gameinfo::Meta m = gameinfo::shared_db(hub::exe_dir() + "data/").lookup(q);
        r = std::make_pair(m.year, m.players);
    }
    if (const GameInfo* g = catalog_for(e)) if (!r.first) r.first = g->year;
    return c.m[e.key] = r;
}

}  // namespace

std::string progress_tag(const std::string& key, std::string* tip) {
    const hub::prefs::Progress* p = custom_progress(key);
    if (!p) return std::string();
    std::string b = hubui::prog::status_badge(p->status);
    if (!b.empty() && tip) *tip = "Your status: " + hubui::prog::status_label(p->status);
    return b;
}

hubui::prog::Facts progress_facts(const hub::GameEntry& e) {
    hubui::prog::Facts f;
    f.key = e.key;
    f.system = hub::console_id(e);   // a console id: smart-shelf rules tell Switch and Switch 2 apart
    f.genre = genre_of_entry(e);
    std::pair<int, int> yp = year_players(e);
    f.year = yp.first;
    f.players = std::max(yp.second, local_players_for(e));
    const hub::GameState& gs = gstate(e.key);
    f.favorite = gs.favorite;
    f.last_played = gs.last_played;
    if (const hub::prefs::Progress* p = custom_progress(e.key)) f.status = p->status;
    return f;
}

std::vector<std::string> progress_genres() {
    std::set<std::string> s;
    for (const auto& e : A.lib) s.insert(genre_of_entry(e));
    std::vector<std::string> v;
    for (const auto& g : s) if (g != "Other") v.push_back(g);
    if (s.count("Other")) v.push_back("Other");
    return v;
}

void progress_details_section(const hub::GameEntry& e) {
    if (e.key.empty() || e.key.rfind("remote:", 0) == 0) return;
    const hub::prefs::Progress* cur = custom_progress(e.key);
    const std::string status = cur ? cur->status : std::string();
    const std::string note = cur ? cur->note : std::string();
    ImGui::Dummy(ImVec2(0, sp(0.75f)));
    ImGui::PushFont(ui::F.bold);
    ImGui::TextUnformatted("Your progress");
    ImGui::PopFont();
    ImGui::PushID("progress");
    bool first = true;
    for (const auto& s : hubui::prog::statuses()) {
        if (!first) ImGui::SameLine(0, sp(0.75f));
        first = false;
        const bool on = status == s.id;
        if (ui::button(s.label, ImVec2(0, sp(3.5f)), on, true, on ? ui::Icon::Check : ui::Icon::None)) custom_set_status(e.key, on ? std::string() : std::string(s.id));
    }
    if (NS.key != e.key) { NS.key = e.key; NS.base = note; std::snprintf(NS.buf, sizeof NS.buf, "%s", note.c_str()); }
    else if (NS.base != note) {   // the note changed elsewhere (another PC synced): reload the box unless the player is mid-edit
        if (hubui::prog::clean_note(NS.buf) == NS.base) std::snprintf(NS.buf, sizeof NS.buf, "%s", note.c_str());
        NS.base = note;
    }
    ImGui::SetNextItemWidth(std::min(sp(60.f), ImGui::GetContentRegionAvail().x - sp(14.f)));
    bool go = osk::input("##progress_note", "Add a short note", NS.buf, sizeof NS.buf, ImGuiInputTextFlags_EnterReturnsTrue);
    go = go || ImGui::IsItemDeactivatedAfterEdit();
    const std::string typed = hubui::prog::clean_note(NS.buf);
    ImGui::SameLine(0, sp(1.f));
    if (ui::button("Save note", ImVec2(0, sp(3.5f)), false, typed != note)) go = true;
    if (go && typed != note) {
        if (custom_set_note(e.key, typed)) std::snprintf(NS.buf, sizeof NS.buf, "%s", typed.c_str());
        else push_toast("Couldn't save the note", COL_BAD, 3.0);
    }
    ImGui::PopID();
}
