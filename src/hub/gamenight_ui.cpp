/* Game night (Plan 3 S6): "Plan a session" in a friend's chat, the stored plans, the reminder and the Host / Join card.
 * Logic and the DM format are in gamenight_logic.h. No server scheduler: plans travel as ordinary DMs and every client checks its own
 * clock once per frame. While a game runs nothing pops up: the reminder and the start notice go to the bell only. */
#include <ctime>

#include "app.h"
#include "friends_ui.h"
#include "gamenight_logic.h"
#include "group_ui.h"
#include "ui_sound.h"

using namespace hubui::night;

namespace {

struct GnState {
    Plans plans;
    bool loaded = false;
    // plan popup
    bool open = false;
    int game_ix = 0, day = 0, hour = 20, min5 = 0;
    std::set<std::string> pick;
    std::string err;
    // host flow: after the room opens, invite these friends once
    std::vector<std::string> pending_invite;
    double pending_at = 0;
};
GnState GN;

std::string path() { return hub::data_dir() + "gamenights.json"; }
void load_once() {
    if (GN.loaded) return;
    GN.loaded = true;
    load(path(), &GN.plans);
}
void persist() { save(path(), GN.plans); }

std::string local_label(long long start_ms) {
    std::time_t t = (std::time_t)(start_ms / 1000);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    char b[48];
    std::strftime(b, sizeof b, "%a %I:%M %p", &tmv);
    return b[4] == '0' ? std::string(b, 4) + (b + 5) : std::string(b);   // "Fri 08:00 PM" -> "Fri 8:00 PM"
}

std::vector<const hub::GameEntry*> online_games() {
    std::vector<const hub::GameEntry*> v;
    for (auto& e : A.lib) if (supports_online(e) && !is_remote_stand_in(e)) v.push_back(&e);
    return v;
}

/* "Quiet" = a game or session owns the screen: bell only. */
bool quiet() { return game_active() && !g_headless; }

void note_bell(const Night& n, const std::string& text) {
    group_note(hubui::group::Kind::Info, n.host_uid, n.host_name, n.game_id, n.game_title, "", text, 0);
}

std::string who(const Night& n) { return n.mine ? std::string("You") : (n.host_name.empty() ? std::string("A friend") : n.host_name); }

}  // namespace

std::string gn_display_text(const std::string& text) { return display_text(text); }

bool gn_on_message(const std::string& uid, const std::string& name, const std::string& text) {
    load_once();
    Night n;
    if (!parse(text, &n)) return false;
    n.host_uid = uid; n.host_name = name; n.id = make_id(uid, n.start_ms);
    if (GN.plans.add(n, wall_ms())) {
        persist();
        std::string t = name + " planned a game night: " + (n.game_title.empty() ? std::string("a session") : n.game_title) + " on " + local_label(n.start_ms);
        if (quiet()) note_bell(n, t); else { ui_sound::play(ui_sound::Sfx::Notify); push_toast(t, ui::ACCENT, 8.0); }
    }
    return true;
}

void gn_tick() {
    load_once();
    long long now = wall_ms();
    bool dirty = false;
    for (auto& n : GN.plans.v) {
        if (reminder_due(n, now)) {
            n.reminded = true; dirty = true;
            std::string t = "Game night " + when_text(n, now) + ": " + n.game_title + (n.mine ? std::string() : " with " + who(n));
            if (quiet()) note_bell(n, t); else { ui_sound::play(ui_sound::Sfx::Notify); push_toast(t, ui::ACCENT, 10.0); }
        }
        if (start_due(n, now)) {
            n.started = true; dirty = true;
            if (quiet()) note_bell(n, "Game night is starting: " + n.game_title + ". Open RetroHub to " + (n.mine ? "host" : "join") + ".");
            else fr_notify_night(n.id, n.mine ? "You" : n.host_name);
        }
    }
    if (dirty) persist();
    if (!GN.pending_invite.empty()) {   // we hosted from the card: invite once the room is up
        if (A.in_room && room_connected()) { fr_invite_uids(GN.pending_invite); GN.pending_invite.clear(); }
        else if (ImGui::GetTime() - GN.pending_at > 20.0) GN.pending_invite.clear();
    }
}

/* Card text for a Notif with `night` set. */
bool gn_card_info(const std::string& id, std::string* title, std::string* sub, const char** action) {
    load_once();
    Night* n = GN.plans.find(id);
    if (!n) return false;
    long long now = wall_ms();
    *title = "Game night: " + n->game_title;
    *sub = who(*n) + " - " + when_text(*n, now);
    bool can_join = false;
    if (!n->mine) { const hubnet::Friend* f = fr_find(n->host_uid); can_join = f && !f->status.lobby_code.empty() && f->status.joinable; }
    *action = n->mine || !can_join ? "Host" : "Join";
    return phase_of(*n, now) != Phase::Over;
}

void gn_card_action(const std::string& id) {
    load_once();
    Night* n = GN.plans.find(id);
    if (!n) return;
    if (!n->mine) {
        const hubnet::Friend* f = fr_find(n->host_uid);
        if (f && !f->status.lobby_code.empty() && f->status.joinable) { fr_join_lobby(f->status.lobby_code, n->game_id, n->game_title, n->host_name); return; }
    }
    if (A.proc) { set_notice("Close the running game first."); return; }
    if (!n->game_id.empty() && find_room_entry(n->game_id)) A.pending_room_game = n->game_id;
    GN.pending_invite = n->mine ? n->invitees : std::vector<std::string>{n->host_uid};
    GN.pending_at = ImGui::GetTime();
    enter_room(true, hubnet::new_room_code());
    if (!A.in_room) GN.pending_invite.clear();
}

