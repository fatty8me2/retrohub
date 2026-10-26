#include "app.h"

/* Play again / recent groups / notification history. See group_logic.h for the rules. */

using namespace hubui::group;

GroupUi GRP;

static std::string dir_a() { return hub::data_dir(); }
static std::string dir_b() { return hub::saves::vault_dir(); }


static World world_now() {
    World w;
    w.now = wall_ms();
    for (auto& r : fr_incoming()) w.pending_request_uids.push_back(r.uid);
    w.update_available = hubupdate::update_available();
    w.conflicts = cloud_ui_conflict_count();
    return w;
}

static void persist(bool force = false) {
    if (g_headless || FR.fake) return;
    double now = ImGui::GetTime();
    if (!force && now - GRP.saved_at < 1.0) return;
    GRP.saved_at = now;
    if (GRP.hist.dirty) { save_history(GRP.hist, dir_a(), dir_b()); GRP.hist.dirty = false; }
}

void group_note(Kind k, const std::string& uid, const std::string& name, const std::string& game_id, const std::string& title,
                const std::string& code, const std::string& text, long long expires_at) {
    Entry e;
    e.kind = k; e.uid = uid; e.name = name; e.game_id = game_id; e.title = title; e.code = code; e.text = text;
    e.at = wall_ms(); e.expires_at = expires_at;
    GRP.hist.add(e);
}
void group_note_invite(const std::string& uid, const std::string& name, const hubnet::Invite& inv) {
    group_note(Kind::Invite, uid, name, inv.game_id, inv.game_title, inv.lobby_code, "", inv.expires_at);
}
void group_note_ask_join(const std::string& uid, const std::string& name, const std::string& code, const std::string& title) {
    group_note(Kind::AskJoin, uid, name, "", title, code, "", wall_ms() + 10LL * 60 * 1000);
}
void group_resolve(Kind k, const std::string& uid, const std::string& code) { GRP.hist.resolve(k, uid, code); }

void group_tick() {
    if (!GRP.loaded) {
        GRP.loaded = true;
        if (!g_headless && !FR.fake) { GRP.groups = load_groups(dir_a(), dir_b()); GRP.hist = load_history(dir_a(), dir_b()); GRP.hist.dirty = false; }
    }
    // friend requests waiting for an answer get a history row (answered ones are marked in fr_accept / the bell)
    for (auto& r : fr_incoming()) {
        bool have = false;
        for (auto& e : GRP.hist.v) if (e.kind == Kind::FriendRequest && e.uid == r.uid && !e.done) { have = true; break; }
        if (!have) group_note(Kind::FriendRequest, r.uid, r.name, "", "", "", "", 0);
    }
    int cf = cloud_ui_conflict_count();
    if (cf > GRP.last_conflicts) group_note(Kind::CloudConflict, "", "", "", "", "", cf == 1 ? "A save conflict needs your decision" : std::to_string(cf) + " save conflicts need your decision", 0);
    GRP.last_conflicts = cf;
    bool up = hubupdate::update_available();
    if (up && !GRP.last_update) group_note(Kind::Update, "", "", "", "", "", "RetroHub " + hubupdate::latest_version() + " is available", 0);
    GRP.last_update = up;
    if (GRP.bell_seen && ImGui::GetFrameCount() - GRP.bell_frame > 1) { GRP.hist.mark_all_read(); GRP.bell_seen = false; }
    if (!GRP.pending_invite.empty()) {   // "Invite last group": the new room is up
        if (!A.in_room && ImGui::GetTime() - GRP.pending_at > 5.0) GRP.pending_invite.clear();
        else if (ImGui::GetTime() - GRP.pending_at > 45.0) GRP.pending_invite.clear();
        else if (A.in_room && room_connected() && is_host()) { auto u = GRP.pending_invite; GRP.pending_invite.clear(); fr_invite_uids(u); }
    }
    persist();
}

int group_unread() { return GRP.hist.unread(); }