void gn_card_dismiss(const std::string& id) {
    load_once();
    if (Night* n = GN.plans.find(id)) { n->dismissed = true; persist(); }
}

/* Chat header button + popup. `cx`/`cy` = top-right corner of the header area. */
void gn_plan_button(const std::string& uid, ImVec2 pos, float h) {
    load_once();
    float w = 0;
    if (fr_pill_button("plan_night", "Plan a session", pos, h, false, true, &w)) {
        GN.open = true; GN.err.clear(); GN.pick.clear(); GN.pick.insert(uid);
        auto gs = online_games();
        GN.game_ix = 0;
        for (size_t i = 0; i < gs.size(); ++i) if (room_id(*gs[i]) == A.st.last_game) GN.game_ix = (int)i;
        std::time_t t = std::time(nullptr) + 3600;
        std::tm tmv{}; localtime_s(&tmv, &t);
        GN.day = 0; GN.hour = tmv.tm_hour; GN.min5 = (tmv.tm_min / 5);
        ImGui::OpenPopup("##plan_night");
    }
}

void gn_plan_popup() {
    if (!GN.open) return;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(sp(52.f), ImGui::GetIO().DisplaySize.x - sp(4.f)), 0));
    if (!ImGui::BeginPopupModal("##plan_night", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) { GN.open = false; return; }
    ImGui::PushFont(ui::F.bold);
    ImGui::TextUnformatted("Plan a session");
    ImGui::PopFont();
    ImGui::TextDisabled("Friends get an invite with the time. Reminder 10 minutes before, then one click to host or join.");
    ImGui::Dummy(ImVec2(0, sp(0.75f)));
    auto gs = online_games();
    if (gs.empty()) ImGui::TextDisabled("No online games in your library yet.");
    else {
        GN.game_ix = std::max(0, std::min(GN.game_ix, (int)gs.size() - 1));
        if (ImGui::BeginCombo("Game", gs[GN.game_ix]->title.c_str())) {
            for (int i = 0; i < (int)gs.size(); ++i) if (ImGui::Selectable(gs[i]->title.c_str(), i == GN.game_ix)) GN.game_ix = i;
            ImGui::EndCombo();
        }
    }
    std::time_t base = std::time(nullptr);
    static const char* kDays[] = {"Today", "Tomorrow", "In 2 days", "In 3 days", "In 4 days", "In 5 days", "In 6 days"};
    ImGui::SetNextItemWidth(sp(18.f));
    ImGui::Combo("Day", &GN.day, kDays, 7);
    ImGui::SetNextItemWidth(sp(14.f));
    ImGui::SliderInt("Hour", &GN.hour, 0, 23);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(sp(14.f));
    int mm = GN.min5 * 5;
    if (ImGui::SliderInt("Minute", &mm, 0, 55, "%d", ImGuiSliderFlags_AlwaysClamp)) GN.min5 = mm / 5;
    std::tm tmv{}; localtime_s(&tmv, &base);
    tmv.tm_mday += GN.day; tmv.tm_hour = GN.hour; tmv.tm_min = GN.min5 * 5; tmv.tm_sec = 0; tmv.tm_isdst = -1;
    long long start = (long long)std::mktime(&tmv) * 1000;
    ImGui::TextDisabled("Starts %s", local_label(start).c_str());
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    ImGui::TextUnformatted("Invite");
    ImGui::BeginChild("##gn_friends", ImVec2(0, sp(20.f)), ImGuiChildFlags_Borders);
    for (auto& f : fr_friends()) {
        bool on = GN.pick.count(f.uid) > 0;
        if (ImGui::Checkbox((f.name + "##gn" + f.uid).c_str(), &on)) { if (on) GN.pick.insert(f.uid); else GN.pick.erase(f.uid); }
    }
    ImGui::EndChild();
    if (!GN.err.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN); ImGui::TextWrapped("%s", GN.err.c_str()); ImGui::PopStyleColor(); }
    // upcoming
    load_once();
    auto act = GN.plans.active(wall_ms());
    if (!act.empty()) {
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        ImGui::TextDisabled("Coming up");
        for (auto* n : act) ImGui::BulletText("%s - %s (%s)", n->game_title.c_str(), local_label(n->start_ms).c_str(), who(*n).c_str());
    }
    ImGui::Dummy(ImVec2(0, sp(0.75f)));
    if (ui::button("Send invite", ImVec2(sp(14.f), sp(4.5f)), true, !gs.empty())) {
        Night n;
        n.mine = true; n.start_ms = start;
        n.game_id = room_id(*gs[GN.game_ix]); n.game_title = gs[GN.game_ix]->title;
        for (auto& u : GN.pick) n.invitees.push_back(u);
        GN.err = plan_error(n, wall_ms());
        if (GN.err.empty()) {
            n.id = make_id("me", n.start_ms);
            n.host_uid = fr_my_code();
            std::string text = encode(n, local_label(n.start_ms));
            for (auto& u : n.invitees) fr_send_text(u, text);
            n.reminded = false;
            if (GN.plans.add(n, wall_ms())) persist();
            set_toast("Game night sent to " + std::to_string(n.invitees.size()) + (n.invitees.size() == 1 ? " friend." : " friends."));
            GN.open = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ui::button("Cancel", ImVec2(sp(10.f), sp(4.5f)))) { GN.open = false; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

void gn_fake_seed() {   // screenshots: one night starting right now (HUB_SHOT_FAKE_NIGHT)
    GN.loaded = true;
    GN.plans = Plans();
    Night n;
    n.host_uid = "miauid"; n.host_name = "Mia"; n.game_id = "mk64"; n.game_title = "Mario Kart 64";
    n.start_ms = wall_ms() - 30000; n.id = make_id(n.host_uid, n.start_ms);
    GN.plans.v.push_back(n);
}