/* ---- recent groups ---- */
void group_session_ended(unsigned long long secs) {
    if (!A.in_room || A.selected_game.empty()) return;
    GRP.again = true;
    GRP.again_game = A.selected_game;
    if (secs < 20) return;   // not a real session
    RecentGroup g;
    int me = room_my_slot();
    std::vector<std::string> names;
    for (auto& m : room_members()) if (m.slot != me) names.push_back(m.name);
    std::vector<FriendRef> frs;
    for (auto& f : fr_friends()) frs.push_back({f.uid, f.name, f.status.lobby_code});
    g.members = match_friends(names, frs, A.code);
    g.game_id = A.selected_game;
    if (const hub::GameEntry* e = find_room_entry(A.selected_game)) g.game_title = e->title;
    else g.game_title = A.proc_title;
    g.at = wall_ms();
    if (GRP.groups.record(g) && !g_headless && !FR.fake) save_groups(GRP.groups, dir_a(), dir_b());
}
void group_left_room() { GRP.again = false; GRP.again_game.clear(); }
bool group_play_again_active() {
    return play_again_applies(GRP.again, A.proc != nullptr, !GRP.again_game.empty() && GRP.again_game != A.selected_game);
}
void group_decorate_room_action(bool update_mode, bool is_launch, bool ready_on, bool enabled, std::string& label, std::string& headline) {
    if (update_mode || !group_play_again_active()) return;
    std::string nl = play_again_label(label, is_launch, ready_on, enabled, true);
    if (nl == label) return;
    label = nl;
    headline = is_launch ? "Everyone's back - play again?" : "Play another round?";
}
const RecentGroup* group_last() { return GRP.groups.last(); }
std::string group_last_label() {
    const RecentGroup* g = group_last();
    return g ? "Invite last group (" + group_names(*g, 2) + ")" : std::string();
}
std::string group_together_text(const std::string& uid) {
    return together_text(GRP.groups.last_with(uid), wall_ms());
}
void group_invite_last() {
    const RecentGroup* g = group_last();
    if (!g || A.proc) return;
    std::vector<std::string> fu, none;
    for (auto& f : fr_friends()) fu.push_back(f.uid);
    std::vector<std::string> u = invitable(*g, fu, none);
    if (u.empty()) { set_notice("Nobody from your last group is on your friends list any more."); return; }
    if (!g->game_id.empty() && find_room_entry(g->game_id)) A.pending_room_game = g->game_id;
    GRP.pending_invite = u;
    GRP.pending_at = ImGui::GetTime();
    enter_room(true, hubnet::new_room_code());
    if (!A.in_room) GRP.pending_invite.clear();
}

/* ---- bell list ---- */
static void draw_entry(Entry& e, int idx, float cw, const World& w) {
    State s = state_of(e, w);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    std::string act = action_label(e, s);
    float pad = sp(1.25f), h = sp(7.5f);
    ImVec2 p1(p0.x + cw, p0.y + h);
    dl->AddRectFilled(p0, p1, e.read ? ui::PANEL : ui::PANEL2, sp(1.5f));
    if (s == State::Active) dl->AddRect(p0, p1, with_alpha(ui::ACCENT, 0.4f), sp(1.5f), 0, 1.f);
    if (!e.read) dl->AddCircleFilled(ImVec2(p0.x + sp(1.f), p0.y + sp(1.f)), sp(0.55f), ui::BAD, 12);
    float bw = sp(10.f), by = p0.y + (h - sp(3.5f)) * 0.5f;
    float rx = p1.x - pad;
    if (!act.empty()) rx -= bw + sp(0.75f) + sp(3.5f);   // action + dismiss
    float tx = p0.x + pad + sp(1.f), tw = std::max(sp(8.f), rx - tx);
    ImU32 headc = s == State::Active || s == State::Info ? ui::TEXT : ui::TEXT2;
    dl->AddText(ui::F.bold, ui::F.bold->FontSize, ImVec2(tx, p0.y + pad), headc,
                ui::ellipsize(ui::F.bold, ui::F.bold->FontSize, headline(e), tw).c_str());
    std::string sub = ago_text(e.at, w.now);
    if (!e.code.empty() && (e.kind == Kind::Invite || e.kind == Kind::AskJoin)) sub = "Room " + e.code + "  -  " + sub;
    const char* sl = state_label(s);
    if (*sl) sub += std::string("  -  ") + sl;
    else if ((e.kind == Kind::Invite || e.kind == Kind::AskJoin) && e.expires_at > 0) {
        int left = hubui::invite_seconds_left(e.expires_at, w.now);
        sub += left >= 120 ? "  -  " + std::to_string(left / 60) + " min left" : "  -  " + std::to_string(left) + "s left";
    }
    dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(tx, p0.y + pad + ui::F.bold->FontSize * 1.3f), s == State::Expired ? ui::BAD : ui::TEXT3,
                ui::ellipsize(ui::F.cap, ui::F.cap->FontSize, sub, tw).c_str());
    ImGui::PushID(idx);
    if (!act.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(p1.x - pad - bw - sp(0.75f) - sp(3.5f), by));
        if (ui::button(act.c_str(), ImVec2(bw, sp(3.5f)), true)) {
            switch (e.kind) {
                case Kind::Invite: fr_join_lobby(e.code, e.game_id, e.title, e.name); e.done = true; ImGui::CloseCurrentPopup(); break;
                case Kind::AskJoin: fr_invite_uids({e.uid}); e.done = true; break;
                case Kind::FriendRequest: fr_accept(e.uid, e.name); e.done = true; break;
                case Kind::CloudConflict: A.page = Page::Saves; ImGui::CloseCurrentPopup(); break;
                case Kind::Update: hubupdate::start_update(); break;
                default: break;
            }
            e.read = true; GRP.hist.dirty = true;
        }
        ImGui::SetCursorScreenPos(ImVec2(p1.x - pad - sp(3.5f), by));
        if (ui::button("X", ImVec2(sp(3.5f), sp(3.5f)), false, true)) {
            if (e.kind == Kind::FriendRequest) { if (!FR.fake) FR.cli.decline(e.uid); else FR.f_in.clear(); }
            e.done = true; e.read = true; GRP.hist.dirty = true;
        }
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(cw, h + sp(0.5f)));
}

void group_draw_bell_list(float cw) {
    GRP.bell_frame = ImGui::GetFrameCount();
    GRP.bell_seen = true;
    World w = world_now();
    if (GRP.hist.v.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
        ImGui::TextUnformatted("Nothing yet. Invites, friend requests and updates show up here.");
        ImGui::PopStyleColor();
        return;
    }
    if (ImGui::SmallButton("Clear all")) { GRP.hist.clear(); return; }
    ImGui::Dummy(ImVec2(0, sp(0.25f)));
    for (size_t i = 0; i < GRP.hist.v.size(); ++i) draw_entry(GRP.hist.v[i], (int)i, cw, w);
}

void group_fake_seed() {
    long long now = wall_ms();
    GRP.loaded = true;
    GRP.groups = RecentGroups();
    RecentGroup g;
    g.members = {{"miauid", "Mia"}, {"jakuid", "Jake"}};
    g.game_id = "mk64"; g.game_title = "Mario Kart 64"; g.at = now - 2 * kDayMs;
    GRP.groups.record(g);
    RecentGroup g2;
    g2.members = {{"kaiuid", "Kai"}};
    g2.game_id = "smash64"; g2.game_title = "Super Smash Bros."; g2.at = now - 5 * 3600000LL;
    GRP.groups.record(g2);
    GRP.hist = NotifHistory();
    GRP.hist.add({Kind::Update, "", "", "", "", "", "RetroHub v9.9.9 is available", now - 6 * kDayMs, 0, true, false});
    GRP.hist.add({Kind::CloudConflict, "", "", "", "", "", "A save conflict needs your decision", now - 3 * kDayMs, 0, true, false});
    GRP.hist.add({Kind::Invite, "kaiuid", "Kai", "mk64", "Mario Kart 64", "ZXCV23", "", now - 3 * 3600000LL, now - 2 * 3600000LL, true, false});
    GRP.hist.add({Kind::Invite, "miauid", "Mia", "mk64", "Mario Kart 64", "K7M2QX", "", now - 4 * 60000LL, now + 25 * 60000LL, false, false});
    GRP.hist.add({Kind::FriendRequest, "oliuid", "Oliver", "", "", "", "", now - 60000, 0, false, false});
    GRP.hist.dirty = false;
}
