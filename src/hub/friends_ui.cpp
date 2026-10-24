#include "app.h"
#include "toast.h"

FriendsUi FR;

/* friends_ui.inc - friends list panel, chat, lobby invites, notifications. Included by main.cpp (after the art helpers),
 * so it can use App A and its helpers. Data comes from hubnet::FriendsClient, or from fake data (HUB_SHOT_FAKE_FRIENDS). */




long long wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

/* ---------- data accessors (real client or fake) ---------- */
static void fr_refresh_lists() {
    int fc = ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : 0;
    if (FR.d_frame == fc && fc != 0) return;
    FR.d_frame = fc;
    FR.d_friends = FR.fake ? FR.f_friends : FR.cli.friends();
    FR.d_in = FR.fake ? FR.f_in : FR.cli.incoming_requests();
    FR.d_out = FR.fake ? FR.f_out : FR.cli.outgoing_requests();
    std::string me = FR.fake ? std::string() : FR.cli.my_uid();
    hubnet::dedupe_friends(FR.d_friends, me);
    hubnet::dedupe_requests(FR.d_friends, FR.d_in, FR.d_out, me);
    FR.d_stale.clear();
    FR.d_ask.clear();
    if (!FR.show_dupes) {
        std::set<std::string> retired;
        if (!FR.fake && FR.started) retired = FR.cli.retired_ids();
        std::vector<hubnet::StaleEntry> stale = hubnet::stale_same_name(FR.d_friends, retired);
        if (!stale.empty()) {
            std::vector<hubnet::Friend> keep;
            for (auto& f : FR.d_friends) {
                const hubnet::StaleEntry* e = nullptr;
                for (auto& s : stale) if (s.uid == f.uid) e = &s;
                if (e && e->confirmed) FR.d_stale.push_back(f);
                else {
                    if (e && !FR.kept_uids.count(f.uid)) FR.d_ask.push_back(f);
                    keep.push_back(f);
                }
            }
            FR.d_friends = std::move(keep);
        }
    }
}
const std::vector<hubnet::Friend>& fr_friends() { fr_refresh_lists(); return FR.d_friends; }
const std::vector<hubnet::Request>& fr_incoming() { fr_refresh_lists(); return FR.d_in; }
const std::vector<hubnet::Request>& fr_outgoing() { fr_refresh_lists(); return FR.d_out; }
const std::vector<hubnet::Message>& fr_conv(const std::string& uid) { return FR.fake ? FR.f_conv[uid] : FR.cli.conversation(uid); }
int fr_unread(const std::string& uid) {
    if (FR.fake) { auto it = FR.f_unread.find(uid); return it == FR.f_unread.end() ? 0 : it->second; }
    return FR.cli.unread(uid);
}
int fr_total_unread() {
    if (!FR.fake) return FR.cli.total_unread();
    int n = 0;
    for (auto& kv : FR.f_unread) n += kv.second;
    return n;
}
void fr_mark_read(const std::string& uid) { if (FR.fake) FR.f_unread[uid] = 0; else FR.cli.mark_read(uid); }
bool fr_connected() { return FR.fake || (FR.started && FR.cli.connected()); }
std::string fr_status_text() { return FR.fake ? "Online" : (FR.started ? FR.cli.status_text() : "Offline"); }
std::string fr_my_code() { return FR.fake ? "RH-7KQ2-M9XD" : (FR.started ? FR.cli.my_code() : ""); }
const hubnet::Friend* fr_find(const std::string& uid) {
    for (auto& f : fr_friends()) if (f.uid == uid) return &f;
    return nullptr;
}

/* ---------- small helpers ---------- */
hubui::FriendRow fr_row(const hubnet::Friend& f) {
    hubui::FriendRow r;
    r.uid = f.uid; r.name = f.name; r.game_title = f.status.game_title;
    switch (f.status.state) {
        case hubnet::PresenceState::Online: r.state = hubui::FS_Online; break;
        case hubnet::PresenceState::InLobby: r.state = hubui::FS_InLobby; break;
        case hubnet::PresenceState::InGame: r.state = hubui::FS_InGame; break;
        default: r.state = hubui::FS_Offline; break;
    }
    r.joinable = f.status.joinable && !f.status.lobby_code.empty();
    r.players = f.status.players; r.max_players = f.status.max_players;
    r.updated_ms = f.status.updated;
    r.in_my_room = fr_in_my_room() && !f.status.lobby_code.empty() && f.status.lobby_code == A.code;
    return r;
}
ImU32 fr_state_col(int st) {
    switch (st) {
        case hubui::FS_Online: return ui::GOOD;
        case hubui::FS_InLobby: return ui::WARN;
        case hubui::FS_InGame: return ui::ACCENT;
        default: return ui::TEXT3;
    }
}
ImU32 fr_avatar_col(const std::string& name) {
    static const ImU32 pal[] = {IM_COL32(108, 122, 255, 255), IM_COL32(232, 110, 150, 255), IM_COL32(60, 180, 160, 255),
                                IM_COL32(230, 150, 70, 255), IM_COL32(160, 110, 235, 255), IM_COL32(90, 160, 235, 255)};
    unsigned h = 7;
    for (unsigned char c : name) h = h * 31 + c;
    return pal[h % 6];
}
void fr_avatar(ImDrawList* dl, ImVec2 c, float r, const std::string& name, int state, ImU32 ring) {
    ImU32 col = state == hubui::FS_Offline ? ui::mix(fr_avatar_col(name), ui::PANEL2, 0.6f) : fr_avatar_col(name);
    dl->AddCircleFilled(c, r, col, 28);
    std::string in = name.empty() ? "?" : name.substr(0, 1);
    in[0] = (char)std::toupper((unsigned char)in[0]);
    ImFont* f = ui::F.bold;
    float fs = std::max(f->FontSize, r * 0.95f);
    ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0.f, in.c_str());
    dl->AddText(f, fs, c - ts * 0.5f, IM_COL32_WHITE, in.c_str());
    ImVec2 dc = c + ImVec2(r * 0.72f, r * 0.72f);
    dl->AddCircleFilled(dc, r * 0.34f, ring, 16);
    dl->AddCircleFilled(dc, r * 0.24f, fr_state_col(state), 16);
}
void fr_text_c(ImDrawList* dl, ImFont* f, ImVec2 c, ImU32 col, const char* s) {
    ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.f, s);
    dl->AddText(f, f->FontSize, c - ts * 0.5f, col, s);
}
float fr_tw(ImFont* f, const std::string& s) { return f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.f, s.c_str()).x; }

void fr_icon(ImDrawList* dl, FrIcon k, ImVec2 c, float s, ImU32 col) {
    float t = std::max(1.5f, s * 0.085f);
    switch (k) {
        case FrIcon::Bell: {
            float r = s * 0.30f;
            ImVec2 top = c + ImVec2(0, -s * 0.06f);
            dl->PathClear();
            dl->PathArcTo(top, r, IM_PI, IM_PI * 2.f, 16);
            dl->PathLineTo(c + ImVec2(r, s * 0.22f));
            dl->PathLineTo(c + ImVec2(r + s * 0.1f, s * 0.30f));
            dl->PathLineTo(c + ImVec2(-r - s * 0.1f, s * 0.30f));
            dl->PathLineTo(c + ImVec2(-r, s * 0.22f));
            dl->PathStroke(col, ImDrawFlags_Closed, t);
            dl->AddCircleFilled(c + ImVec2(0, s * 0.42f), s * 0.07f, col, 10);
            dl->AddLine(c + ImVec2(0, -s * 0.40f), c + ImVec2(0, -s * 0.34f), col, t);
            break;
        }
        case FrIcon::People: {
            ImVec2 a = c + ImVec2(-s * 0.12f, 0);
            dl->AddCircle(a + ImVec2(0, -s * 0.16f), s * 0.15f, col, 16, t);
            dl->PathClear();
            dl->PathArcTo(a + ImVec2(0, s * 0.36f), s * 0.28f, IM_PI * 1.08f, IM_PI * 1.92f, 12);
            dl->PathStroke(col, 0, t);
            ImVec2 b = c + ImVec2(s * 0.22f, 0);
            dl->AddCircle(b + ImVec2(0, -s * 0.10f), s * 0.11f, ui::scale_alpha(col, 0.6f), 14, t * 0.9f);
            dl->PathClear();
            dl->PathArcTo(b + ImVec2(0, s * 0.34f), s * 0.2f, IM_PI * 1.15f, IM_PI * 1.85f, 10);
            dl->PathStroke(ui::scale_alpha(col, 0.6f), 0, t * 0.9f);
            break;
        }
        case FrIcon::Chat: {
            ImVec2 p0 = c + ImVec2(-s * 0.36f, -s * 0.28f), p1 = c + ImVec2(s * 0.36f, s * 0.16f);
            dl->AddRect(p0, p1, col, s * 0.12f, 0, t);
            dl->AddLine(ImVec2(p0.x + s * 0.12f, p1.y), ImVec2(p0.x + s * 0.08f, p1.y + s * 0.2f), col, t);
            dl->AddLine(ImVec2(p0.x + s * 0.08f, p1.y + s * 0.2f), ImVec2(p0.x + s * 0.30f, p1.y), col, t);
            break;
        }
        case FrIcon::UserPlus: {
            ImVec2 a = c + ImVec2(-s * 0.1f, 0);
            dl->AddCircle(a + ImVec2(0, -s * 0.16f), s * 0.15f, col, 16, t);
            dl->PathClear();
            dl->PathArcTo(a + ImVec2(0, s * 0.36f), s * 0.28f, IM_PI * 1.08f, IM_PI * 1.92f, 12);
            dl->PathStroke(col, 0, t);
            ImVec2 p = c + ImVec2(s * 0.30f, -s * 0.12f);
            dl->AddLine(p + ImVec2(-s * 0.1f, 0), p + ImVec2(s * 0.1f, 0), col, t);
            dl->AddLine(p + ImVec2(0, -s * 0.1f), p + ImVec2(0, s * 0.1f), col, t);
            break;
        }
        case FrIcon::Copy: {
            dl->AddRect(c + ImVec2(-s * 0.26f, -s * 0.18f), c + ImVec2(s * 0.12f, s * 0.30f), col, s * 0.06f, 0, t);
            dl->AddRect(c + ImVec2(-s * 0.12f, -s * 0.32f), c + ImVec2(s * 0.26f, s * 0.16f), col, s * 0.06f, 0, t);
            break;
        }
        case FrIcon::More:
            for (int i = -1; i <= 1; ++i) dl->AddCircleFilled(c + ImVec2(i * s * 0.22f, 0), s * 0.065f, col, 10);
            break;
        case FrIcon::Send:
            dl->AddTriangleFilled(c + ImVec2(-s * 0.34f, -s * 0.30f), c + ImVec2(s * 0.38f, 0), c + ImVec2(-s * 0.34f, s * 0.30f), col);
            dl->AddTriangleFilled(c + ImVec2(-s * 0.34f, -s * 0.30f), c + ImVec2(-s * 0.10f, 0), c + ImVec2(-s * 0.34f, s * 0.30f), ui::scale_alpha(IM_COL32_BLACK, 0.25f));
            break;
    }
}

/* Square-ish icon button with optional badge. Returns pressed. */
bool fr_icon_button(const char* id, FrIcon k, float size, bool active, int badge, ImU32 badge_col, const char* tip, ImVec2* rect_max) {
    ImGui::PushID(id);
    bool pressed = ImGui::InvisibleButton("b", ImVec2(size, size), ImGuiButtonFlags_EnableNav);
    bool hov = ImGui::IsItemHovered(), focus = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
    float* an = ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID(), 0.f);
    ui::anim(*an, (hov || focus) ? 1.f : 0.f, 16.f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    float rd = sp(1.5f);
    dl->AddRectFilled(p0, p1, active ? with_alpha(ui::ACCENT, 0.28f) : ui::mix(ui::PANEL, ui::PANEL3, *an), rd);
    dl->AddRect(p0, p1, ui::HAIRLINE, rd, 0, 1.f);
    fr_icon(dl, k, (p0 + p1) * 0.5f, size * 0.52f, active ? ui::mix(ui::ACCENT, IM_COL32_WHITE, 0.55f) : ui::mix(ui::TEXT2, IM_COL32_WHITE, *an));
    if (badge > 0) {
        char b[8];
        std::snprintf(b, sizeof b, "%d", std::min(badge, 99));
        float bw = std::max(sp(2.f), fr_tw(ui::F.cap, b) + sp(1.f));
        ImVec2 bc(p1.x - sp(0.25f), p0.y + sp(0.25f));
        dl->AddRectFilled(ImVec2(bc.x - bw * 0.7f, bc.y - sp(1.f)), ImVec2(bc.x + bw * 0.3f, bc.y + sp(1.f)), badge_col, sp(1.f));
        fr_text_c(dl, ui::F.cap, ImVec2(bc.x - bw * 0.2f, bc.y), IM_COL32_WHITE, b);
    }
    if (focus) ui::focus_ring(dl, p0, p1, rd);
    if (hov && tip) ImGui::SetTooltip("%s", tip);
    if (rect_max) *rect_max = p1;
    ImGui::PopID();
    return pressed;
}
/* Small text pill button drawn at a given screen position (no layout). */
bool fr_pill_button(const char* id, const char* label, ImVec2 pos, float h, bool primary, bool enabled, float* width_out) {
    float w = fr_tw(ui::F.bold, label) + sp(2.5f);
    if (width_out) *width_out = w;
    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginDisabled(!enabled);
    bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
    bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    float rd = h * 0.5f;
    if (primary) dl->AddRectFilled(p0, p1, ui::mix(ui::ACCENT, IM_COL32_WHITE, hov ? 0.2f : 0.f), rd);
    else {
        dl->AddRectFilled(p0, p1, ui::mix(ui::PANEL3, IM_COL32_WHITE, hov ? 0.1f : 0.f), rd);
        dl->AddRect(p0, p1, ui::HAIRLINE, rd, 0, 1.f);
    }
    fr_text_c(dl, ui::F.bold, (p0 + p1) * 0.5f, enabled ? IM_COL32_WHITE : ui::TEXT3, label);
    ImGui::EndDisabled();
    return pressed;
}

/* Game thumbnail: library/catalog art when we have it, otherwise a coloured tile with initials. */
void fr_game_thumb(ImDrawList* dl, const std::string& game_id, const std::string& title, ImVec2 p0, ImVec2 p1, float rad) {
    ArtRef a;
    const hub::GameEntry* ge = nullptr;
    if (!game_id.empty()) { ge = find_entry(game_id); if (!ge) ge = find_room_entry(game_id); }
    if (ge) a = art_for_entry(*ge);
    else if (const GameInfo* g = game_id.empty() ? nullptr : hub::find_game(game_id)) a = art_for_pick(*g);
    if (a.tex) { ui::draw_art(dl, a.tex, a.w, a.h, p0, p1, rad); return; }
    ImU32 c0, c1;
    game_colors(game_id, &c0, &c1);
    gradient_round_rect(dl, p0, p1, c0, c1, rad);
    std::string in = initials(title);
    if (in.empty()) in = "?";
    fr_text_c(dl, ui::F.bold, (p0 + p1) * 0.5f, IM_COL32_WHITE, in.c_str());
}

std::string fr_time(long long ms) {
    time_t t = (time_t)(ms / 1000);
    tm lt{};
    localtime_s(&lt, &t);
    char b[16];
    std::snprintf(b, sizeof b, "%02d:%02d", lt.tm_hour, lt.tm_min);
    return b;
}
int fr_day(long long ms) {
    time_t t = (time_t)(ms / 1000);
    tm lt{};
    localtime_s(&lt, &t);
    return lt.tm_year * 400 + lt.tm_yday;
}
std::string fr_day_label(long long ms) {
    int d = fr_day(ms), today = fr_day(wall_ms());
    if (d == today) return "Today";
    if (d == today - 1) return "Yesterday";
    time_t t = (time_t)(ms / 1000);
    tm lt{};
    localtime_s(&lt, &t);
    char b[32];
    std::strftime(b, sizeof b, "%b %d", &lt);
    return b;
}

/* ---------- actions ---------- */
hubnet::Invite fr_make_invite() {
    hubnet::Invite inv;
    inv.lobby_code = A.code;
    inv.game_id = A.selected_game;
    if (const hub::GameEntry* g = find_room_entry(A.selected_game)) { inv.game_title = g->title; inv.system = sys_short(*g); }
    else if (const GameInfo* gi = hub::find_game(A.selected_game)) { inv.game_title = gi->title; inv.system = gi->system; }
    inv.expires_at = wall_ms() + 30LL * 60 * 1000;
    return inv;
}
bool fr_in_my_room() { return A.in_room && room_connected(); }
/* Test aid: HUB_SHOT_TRACE=<file> appends one line per friends-panel action, so the click harness can assert what a click did. */
static void fr_trace(const char* what, const std::string& arg = std::string()) {
    static const char* path = std::getenv("HUB_SHOT_TRACE");
    if (!path) return;
    if (FILE* f = std::fopen(path, "a")) { std::fprintf(f, "%s %s\n", what, arg.c_str()); std::fclose(f); }
}
void fr_invite_uids(const std::vector<std::string>& uids) {
    fr_trace("invite", uids.empty() ? "" : uids[0]);
    if (uids.empty() || !fr_in_my_room()) return;
    if (!FR.fake) FR.cli.send_invite(uids, fr_make_invite());
    double now = ImGui::GetTime();
    std::string names;
    for (auto& u : uids) {
        FR.invited_at[u] = now;
        if (const hubnet::Friend* f = fr_find(u)) { if (!names.empty()) names += ", "; names += f->name; }
    }
    if (FR.fake) set_toast("Invited " + names + " to room " + A.code + ".");   // live: the server's invite_result reports who was reached
}
void fr_join_lobby(const std::string& code, const std::string& game_id, const std::string& title, const std::string& who) {
    fr_trace("join", code);
    if (!hubnet::valid_room_code(code)) { set_notice("That lobby code is not valid any more."); return; }
    if (A.proc) {   // our game is running: remember the room and join when it closes
        if (hubui::join_while_running(A.in_room && A.code == code) == hubui::JoinWhileRunning::AlreadyInRoom) {
            FR.pending_join = {}; set_notice("You are already in that room."); return;
        }
        std::string msg = (!FR.pending_join.code.empty() && FR.pending_join.code != code)
            ? hubui::queued_join_replaced_text(FR.pending_join.name, who) : hubui::queued_join_text(who);
        FR.pending_join = {code, game_id, title, who};
        set_notice(msg, 20.0);
        set_toast(msg, true, 6.0);
        return;
    }
    FR.pending_join = {};
    if (A.in_room && A.code == code) { A.page = Page::Online; return; }
    bool owned = game_id.empty() || find_entry(game_id) || find_room_entry(game_id);
    enter_room(false, code);   // leaves any current room, switches to Play Online
    if (A.in_room && A.code == code && !owned)
        set_notice("You don't have " + (title.empty() ? std::string("that game") : title) + " yet - you can still join the room.", 12.0);
}
void fr_open_chat(const std::string& uid) {
    fr_trace("chat", uid);
    FR.chat_uid = uid;
    FR.chat_unread = fr_unread(uid);
    FR.chat_seen = 0;
    fr_mark_read(uid);
    if (!FR.fake && FR.hist_asked.insert(uid).second) FR.cli.request_history(uid);
    A.st.show_friends = true;
}
void fr_toggle_panel() {
    A.st.show_friends = !A.st.show_friends;
    if (A.st.show_friends && A.pad_mode) FR.want_focus = true;
    mark_dirty();
}

void fr_notify(bool invite, const std::string& uid, const std::string& name, const hubnet::Invite& inv) {
    for (auto& o : FR.notifs)
        if (!o.done && o.invite == invite && o.uid == uid && (!invite || o.inv.lobby_code == inv.lobby_code)) o.done = true;
    Notif n;
    n.id = FR.next_id++; n.invite = invite; n.uid = uid; n.name = name; n.inv = inv; n.t0 = ImGui::GetTime();
    FR.notifs.push_back(n);
    FR.notifs.erase(std::remove_if(FR.notifs.begin(), FR.notifs.end(), [](const Notif& x) { return x.done; }), FR.notifs.end());
    if (FR.notifs.size() > 30) FR.notifs.erase(FR.notifs.begin());
    if (invite && A.proc && !g_headless) hubtoast::notify_invite(hubtoast::system_notifier(), A.st.notify_in_game, true, name, inv.game_title, inv.lobby_code);
    if (invite && A.proc) flash_taskbar();   // a game is in front: blink the taskbar button, never take focus
}
int fr_pending_count() {
    int n = (int)fr_incoming().size();
    long long now = wall_ms();
    for (auto& x : FR.notifs) if (x.invite && !x.done && !hubui::invite_expired(x.inv.expires_at, now)) ++n;
    return n;
}
bool fr_panel_shown() { return A.st.show_friends && FR.panel_t > 0.5f; }

std::string fr_error_text(const std::string& r, const std::string& detail) { return hubui::friends_error_text(r, detail); }

/* Presence derived from the hub state. */
hubnet::Status fr_desired_status() {
    hubnet::Status s;
    if (A.st.appear_offline) { s.state = hubnet::PresenceState::Offline; return s; }
    s.state = hubnet::PresenceState::Online;
    auto fill_game = [&](const std::string& id) {
        const hub::GameEntry* g = find_entry(id);
        if (!g) g = find_room_entry(id);
        if (g) { s.game_id = room_id(*g); s.game_title = g->title; s.system = is_native(*g) ? "n64" : sys_short(*g); }
        else if (const GameInfo* gi = hub::find_game(id)) { s.game_id = gi->id; s.game_title = gi->title; s.system = gi->system; }
    };
    if (A.proc) {
        s.state = hubnet::PresenceState::InGame;
        fill_game(A.run.key);
        if (s.game_title.empty()) s.game_title = A.run.title;
        if (s.system.empty()) s.system = A.run.system_short;
        if (s.game_id == "remix" && !A.run_fighter.empty() && A.run.online) s.game_title += " as " + A.run_fighter;   // "Playing Smash Remix as Falco"
        if (A.in_room && room_connected()) {   // launched from a room: keep it visible so friends can join the next match
            s.lobby_code = A.code;
            s.players = (int)room_members().size();
            const hub::GameEntry* rg = find_room_entry(A.selected_game);
            s.max_players = rg ? max_players_for(*rg) : 4;
            s.joinable = hubui::lobby_joinable(s.players, s.max_players, false);
        }
        return s;
    }
    if (A.in_room && room_connected()) {
        s.state = hubnet::PresenceState::InLobby;
        s.lobby_code = A.code;
        if (!A.selected_game.empty()) fill_game(A.selected_game);
        const hub::GameEntry* rg = find_room_entry(A.selected_game);
        s.players = (int)room_members().size();
        s.max_players = rg ? max_players_for(*rg) : 4;
        s.joinable = hubui::lobby_joinable(s.players, s.max_players, false);
    }
    return s;
}
void fr_update_presence() {
    hubnet::Status s = fr_desired_status();
    const hubnet::Status& o = FR.last_status;
    bool same = FR.status_sent && s.state == o.state && s.game_id == o.game_id && s.game_title == o.game_title && s.system == o.system &&
                s.lobby_code == o.lobby_code && s.joinable == o.joinable && s.players == o.players && s.max_players == o.max_players;
    if (same) return;
    FR.last_status = s;
    FR.status_sent = true;
    FR.cli.set_status(s);
}

void fr_set_auto_friend(bool on) { if (!FR.fake) FR.cli.set_auto_friend(on); }
std::string fr_lobby_ident_msg() {
    if (FR.fake || !FR.started || FR.cli.my_uid().empty() || FR.cli.my_secret().empty()) return "";
    return hubui::lobby_ident_json(FR.cli.my_uid(), FR.cli.my_secret());
}

void fr_start() {
    if (g_headless || FR.fake) return;
    FR.cli.set_paths(hub::data_dir(), hub::saves::vault_dir());
    FR.cli.set_auto_friend(A.st.auto_friend);
    FR.started = FR.cli.start("", A.st.name);
    FR.last_name = A.st.name;
}

void fr_run_pending_join() {
    if (!hubui::pending_join_ready(!FR.pending_join.code.empty(), A.proc != nullptr)) return;
    auto pj = FR.pending_join;
    FR.pending_join = {};
    fr_join_lobby(pj.code, pj.game_id, pj.title, pj.name);
}

/* After a save sync/import: a vault identity older than ours is the user's original account; adopt it. */
void fr_adopt_identity_after_vault_change() {
    if (g_headless || FR.fake) return;
    if (FR.started) { FR.cli.adopt_vault_identity_if_older(); return; }   // toast comes from the IdentityAdopted event
    hubnet::Identity id;
    if (hubnet::identity_adopt_vault_if_older(hub::data_dir(), hub::saves::vault_dir(), &id))
        set_toast("Signed in as " + hubnet::friend_code(id.user_id) + " (your account from your other PC)");
}

void fr_tick() {
    fr_run_pending_join();
    if (!FR.started || FR.fake) return;
    FR.cli.update();
    hubnet::Event e;
    while (FR.cli.poll_event(&e)) {
        switch (e.kind) {
            case hubnet::Event::RequestReceived: fr_notify(false, e.uid, e.name, hubnet::Invite()); break;
            case hubnet::Event::RequestResolved:
                if (e.text == "accepted") set_toast((e.name.empty() ? std::string("Your request") : e.name) + " accepted your friend request.");
                break;
            case hubnet::Event::IdentityAdopted:
                set_toast("Signed in as " + e.text + " (your account from your other PC)");
                break;
            case hubnet::Event::FriendAdded:
                set_toast(e.automatic ? hubui::auto_friend_text(e.name) : e.name + " is now your friend.");
                break;
            case hubnet::Event::MessageReceived:
                if (fr_panel_shown() && FR.chat_uid == e.uid) fr_mark_read(e.uid);
                else push_toast(e.name + ": " + (e.text.size() > 80 ? e.text.substr(0, 77) + "..." : e.text), ui::ACCENT, 5.0);
                break;
            case hubnet::Event::InviteReceived:
                if (!hubui::invite_expired(e.invite.expires_at, wall_ms())) fr_notify(true, e.uid, e.name, e.invite);
                break;
            case hubnet::Event::Error:
                if (e.name == "invite") break;   // reported per target by InviteResult
                { std::string t = fr_error_text(e.text, e.detail); if (!t.empty()) push_toast(t, ui::WARN, 6.0); }
                break;
            case hubnet::Event::InviteResult: {
                std::vector<std::string> okn, badn;
                for (auto& u : e.ok_uids) if (const hubnet::Friend* f = fr_find(u)) okn.push_back(f->name);
                for (auto& u : e.failed_uids) { FR.invited_at.erase(u); if (const hubnet::Friend* f = fr_find(u)) badn.push_back(f->name); }
                std::string t = hubui::invite_result_text(okn, badn, e.failed_reasons.empty() ? std::string() : e.failed_reasons[0], A.code);
                if (!t.empty()) set_toast(t);
                break;
            }
            default: break;
        }
    }
    fr_update_presence();
    if (A.st.name != FR.last_name) {   // debounce: rename once typing stops
        if (FR.name_changed == 0) FR.name_changed = ImGui::GetTime();
        if (ImGui::GetTime() - FR.name_changed > 1.0 && !A.st.name.empty()) {
            FR.cli.rename(A.st.name);
            FR.last_name = A.st.name;
            FR.name_changed = 0;
        }
    } else FR.name_changed = 0;
}

/* ---------- fake data for screenshots ---------- */
void fr_fake_init() {
    FR.fake = true;
    long long now = wall_ms();
    auto mk = [&](const char* uid, const char* name, hubnet::PresenceState st, const char* gid, const char* gt, const char* sys, const char* lobby,
                  bool joinable, int pl, int mx, long long upd) {
        hubnet::Friend f;
        f.uid = uid; f.name = name; f.code = std::string("RH-") + "AB2C-D" + uid[0] + "FG";
        f.status.state = st; f.status.game_id = gid; f.status.game_title = gt; f.status.system = sys; f.status.lobby_code = lobby;
        f.status.joinable = joinable; f.status.players = pl; f.status.max_players = mx; f.status.updated = upd;
        FR.f_friends.push_back(f);
    };
    using PS = hubnet::PresenceState;
    mk("miauid", "Mia", PS::InLobby, "mk64", "Mario Kart 64", "n64", "K7M2QX", true, 2, 4, now);
    mk("jakuid", "Jake", PS::InGame, "smash64", "Super Smash Bros.", "n64", "QW8R2T", true, 2, 4, now);
    mk("kaiuid", "Kai", PS::InGame, "mk64", "Mario Kart 64", "n64", "", false, 0, 0, now);
    mk("lunuid", "Luna", PS::Online, "", "", "", "", false, 0, 0, now);
    mk("devuid", "Dev", PS::InLobby, "mp2", "Mario Party 2", "n64", "ABC234", false, 4, 4, now);
    mk("samuid", "Sam", PS::Offline, "", "", "", "", false, 0, 0, now - 3 * 3600 * 1000LL);
    mk("priuid", "Priya", PS::Offline, "", "", "", "", false, 0, 0, now - 2 * 86400 * 1000LL);
    hubnet::Request r;
    r.uid = "oliuid"; r.name = "Oliver"; r.code = "RH-QW3E-RT4Y"; r.at = now;
    FR.f_in.push_back(r);
    hubnet::Request o;
    o.uid = "rinuid"; o.name = "Rin"; o.code = "RH-ZX5C-VB6N"; o.at = now;
    FR.f_out.push_back(o);
    if (std::getenv("HUB_SHOT_FAKE_DUPES")) {   // screenshot aid: the same people repeated in every way; the panel must still show each once
        mk("oldkai", "Kai", PS::Offline, "", "", "", "", false, 0, 0, now - 20 * 86400 * 1000LL);   // Kai's old identity (never retired), same name
        mk("miauid", "Mia", PS::Offline, "", "", "", "", false, 0, 0, now - 86400 * 1000LL);   // second row, same uid
        mk("jakuid", "Jake", PS::Online, "", "", "", "", false, 0, 0, now - 5000);
        mk("oliuid", "Oliver", PS::Online, "", "", "", "", false, 0, 0, now);                  // friend AND incoming request below
        hubnet::Request d1 = FR.f_in[0];
        FR.f_in.push_back(d1);                                                                 // duplicate incoming
        hubnet::Request d2 = FR.f_out[0]; d2.uid = "oliuid"; d2.name = "Oliver";
        FR.f_out.push_back(d2);                                                                // outgoing to a friend
        hubnet::Request d3 = FR.f_in[0]; d3.uid = "rinuid"; d3.name = "Rin";
        FR.f_in.push_back(d3);                                                                 // Rin incoming AND outgoing
    }
    auto msg = [&](const char* from, bool mine, const char* text, long long at) {
        hubnet::Message m;
        m.id = std::to_string(at); m.from = mine ? "me" : from; m.to = mine ? from : "me"; m.mine = mine; m.text = text; m.at = at;
        FR.f_conv["miauid"].push_back(m);
    };
    msg("miauid", false, "yo! wanna race tonight?", now - 26 * 3600 * 1000LL);
    msg("miauid", true, "sure, give me 10 min", now - 26 * 3600 * 1000LL + 60000);
    msg("miauid", false, "I just opened a room, hop in when you're ready", now - 4 * 60000);
    hubnet::Message im;
    im.id = "inv1"; im.from = "miauid"; im.to = "me"; im.invite = true; im.at = now - 3 * 60000;
    im.inv.lobby_code = "K7M2QX"; im.inv.game_id = "mk64"; im.inv.game_title = "Mario Kart 64"; im.inv.system = "n64"; im.inv.expires_at = now + 27 * 60000;
    FR.f_conv["miauid"].push_back(im);
    msg("miauid", false, "bring the good controller", now - 60000);
    FR.f_unread["miauid"] = 2;
    FR.f_unread["jakuid"] = 0;
    hubnet::Invite inv = im.inv;
    fr_notify(true, "miauid", "Mia", inv);
}

/* ============================ drawing ============================ */

float fr_panel_w(float disp_w) { return std::min(sp(37.5f), disp_w * 0.92f); }
void fr_anim_panel() {
    float target = A.st.show_friends ? 1.f : 0.f;
    if (FR.panel_t < 0) FR.panel_t = target;
    ui::anim(FR.panel_t, target, 16.f);
    if (std::fabs(FR.panel_t - target) < 0.002f) FR.panel_t = target;
}

/* ---- invite / request cards (toast + bell list) ---- */
float fr_invite_card_h() { return sp(1.75f) * 2 + sp(8.f) + sp(1.f) + sp(3.5f); }
/* Returns 1 = Join, 2 = Dismiss. frac >= 0 draws the remaining-time bar. */
int fr_invite_card(const Notif& n, float w, float frac, bool shadow) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float pad = sp(1.75f), h = fr_invite_card_h();
    ImVec2 p1(p0.x + w, p0.y + h);
    long long now = wall_ms();
    bool expired = hubui::invite_expired(n.inv.expires_at, now);
    if (shadow) ui::shadow(dl, p0, p1, sp(2.f), 1.f, sp(0.5f));
    dl->AddRectFilled(p0, p1, ui::PANEL2, sp(2.f));
    dl->AddRect(p0, p1, with_alpha(ui::ACCENT, 0.45f), sp(2.f), 0, 1.f);
    ImVec2 t0 = p0 + ImVec2(pad, pad), t1 = t0 + ImVec2(sp(8.f), sp(8.f));
    fr_game_thumb(dl, n.inv.game_id, n.inv.game_title, t0, t1, sp(1.f));
    float tx = t1.x + sp(1.5f), tw = p1.x - pad - tx;
    std::string game = n.inv.game_title.empty() ? "a game" : n.inv.game_title;
    auto lines = ui::wrap_lines(ui::F.bold, ui::F.bold->FontSize, n.name + " invited you to play " + game, tw, 2);
    float y = t0.y;
    for (auto& l : lines) { dl->AddText(ui::F.bold, ui::F.bold->FontSize, ImVec2(tx, y), ui::TEXT, l.c_str()); y += ui::F.bold->FontSize * 1.2f; }
    std::string sub = "Room " + n.inv.lobby_code;
    int left = hubui::invite_seconds_left(n.inv.expires_at, now);
    sub += expired ? "  -  expired" : "  -  " + (left >= 120 ? std::to_string(left / 60) + " min left" : std::to_string(left) + "s left");
    dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(tx, y + sp(0.25f)), expired ? ui::BAD : ui::TEXT2, ui::ellipsize(ui::F.cap, ui::F.cap->FontSize, sub, tw).c_str());
    bool owned = n.inv.game_id.empty() || find_entry(n.inv.game_id) || find_room_entry(n.inv.game_id);
    if (!owned) dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(tx, y + sp(0.25f) + ui::F.cap->FontSize * 1.25f), ui::WARN,
                            ui::ellipsize(ui::F.cap, ui::F.cap->FontSize, "You don't have " + game + " yet", tw).c_str());
    int act = 0;
    float by = p1.y - pad - sp(3.5f), bw = (w - pad * 2 - sp(1.f)) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, by));
    if (ui::button("Join", ImVec2(bw, sp(3.5f)), true, !expired)) act = 1;
    ImGui::SetCursorScreenPos(ImVec2(p0.x + pad + bw + sp(1.f), by));
    if (ui::button("Dismiss", ImVec2(bw, sp(3.5f)), false, true)) act = 2;
    if (frac >= 0.f) {
        float bw2 = (w - sp(4.f)) * std::max(0.f, std::min(1.f, frac));
        dl->AddRectFilled(ImVec2(p0.x + sp(2.f), p1.y - sp(0.4f)), ImVec2(p0.x + sp(2.f) + bw2, p1.y), with_alpha(ui::ACCENT, 0.9f), sp(0.2f));
    }
    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(w, h));
    return act;
}
float fr_request_card_h() { return sp(1.75f) * 2 + sp(4.f) + sp(1.f) + sp(3.5f); }
/* 1 = Accept, 2 = Decline/Dismiss */
int fr_request_card(const std::string& name, const std::string& code, float w, float frac, bool shadow, const char* decline) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float pad = sp(1.75f), h = fr_request_card_h();
    ImVec2 p1(p0.x + w, p0.y + h);
    if (shadow) ui::shadow(dl, p0, p1, sp(2.f), 1.f, sp(0.5f));
    dl->AddRectFilled(p0, p1, ui::PANEL2, sp(2.f));
    dl->AddRect(p0, p1, with_alpha(ui::GOOD, 0.45f), sp(2.f), 0, 1.f);
    fr_avatar(dl, ImVec2(p0.x + pad + sp(2.f), p0.y + pad + sp(2.f)), sp(2.f), name, hubui::FS_Online, ui::PANEL2);
    float tx = p0.x + pad + sp(5.f);
    dl->AddText(ui::F.bold, ui::F.bold->FontSize, ImVec2(tx, p0.y + pad), ui::TEXT, ui::ellipsize(ui::F.bold, ui::F.bold->FontSize, name + " wants to be friends", p1.x - pad - tx).c_str());
    dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(tx, p0.y + pad + ui::F.bold->FontSize * 1.3f), ui::TEXT2, code.c_str());
    int act = 0;
    float by = p1.y - pad - sp(3.5f), bw = (w - pad * 2 - sp(1.f)) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, by));
    if (ui::button("Accept", ImVec2(bw, sp(3.5f)), true)) act = 1;
    ImGui::SetCursorScreenPos(ImVec2(p0.x + pad + bw + sp(1.f), by));
    if (ui::button(decline, ImVec2(bw, sp(3.5f)))) act = 2;
    if (frac >= 0.f) {
        float bw2 = (w - sp(4.f)) * std::max(0.f, std::min(1.f, frac));
        dl->AddRectFilled(ImVec2(p0.x + sp(2.f), p1.y - sp(0.4f)), ImVec2(p0.x + sp(2.f) + bw2, p1.y), with_alpha(ui::GOOD, 0.9f), sp(0.2f));
    }
    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(w, h));
    return act;
}
void fr_accept(const std::string& uid, const std::string& name) {
    fr_trace("accept", uid);
    if (FR.fake) {
        for (size_t i = 0; i < FR.f_in.size(); ++i) if (FR.f_in[i].uid == uid) { FR.f_in.erase(FR.f_in.begin() + i); break; }
    } else FR.cli.accept(uid);
    set_toast("You and " + name + " are now friends.");
}
void fr_join_notif(Notif& n) {
    fr_join_lobby(n.inv.lobby_code, n.inv.game_id, n.inv.game_title, n.name);
    n.done = true;
}

/* Toast cards (top right, below the top bar). */
/* Friend request / invite cards stack upward from `bottom_y` (bottom right, clear of the page header buttons such as "Invite friends"). */
void fr_draw_toasts(float right_x, float bottom_y) {
    double now = ImGui::GetTime();
    float y = bottom_y, w = std::min(sp(45.f), right_x - sp(2.f));
    for (auto& n : FR.notifs) {
        if (n.done || !n.toast) continue;
        if (FR.fake && g_headless) n.t0 = std::max(n.t0, now - 1.0);   // screenshots: the toast must outlive a slow library scan
        double life = n.invite ? 30.0 : 20.0, age = now - n.t0;
        if (age > life) { n.toast = false; continue; }
        float in = (float)std::min(1.0, age / 0.3);
        in = 1.f - (1.f - in) * (1.f - in);
        float h = n.invite ? fr_invite_card_h() : fr_request_card_h();
        ImGui::SetNextWindowPos(ImVec2(right_x - w + (1.f - in) * sp(8.f), y - h));
        ImGui::SetNextWindowSize(ImVec2(w, h));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, 0);
        std::string wid = "##toast" + std::to_string(n.id);
        if (ImGui::Begin(wid.c_str(), nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing)) {
            float frac = (float)(1.0 - age / life);
            if (n.invite) {
                int a = fr_invite_card(n, w, frac, true);
                if (a == 1) fr_join_notif(n);
                if (a == 2) n.toast = false;
            } else {
                int a = fr_request_card(n.name, "wants to be your friend", w, frac, true, "Dismiss");
                if (a == 1) { fr_accept(n.uid, n.name); n.done = true; }
                if (a == 2) n.toast = false;
            }
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        y -= h + sp(1.f);
    }
}

/* ---- top bar (inside the content column) ---- */
void fr_top_bar() {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = sp(4.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // presence pill (left)
    std::string txt;
    ImU32 dot;
    if (A.st.appear_offline) { txt = "Appearing offline"; dot = ui::TEXT3; }
    else if (!fr_connected()) { txt = FR.started ? fr_status_text() : "Friends offline"; dot = ui::WARN; }
    else {
        hubnet::Status s = fr_desired_status();
        if (s.state == hubnet::PresenceState::InGame) { txt = "Playing " + s.game_title; dot = ui::ACCENT; }
        else if (s.state == hubnet::PresenceState::InLobby) { txt = "In lobby " + A.code; dot = ui::WARN; }
        else { txt = "Online"; dot = ui::GOOD; }
        int on = 0;
        for (auto& f : fr_friends()) if (f.online()) ++on;
        txt += "  -  " + std::to_string(on) + (on == 1 ? " friend online" : " friends online");
    }
    txt = ui::ellipsize(ui::F.cap, ui::F.cap->FontSize, txt, std::max(sp(10.f), w - sp(12.f)));
    float pw = fr_tw(ui::F.cap, txt) + sp(4.f);
    ImVec2 q0(p.x, p.y + sp(0.5f)), q1(p.x + pw, p.y + h - sp(0.5f));
    dl->AddRectFilled(q0, q1, ui::PANEL, sp(1.75f));
    dl->AddRect(q0, q1, ui::HAIRLINE, sp(1.75f), 0, 1.f);
    dl->AddCircleFilled(ImVec2(q0.x + sp(1.75f), (q0.y + q1.y) * 0.5f), sp(0.5f), dot, 12);
    dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(q0.x + sp(3.f), (q0.y + q1.y - ui::F.cap->FontSize) * 0.5f), ui::TEXT2, txt.c_str());

    // buttons (right)
    float bs = h - sp(0.5f);
    int online = 0;
    for (auto& f : fr_friends()) if (f.online()) ++online;
    int unread = fr_total_unread();
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - bs, p.y + sp(0.25f)));
    if (fr_icon_button("friends", FrIcon::People, bs, A.st.show_friends, unread > 0 ? unread : online, unread > 0 ? ui::BAD : ui::GOOD,
                       A.st.show_friends ? "Hide friends (F2)" : "Show friends (F2)"))
        fr_toggle_panel();
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - bs * 2 - sp(0.75f), p.y + sp(0.25f)));
    ImVec2 bell_max;
    int pend = fr_pending_count();
    if (fr_icon_button("bell", FrIcon::Bell, bs, false, pend, ui::ACCENT, "Notifications", &bell_max) || FR.shot_bell) {
        ImGui::OpenPopup("##notifs");
        FR.shot_bell = false;
    }
    ImGui::SetNextWindowPos(ImVec2(bell_max.x, bell_max.y + sp(0.75f)), ImGuiCond_Always, ImVec2(1.f, 0.f));
    float pwid = std::min(sp(46.f), ImGui::GetIO().DisplaySize.x - sp(4.f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(pwid, sp(10.f)), ImVec2(pwid, ImGui::GetIO().DisplaySize.y * 0.7f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(1.5f), sp(1.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ui::PANEL);
    if (ImGui::BeginPopup("##notifs")) {
        float cw = ImGui::GetContentRegionAvail().x;
        ImGui::PushFont(ui::F.bold);
        ImGui::TextUnformatted("Notifications");
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        long long now = wall_ms();
        bool any = false;
        for (auto& n : FR.notifs) {
            if (!n.invite || n.done) continue;
            if (hubui::invite_expired(n.inv.expires_at, now) && now - n.inv.expires_at > 5 * 60 * 1000) { n.done = true; continue; }
            any = true;
            ImGui::PushID(n.id);
            int a = fr_invite_card(n, cw, -1.f, false);
            if (a == 1) { fr_join_notif(n); ImGui::CloseCurrentPopup(); }
            if (a == 2) n.done = true;
            ImGui::PopID();
            ImGui::Dummy(ImVec2(0, sp(0.75f)));
        }
        std::vector<hubnet::Request> in = fr_incoming();
        for (auto& r : in) {
            any = true;
            ImGui::PushID(r.uid.c_str());
            int a = fr_request_card(r.name, r.code, cw, -1.f, false, "Decline");
            if (a == 1) fr_accept(r.uid, r.name);
            if (a == 2) { if (!FR.fake) FR.cli.decline(r.uid); else FR.f_in.clear(); }
            ImGui::PopID();
            ImGui::Dummy(ImVec2(0, sp(0.75f)));
        }
        if (!any) {
            ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
            ImGui::TextUnformatted("Nothing new. Invites and friend requests show up here.");
            ImGui::PopStyleColor();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(w, h + sp(0.5f)));
}

/* ---- friend row ---- */
void fr_confirm(const char* kind, const hubnet::Friend& f) {
    fr_trace(kind, f.uid);
    FR.confirm_kind = kind; FR.confirm_uid = f.uid; FR.confirm_name = f.name; FR.confirm_open = true;
}
void fr_friend_row(const hubnet::Friend& f, const hubui::FriendRow& fr, float w, long long now) {
    float h = sp(7.f);
    ImGui::PushID(f.uid.c_str());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + w, p0.y + h);
    ImGui::SetNextItemAllowOverlap();
    bool clicked = ImGui::InvisibleButton("row", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    bool focus = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
    bool hov = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseHoveringRect(p0, p1);
    bool can_join = hubui::friend_can_join(fr);
    const char* join_lbl = fr.state == hubui::FS_InGame ? "Join lobby" : "Join";
    bool can_invite = fr_in_my_room() && f.online() && f.status.lobby_code != A.code;
    auto inv_it = FR.invited_at.find(f.uid);
    bool invited_recent = inv_it != FR.invited_at.end() && ImGui::GetTime() - inv_it->second < 20.0;
    static const char* shot_hover = std::getenv("HUB_SHOT_HOVER");   // screenshot aid: pretend this friend uid is hovered
    bool show = hov || focus || (shot_hover && f.uid == shot_hover);
    int unread = fr_unread(f.uid);
    if (show) dl->AddRectFilled(p0, p1, ui::PANEL2, sp(1.5f));
    ImVec2 ac(p0.x + sp(3.f), (p0.y + p1.y) * 0.5f);
    fr_avatar(dl, ac, sp(2.25f), f.name, fr.state, show ? ui::PANEL2 : ui::PANEL);
    if (unread > 0) {
        char b[8];
        std::snprintf(b, sizeof b, "%d", std::min(unread, 99));
        ImVec2 bc = ac + ImVec2(sp(1.9f), -sp(1.9f));
        dl->AddCircleFilled(bc, sp(1.f), ui::BAD, 14);
        fr_text_c(dl, ui::F.cap, bc, IM_COL32_WHITE, b);
    }
    // right side: buttons while hovered/focused, game thumbnail otherwise
    float right = p1.x - sp(1.f);
    bool has_game = (fr.state == hubui::FS_InGame || fr.state == hubui::FS_InLobby) && (!f.status.game_id.empty() || !f.status.game_title.empty());
    float btn_h = sp(3.25f), by = ac.y - btn_h * 0.5f;
    if (show) {
        float x = right;
        ImVec2 mp(x - sp(3.25f), by);
        ImGui::SetCursorScreenPos(mp);
        if (ImGui::InvisibleButton("more", ImVec2(sp(3.25f), btn_h))) ImGui::OpenPopup("ctx");
        bool mh = ImGui::IsItemHovered();
        dl->AddRectFilled(mp, mp + ImVec2(sp(3.25f), btn_h), ui::mix(ui::PANEL3, IM_COL32_WHITE, mh ? 0.1f : 0.f), btn_h * 0.5f);
        fr_icon(dl, FrIcon::More, mp + ImVec2(sp(1.625f), btn_h * 0.5f), sp(2.6f), ui::TEXT);
        if (mh) ImGui::SetTooltip("More");
        x = mp.x - sp(0.5f);
        ImVec2 cp(x - sp(3.25f), by);
        ImGui::SetCursorScreenPos(cp);
        if (ImGui::InvisibleButton("msg", ImVec2(sp(3.25f), btn_h))) fr_open_chat(f.uid);
        bool ch = ImGui::IsItemHovered();
        dl->AddRectFilled(cp, cp + ImVec2(sp(3.25f), btn_h), ui::mix(ui::PANEL3, IM_COL32_WHITE, ch ? 0.1f : 0.f), btn_h * 0.5f);
        fr_icon(dl, FrIcon::Chat, cp + ImVec2(sp(1.625f), btn_h * 0.5f), sp(2.4f), ui::TEXT);
        if (ch) ImGui::SetTooltip("Message");
        x = cp.x - sp(0.5f);
        float pw = 0;
        if (can_invite) {
            const char* il = invited_recent ? "Invited" : "Invite";
            pw = fr_tw(ui::F.bold, il) + sp(2.5f);
            if (fr_pill_button("invite", il, ImVec2(x - pw, by), btn_h, false, !invited_recent)) fr_invite_uids({f.uid});
            x -= pw + sp(0.5f);
        }
        if (can_join) {
            if (fr_pill_button("join", join_lbl, ImVec2(x - fr_tw(ui::F.bold, join_lbl) - sp(2.5f), by), btn_h, true, true, &pw)) fr_join_lobby(f.status.lobby_code, f.status.game_id, f.status.game_title, f.name);
            x -= pw + sp(0.5f);
        }
        right = x;
    } else if (has_game) {
        ImVec2 t1(right, ac.y + sp(2.5f)), t0(right - sp(5.f), ac.y - sp(2.5f));
        fr_game_thumb(dl, f.status.game_id, f.status.game_title, t0, t1, sp(0.75f));
        right = t0.x - sp(1.f);
    }
    float tx = p0.x + sp(6.5f), tw = std::max(sp(6.f), right - tx);
    dl->AddText(ui::F.bold, ui::F.bold->FontSize, ImVec2(tx, p0.y + sp(1.5f)), fr.state == hubui::FS_Offline ? ui::TEXT2 : ui::TEXT,
                ui::ellipsize(ui::F.bold, ui::F.bold->FontSize, f.name, tw).c_str());
    dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(tx, p0.y + sp(1.5f) + ui::F.bold->FontSize * 1.25f),
                fr.state == hubui::FS_Offline ? ui::TEXT3 : ui::TEXT2,
                ui::ellipsize(ui::F.cap, ui::F.cap->FontSize, hubui::friend_status_line(fr, now), tw).c_str());
    if (focus) ui::focus_ring(dl, p0, p1, sp(1.5f));

    if (clicked) ImGui::OpenPopup("ctx");   // left-click on the row opens the actions menu (Message is its first entry); right-click does the same
    if (ImGui::IsMouseHoveringRect(p0, p1) && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("ctx");
    if (focus) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false)) {
            if (can_join) fr_join_lobby(f.status.lobby_code, f.status.game_id, f.status.game_title, f.name);
            else if (can_invite) fr_invite_uids({f.uid});
        }
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false)) ImGui::OpenPopup("ctx");
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(1.f), sp(1.f)));
    if (ImGui::BeginPopup("ctx")) {
        if (ImGui::MenuItem("Message")) fr_open_chat(f.uid);
        if (can_join && ImGui::MenuItem("Join lobby")) fr_join_lobby(f.status.lobby_code, f.status.game_id, f.status.game_title, f.name);
        if (can_invite && ImGui::MenuItem("Invite to my room")) fr_invite_uids({f.uid});
        if (ImGui::MenuItem("Copy friend code")) { SDL_SetClipboardText(f.code.c_str()); fr_trace("copy", f.uid); set_toast("Friend code copied.", true, 3.0); }
        ImGui::Separator();
        if (ImGui::MenuItem("Remove friend")) fr_confirm("remove", f);
        if (ImGui::MenuItem("Block")) fr_confirm("block", f);
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(w, h + sp(0.25f)));
    ImGui::PopID();
}

void fr_section(const std::string& title) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(p.x + sp(0.5f), p.y + sp(0.5f)), ui::TEXT3, title.c_str());
    ImGui::Dummy(ImVec2(1, ui::F.cap->FontSize + sp(1.25f)));
}

/* ---- chat ---- */
void fr_chat_view(float cw) {
    const hubnet::Friend* f = fr_find(FR.chat_uid);
    if (!f) { FR.chat_uid.clear(); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    hubui::FriendRow fr = fr_row(*f);
    long long now = wall_ms();
    {   // header
        ImVec2 p = ImGui::GetCursorScreenPos();
        float hh = sp(6.f);
        if (ui::icon_button("chat_back", ui::Icon::ChevronL, sp(4.f), false, "Back to friends")) FR.chat_uid.clear();
        fr_avatar(dl, ImVec2(p.x + sp(7.f), p.y + hh * 0.5f), sp(2.f), f->name, fr.state, ui::PANEL);
        float tx = p.x + sp(10.f), tw = cw - sp(10.f);
        dl->AddText(ui::F.bold, ui::F.bold->FontSize, ImVec2(tx, p.y + sp(0.75f)), ui::TEXT, ui::ellipsize(ui::F.bold, ui::F.bold->FontSize, f->name, tw).c_str());
        dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(tx, p.y + sp(0.75f) + ui::F.bold->FontSize * 1.25f), ui::TEXT2,
                    ui::ellipsize(ui::F.cap, ui::F.cap->FontSize, hubui::friend_status_line(fr, now), tw).c_str());
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + hh));
        ImGui::Dummy(ImVec2(1, sp(0.5f)));
    }
    float input_h = ImGui::GetFrameHeight() + sp(1.f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::BG);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, sp(1.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(1.f), sp(1.f)));
    ImGui::BeginChild("##msgs", ImVec2(0, -input_h), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened);
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    const auto& msgs = fr_conv(FR.chat_uid);
    float aw = ImGui::GetContentRegionAvail().x;
    size_t first_unread = msgs.size() - std::min<size_t>(msgs.size(), (size_t)FR.chat_unread);
    int last_day = -1;
    ImFont* bf = ui::F.body;
    float fs = bf->FontSize, pad = sp(1.25f);
    if (msgs.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
        ImGui::TextWrapped("No messages yet. Say hi!");
        ImGui::PopStyleColor();
    }
    for (size_t i = 0; i < msgs.size(); ++i) {
        const hubnet::Message& m = msgs[i];
        ImVec2 c = ImGui::GetCursorScreenPos();
        int day = fr_day(m.at);
        if (day != last_day) {
            last_day = day;
            fr_text_c(cdl, ui::F.cap, ImVec2(c.x + aw * 0.5f, c.y + sp(1.f)), ui::TEXT3, fr_day_label(m.at).c_str());
            ImGui::Dummy(ImVec2(aw, sp(2.f)));
            c = ImGui::GetCursorScreenPos();
        }
        if (!m.mine && i == first_unread && FR.chat_unread > 0) {
            float ly = c.y + sp(1.f);
            std::string lab = "New messages";
            float lw = fr_tw(ui::F.cap, lab) + sp(2.f);
            cdl->AddLine(ImVec2(c.x, ly), ImVec2(c.x + aw * 0.5f - lw * 0.5f, ly), with_alpha(ui::BAD, 0.6f), 1.f);
            cdl->AddLine(ImVec2(c.x + aw * 0.5f + lw * 0.5f, ly), ImVec2(c.x + aw, ly), with_alpha(ui::BAD, 0.6f), 1.f);
            fr_text_c(cdl, ui::F.cap, ImVec2(c.x + aw * 0.5f, ly), ui::BAD, lab.c_str());
            ImGui::Dummy(ImVec2(aw, sp(2.f)));
            c = ImGui::GetCursorScreenPos();
        }
        std::string tm = fr_time(m.at);
        if (m.invite) {
            float cwid = aw * 0.94f, ch = pad * 2 + sp(7.f) + (m.mine ? 0.f : sp(1.f) + sp(3.f));
            float x0 = m.mine ? c.x + aw - cwid : c.x;
            ImVec2 q0(x0, c.y), q1(x0 + cwid, c.y + ch);
            bool expired = hubui::invite_expired(m.inv.expires_at, now);
            cdl->AddRectFilled(q0, q1, ui::PANEL2, sp(1.75f));
            cdl->AddRect(q0, q1, with_alpha(ui::ACCENT, 0.4f), sp(1.75f), 0, 1.f);
            ImVec2 t0 = q0 + ImVec2(pad, pad);
            fr_game_thumb(cdl, m.inv.game_id, m.inv.game_title, t0, t0 + ImVec2(sp(7.f), sp(7.f)), sp(0.75f));
            float tx = t0.x + sp(8.5f), tw2 = q1.x - pad - tx;
            std::string gt = m.inv.game_title.empty() ? "a game" : m.inv.game_title;
            cdl->AddText(ui::F.bold, ui::F.bold->FontSize, ImVec2(tx, t0.y - sp(0.25f)), ui::TEXT, ui::ellipsize(ui::F.bold, ui::F.bold->FontSize, m.mine ? "You sent an invite" : "Lobby invite", tw2).c_str());
            cdl->AddText(bf, fs, ImVec2(tx, t0.y + ui::F.bold->FontSize * 1.2f), ui::TEXT2, ui::ellipsize(bf, fs, gt, tw2).c_str());
            std::string sub = tm + "  -  " + (expired ? "expired" : "room " + m.inv.lobby_code);
            cdl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(tx, t0.y + ui::F.bold->FontSize * 1.2f + fs * 1.25f), expired ? ui::BAD : ui::TEXT3, sub.c_str());
            if (!m.mine) {
                ImGui::PushID((int)i);
                ImGui::SetCursorScreenPos(ImVec2(q0.x + pad, q1.y - pad - sp(3.f)));
                if (ui::button("Join", ImVec2(cwid - pad * 2, sp(3.f)), true, !expired))
                    fr_join_lobby(m.inv.lobby_code, m.inv.game_id, m.inv.game_title, (fr_find(m.from) ? fr_find(m.from)->name : std::string()));
                ImGui::PopID();
            }
            ImGui::SetCursorScreenPos(c);
            ImGui::Dummy(ImVec2(aw, ch + sp(1.f)));
            continue;
        }
        auto lines = ui::wrap_lines(bf, fs, m.text, aw * 0.78f - pad * 2, 60);
        float tw = fr_tw(ui::F.cap, tm);
        for (auto& l : lines) tw = std::max(tw, fr_tw(bf, l));
        float bw = tw + pad * 2, bh = pad * 2 + lines.size() * fs * 1.2f + ui::F.cap->FontSize + sp(0.25f);
        float x0 = m.mine ? c.x + aw - bw : c.x;
        ImVec2 q0(x0, c.y), q1(x0 + bw, c.y + bh);
        cdl->AddRectFilled(q0, q1, m.mine ? ui::mix(ui::ACCENT, ui::BG, 0.25f) : ui::PANEL2, sp(1.75f));
        float y = q0.y + pad;
        for (auto& l : lines) { cdl->AddText(bf, fs, ImVec2(q0.x + pad, y), m.mine ? IM_COL32_WHITE : ui::TEXT, l.c_str()); y += fs * 1.2f; }
        cdl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(q1.x - pad - fr_tw(ui::F.cap, tm), y + sp(0.125f)), m.mine ? IM_COL32(255, 255, 255, 170) : ui::TEXT3, tm.c_str());
        ImGui::Dummy(ImVec2(aw, bh + sp(0.75f)));
    }
    if (msgs.size() != FR.chat_seen) { ImGui::SetScrollHereY(1.f); FR.chat_seen = msgs.size(); }
    if (fr_panel_shown() && fr_unread(FR.chat_uid) > 0) fr_mark_read(FR.chat_uid);
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    ImGui::SetNextItemWidth(cw - sp(9.f));
    std::string hint = "Message " + f->name;
    bool enter = ImGui::InputTextWithHint("##fmsg", hint.c_str(), FR.chat_buf, 500, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(0, sp(0.75f));
    bool send = ui::button("Send", ImVec2(sp(8.25f), ImGui::GetFrameHeight()), true, FR.chat_buf[0] != 0);
    if ((enter || send) && FR.chat_buf[0]) {
        std::string t = FR.chat_buf;
        FR.chat_buf[0] = 0;
        if (FR.fake) {
            hubnet::Message m;
            m.id = std::to_string(wall_ms()); m.from = "me"; m.to = FR.chat_uid; m.mine = true; m.text = t; m.at = wall_ms();
            FR.f_conv[FR.chat_uid].push_back(m);
        } else FR.cli.send_message(FR.chat_uid, t);
        if (!A.pad_mode) ImGui::SetKeyboardFocusHere(-1);
    }
}

/* ---- the panel ---- */
void fr_draw_panel(float disp_w, float disp_h) {
    float pw = fr_panel_w(disp_w);
    float shown = FR.panel_t;
    FR.panel_focused = false;
    if (shown <= 0.002f) return;
    if (FR.want_focus) { ImGui::SetNextWindowFocus(); FR.want_focus = false; }
    ImGui::SetNextWindowPos(ImVec2(disp_w - pw * shown, 0));
    ImGui::SetNextWindowSize(ImVec2(pw, disp_h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(1.75f), sp(2.f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::PANEL);
    bool open = ImGui::Begin("##friends", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    if (!open) { ImGui::End(); return; }
    FR.panel_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    dl->AddLine(wp, wp + ImVec2(0, disp_h), ui::HAIRLINE, 1.f);
    float cw = ImGui::GetContentRegionAvail().x;
    long long now = wall_ms();

    if (!FR.chat_uid.empty()) {
        fr_chat_view(cw);
    } else {
        // title row
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushFont(ui::F.h2);
        dl->AddText(ui::F.h2, ui::F.h2->FontSize, ImVec2(p.x + sp(0.5f), p.y), ui::TEXT, "Friends");
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(p.x + cw - sp(4.f), p.y - sp(0.5f)));
        if (ui::icon_button("fr_close", ui::Icon::Close, sp(4.f), false, "Hide (Esc)")) { A.st.show_friends = false; mark_dirty(); }
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + sp(4.25f)));

        // identity card
        {
            ImVec2 c0 = ImGui::GetCursorScreenPos();
            float ch = sp(9.f);
            dl->AddRectFilled(c0, c0 + ImVec2(cw, ch), ui::PANEL2, sp(1.75f));
            dl->AddRect(c0, c0 + ImVec2(cw, ch), ui::HAIRLINE, sp(1.75f), 0, 1.f);
            fr_avatar(dl, c0 + ImVec2(sp(3.f), ch * 0.5f), sp(2.25f), A.st.name, A.st.appear_offline ? hubui::FS_Offline : (fr_connected() ? hubui::FS_Online : hubui::FS_Offline), ui::PANEL2);
            float tx = c0.x + sp(6.5f), tw = cw - sp(6.5f) - sp(9.f);
            dl->AddText(ui::F.bold, ui::F.bold->FontSize, ImVec2(tx, c0.y + sp(1.25f)), ui::TEXT, ui::ellipsize(ui::F.bold, ui::F.bold->FontSize, A.st.name, tw).c_str());
            std::string code = fr_my_code().empty() ? "..." : fr_my_code();
            dl->AddText(ui::F.body, ui::F.body->FontSize, ImVec2(tx, c0.y + sp(1.25f) + ui::F.bold->FontSize * 1.25f), ui::mix(ui::ACCENT, IM_COL32_WHITE, 0.55f), code.c_str());
            ImU32 dc = A.st.appear_offline ? ui::TEXT3 : (fr_connected() ? ui::GOOD : ui::WARN);
            dl->AddCircleFilled(ImVec2(tx + sp(0.5f), c0.y + ch - sp(1.75f)), sp(0.45f), dc, 10);
            dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(tx + sp(1.5f), c0.y + ch - sp(1.75f) - ui::F.cap->FontSize * 0.5f), ui::TEXT2,
                        ui::ellipsize(ui::F.cap, ui::F.cap->FontSize, A.st.appear_offline ? "Appearing offline" : fr_status_text(), tw).c_str());
            ImGui::SetCursorScreenPos(ImVec2(c0.x + cw - sp(8.5f), c0.y + (ch - sp(3.5f)) * 0.5f));
            if (ui::button("Copy", ImVec2(sp(7.25f), sp(3.5f)), false, !fr_my_code().empty())) {
                SDL_SetClipboardText(fr_my_code().c_str());
                set_toast("Friend code copied.", true, 3.0);
            }
            ImGui::SetCursorScreenPos(ImVec2(c0.x, c0.y + ch + sp(0.75f)));
        }
        if (ImGui::Checkbox("Appear offline", &A.st.appear_offline)) mark_dirty();
        ImGui::Dummy(ImVec2(0, sp(0.25f)));

        // add friend
        {
            ImGui::SetNextItemWidth(cw - sp(8.75f));
            bool enter = ImGui::InputTextWithHint("##addf", "Add friend: RH-XXXX-XXXX", FR.add_buf, sizeof FR.add_buf, ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine(0, sp(0.75f));
            bool add = ui::button("Add", ImVec2(sp(8.f), ImGui::GetFrameHeight()), true, FR.add_buf[0] != 0);
            if ((enter || add) && FR.add_buf[0]) {
                std::string code = hubui::normalize_friend_code(FR.add_buf);
                if (code.empty()) { FR.add_msg = "That doesn't look like a friend code (RH-XXXX-XXXX)."; FR.add_msg_until = ImGui::GetTime() + 6; }
                else if (code == fr_my_code()) { FR.add_msg = "That is your own code."; FR.add_msg_until = ImGui::GetTime() + 6; }
                else {
                    if (!FR.fake) FR.cli.add_friend(code);
                    FR.add_msg = "Friend request sent to " + code + ".";
                    FR.add_msg_until = ImGui::GetTime() + 5;
                    FR.add_buf[0] = 0;
                }
            }
            if (ImGui::GetTime() < FR.add_msg_until) {
                bool bad = FR.add_msg.find("sent") == std::string::npos;
                ImGui::PushStyleColor(ImGuiCol_Text, bad ? ui::BAD : ui::GOOD);
                ImGui::PushTextWrapPos(0.f);
                ImGui::TextUnformatted(FR.add_msg.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }
        }
        ImGui::Dummy(ImVec2(0, sp(0.5f)));

        // list
        ImGui::PushStyleColor(ImGuiCol_ChildBg, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::BeginChild("##frlist", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        float lw = ImGui::GetContentRegionAvail().x - sp(0.5f);
        std::vector<hubnet::Request> in = fr_incoming(), out = fr_outgoing();
        if (!in.empty() || !out.empty()) {
            fr_section("REQUESTS  -  " + std::to_string(in.size() + out.size()));
            for (auto& r : in) {
                ImGui::PushID(("in" + r.uid).c_str());
                ImVec2 p0 = ImGui::GetCursorScreenPos();
                float h = sp(6.f);
                ImDrawList* ldl = ImGui::GetWindowDrawList();
                fr_avatar(ldl, ImVec2(p0.x + sp(3.f), p0.y + h * 0.5f), sp(2.f), r.name, hubui::FS_Online, ui::PANEL);
                ldl->AddText(ui::F.bold, ui::F.bold->FontSize, ImVec2(p0.x + sp(6.f), p0.y + sp(0.75f)), ui::TEXT, ui::ellipsize(ui::F.bold, ui::F.bold->FontSize, r.name, lw - sp(22.f)).c_str());
                ldl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(p0.x + sp(6.f), p0.y + sp(0.75f) + ui::F.bold->FontSize * 1.25f), ui::TEXT3, "Friend request");
                float bh = sp(3.f), by = p0.y + (h - bh) * 0.5f, pwid = 0;
                float x = p0.x + lw;
                float w2 = fr_tw(ui::F.bold, "Decline") + sp(2.5f);
                if (fr_pill_button("dec", "Decline", ImVec2(x - w2, by), bh, false, true, &pwid)) { fr_trace("decline", r.uid); if (!FR.fake) FR.cli.decline(r.uid); else FR.f_in.clear(); }
                x -= pwid + sp(0.5f);
                float w3 = fr_tw(ui::F.bold, "Accept") + sp(2.5f);
                if (fr_pill_button("acc", "Accept", ImVec2(x - w3, by), bh, true, true, &pwid)) fr_accept(r.uid, r.name);
                ImGui::SetCursorScreenPos(p0);
                ImGui::Dummy(ImVec2(lw, h));
                ImGui::PopID();
            }
            for (auto& r : out) {
                ImGui::PushID(("out" + r.uid).c_str());
                ImVec2 p0 = ImGui::GetCursorScreenPos();
                float h = sp(6.f);
                ImDrawList* ldl = ImGui::GetWindowDrawList();
                std::string nm = r.name.empty() ? r.code : r.name;
                fr_avatar(ldl, ImVec2(p0.x + sp(3.f), p0.y + h * 0.5f), sp(2.f), nm, hubui::FS_Offline, ui::PANEL);
                ldl->AddText(ui::F.bold, ui::F.bold->FontSize, ImVec2(p0.x + sp(6.f), p0.y + sp(0.75f)), ui::TEXT2, ui::ellipsize(ui::F.bold, ui::F.bold->FontSize, nm, lw - sp(14.f)).c_str());
                ldl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(p0.x + sp(6.f), p0.y + sp(0.75f) + ui::F.bold->FontSize * 1.25f), ui::TEXT3, "request sent");
                float bh = sp(3.f), pwid = 0, w2 = fr_tw(ui::F.bold, "Cancel") + sp(2.5f);
                if (fr_pill_button("can", "Cancel", ImVec2(p0.x + lw - w2, p0.y + (h - bh) * 0.5f), bh, false, true, &pwid)) { if (!FR.fake) FR.cli.cancel(r.uid); else FR.f_out.clear(); }
                ImGui::SetCursorScreenPos(p0);
                ImGui::Dummy(ImVec2(lw, h));
                ImGui::PopID();
            }
            ImGui::Dummy(ImVec2(0, sp(0.5f)));
        }
        std::vector<hubui::FriendRow> rows;
        for (auto& f : fr_friends()) rows.push_back(fr_row(f));
        hubui::FriendGroups g = hubui::group_friends(rows);
        const auto& fl = fr_friends();
        if (!FR.d_stale.empty()) {   // identities the server confirmed were replaced by a newer one
            ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
            ImGui::PushTextWrapPos(lw);
            std::string names;
            for (auto& f : FR.d_stale) names += (names.empty() ? "" : ", ") + f.name;
            ImGui::TextUnformatted(("These are old accounts of people also listed under a newer one: " + names + ". Merge?").c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImVec2 bp = ImGui::GetCursorScreenPos();
            float bh = sp(3.f), pw = 0;
            if (fr_pill_button("merge", "Merge", bp, bh, true, true, &pw)) {
                for (auto& f : FR.d_stale) {
                    if (FR.fake) { for (size_t i = 0; i < FR.f_friends.size(); ++i) if (FR.f_friends[i].uid == f.uid) { FR.f_friends.erase(FR.f_friends.begin() + i); break; } }
                    else FR.cli.remove_friend(f.uid);
                }
                FR.d_frame = -1;
            }
            if (fr_pill_button("keepboth", "Keep both", ImVec2(bp.x + pw + sp(1.f), bp.y), bh, false, true, nullptr)) { FR.show_dupes = true; FR.d_frame = -1; }
            ImGui::Dummy(ImVec2(0, bh + sp(1.f)));
        }
        if (!FR.d_ask.empty()) {   // guesses only: one person at a time, never a bulk removal
            const hubnet::Friend af = FR.d_ask[0];   // copy: the list is rebuilt when we act
            long long now_ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            long long seen_ms = std::max(af.status.updated, af.since);
            ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
            ImGui::PushTextWrapPos(lw);
            ImGui::TextUnformatted(("Remove the old '" + af.name + "' (last seen " + (seen_ms > 0 ? hubui::format_ago_ms(now_ms, seen_ms) : std::string("a while ago")) +
                                    ")? Another friend with the same name was seen more recently. They may be a different person.").c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImVec2 bp = ImGui::GetCursorScreenPos();
            float bh = sp(3.f), pw = 0;
            if (fr_pill_button("askrm", "Remove", bp, bh, true, true, &pw)) {
                if (FR.fake) { for (size_t i = 0; i < FR.f_friends.size(); ++i) if (FR.f_friends[i].uid == af.uid) { FR.f_friends.erase(FR.f_friends.begin() + i); break; } }
                else FR.cli.remove_friend(af.uid);
                FR.d_frame = -1;
            }
            if (fr_pill_button("askkeep", "Keep", ImVec2(bp.x + pw + sp(1.f), bp.y), bh, false, true, nullptr)) { FR.kept_uids.insert(af.uid); FR.d_frame = -1; }
            ImGui::Dummy(ImVec2(0, bh + sp(1.f)));
        }
        if (!g.online.empty()) {
            fr_section("ONLINE  -  " + std::to_string(g.online.size()));
            for (int i : g.online) fr_friend_row(fl[i], rows[i], lw, now);
        }
        if (!g.offline.empty()) {
            ImGui::Dummy(ImVec2(0, sp(0.5f)));
            fr_section("OFFLINE  -  " + std::to_string(g.offline.size()));
            for (int i : g.offline) fr_friend_row(fl[i], rows[i], lw, now);
        }
        if (fl.empty()) {
            ImGui::Dummy(ImVec2(0, sp(1.f)));
            ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
            ImGui::PushTextWrapPos(lw);
            ImGui::TextUnformatted("No friends yet. Share your friend code, or paste a friend's code above to send a request. Friends can see when you're online, message you and invite you to their lobby.");
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
    }

    // remove / block confirmation
    if (FR.confirm_open) { ImGui::OpenPopup("Are you sure?##frconf"); FR.confirm_open = false; }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(2.5f), sp(2.5f)));
    if (ImGui::BeginPopupModal("Are you sure?##frconf", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        bool block = FR.confirm_kind == "block";
        ImGui::PushTextWrapPos(sp(36.f));
        ImGui::TextUnformatted((std::string(block ? "Block " : "Remove ") + FR.confirm_name + (block ? "? They will be removed from your friends and can't contact you." : " from your friends?")).c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, sp(1.f)));
        if (ui::button(block ? "Block" : "Remove", ImVec2(sp(14.f), sp(5.f)), true)) {
            if (FR.fake) {
                for (size_t i = 0; i < FR.f_friends.size(); ++i) if (FR.f_friends[i].uid == FR.confirm_uid) { FR.f_friends.erase(FR.f_friends.begin() + i); break; }
            } else if (block) FR.cli.block(FR.confirm_uid);
            else FR.cli.remove_friend(FR.confirm_uid);
            if (FR.chat_uid == FR.confirm_uid) FR.chat_uid.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ui::button("Cancel", ImVec2(sp(14.f), sp(5.f)))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    ImGui::End();
}

/* Back / Esc: chat -> list -> hide. Returns true when consumed. */
bool fr_handle_back() {
    if (!A.st.show_friends || !FR.panel_focused) return false;
    if (!FR.chat_uid.empty()) FR.chat_uid.clear();
    else { A.st.show_friends = false; mark_dirty(); ImGui::SetWindowFocus("##root"); }
    return true;
}

/* ---- invite picker (Play Online room page) ---- */
void fr_invite_button_and_picker(float btn_w, float btn_h) {
    int online = 0;
    for (auto& f : fr_friends()) if (f.online()) ++online;
    bool can = fr_in_my_room();
    if (ui::button("Invite friends", ImVec2(btn_w, btn_h), true, can, ui::Icon::Plus) || (FR.open_picker && can)) {
        FR.pick.clear();
        ImGui::OpenPopup("Invite friends##picker");
        FR.open_picker = false;
    }
    if (ImGui::IsItemHovered() && !can) ImGui::SetTooltip("Waiting for the room to connect.");
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(sp(60.f), ds.x * 0.92f), std::min(sp(72.f), ds.y * 0.86f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(2.5f), sp(2.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.5f));
    bool open = ImGui::BeginPopupModal("Invite friends##picker", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleVar(2);
    if (!open) return;
    hubnet::Invite inv = fr_make_invite();
    float cw = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    {   // game summary
        ImVec2 p = ImGui::GetCursorScreenPos();
        float hh = sp(7.f);
        dl->AddRectFilled(p, p + ImVec2(cw, hh), ui::PANEL2, sp(1.5f));
        fr_game_thumb(dl, inv.game_id, inv.game_title, p + ImVec2(sp(1.f), sp(1.f)), p + ImVec2(sp(6.f), hh - sp(1.f)), sp(0.75f));
        dl->AddText(ui::F.bold, ui::F.bold->FontSize, p + ImVec2(sp(7.5f), sp(1.25f)), ui::TEXT, inv.game_title.empty() ? "No game picked yet" : inv.game_title.c_str());
        dl->AddText(ui::F.cap, ui::F.cap->FontSize, p + ImVec2(sp(7.5f), sp(1.25f) + ui::F.bold->FontSize * 1.3f), ui::TEXT2, ("Room " + A.code + "  -  invites expire in 30 min").c_str());
        ImGui::Dummy(ImVec2(cw, hh + sp(1.f)));
    }
    std::vector<hubui::FriendRow> rows;
    for (auto& f : fr_friends()) rows.push_back(fr_row(f));
    hubui::FriendGroups g = hubui::group_friends(rows);
    const auto& fl = fr_friends();
    float foot = sp(8.f);
    ImGui::BeginChild("##pick", ImVec2(0, -foot), ImGuiChildFlags_NavFlattened);
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    float lw = ImGui::GetContentRegionAvail().x - sp(0.5f);
    if (g.online.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
        ImGui::PushTextWrapPos(lw);
        ImGui::TextUnformatted(fl.empty() ? "You have no friends yet. Add some from the friends panel, then invite them here."
                                          : "None of your friends are online right now. Share the room code instead, or invite them later.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    for (int i : g.online) {
        const hubnet::Friend& f = fl[i];
        bool in_room = f.status.lobby_code == A.code;
        ImGui::PushID(f.uid.c_str());
        float h = sp(6.5f);
        ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + lw, p0.y + h);
        ImGui::BeginDisabled(in_room);
        bool clicked = ImGui::InvisibleButton("row", ImVec2(lw, h), ImGuiButtonFlags_EnableNav);
        bool hov = ImGui::IsItemHovered(), focus = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
        ImGui::EndDisabled();
        bool sel = FR.pick.count(f.uid) > 0;
        if (clicked) { if (sel) FR.pick.erase(f.uid); else FR.pick.insert(f.uid); sel = !sel; }
        cdl->AddRectFilled(p0, p1, sel ? with_alpha(ui::ACCENT, 0.2f) : (hov || focus ? ui::PANEL2 : ui::PANEL), sp(1.5f));
        if (sel) cdl->AddRect(p0, p1, with_alpha(ui::ACCENT, 0.7f), sp(1.5f), 0, 1.5f);
        fr_avatar(cdl, ImVec2(p0.x + sp(3.f), p0.y + h * 0.5f), sp(2.f), f.name, rows[i].state, sel ? ui::PANEL2 : ui::PANEL);
        float tx = p0.x + sp(6.f), tw = lw - sp(11.f);
        cdl->AddText(ui::F.bold, ui::F.bold->FontSize, ImVec2(tx, p0.y + sp(1.25f)), ui::TEXT, ui::ellipsize(ui::F.bold, ui::F.bold->FontSize, f.name, tw).c_str());
        cdl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(tx, p0.y + sp(1.25f) + ui::F.bold->FontSize * 1.25f), in_room ? ui::GOOD : ui::TEXT2,
                     ui::ellipsize(ui::F.cap, ui::F.cap->FontSize, in_room ? "Already in your room" : hubui::friend_status_line(rows[i], wall_ms()), tw).c_str());
        ImVec2 bc(p1.x - sp(3.f), p0.y + h * 0.5f);
        if (in_room) ui::icon(cdl, ui::Icon::Check, bc, sp(2.5f), ui::GOOD);
        else {
            cdl->AddCircle(bc, sp(1.25f), sel ? ui::ACCENT : ui::TEXT3, 20, 2.f);
            if (sel) { cdl->AddCircleFilled(bc, sp(1.25f), ui::ACCENT, 20); ui::icon(cdl, ui::Icon::Check, bc, sp(1.9f), IM_COL32_WHITE); }
        }
        if (focus) ui::focus_ring(cdl, p0, p1, sp(1.5f));
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        ImGui::PopID();
    }
    int off = (int)g.offline.size();
    if (off > 0 && !g.online.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
        ImGui::Text("%d friend%s offline", off, off == 1 ? " is" : "s are");
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, sp(0.75f)));
    int n = (int)FR.pick.size();
    std::string lab = n ? "Send invite (" + std::to_string(n) + ")" : "Send invite";
    if (ui::button(lab.c_str(), ImVec2(sp(20.f), sp(5.f)), true, n > 0)) {
        fr_invite_uids(std::vector<std::string>(FR.pick.begin(), FR.pick.end()));
        FR.pick.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ui::button("Cancel", ImVec2(sp(12.f), sp(5.f)))) ImGui::CloseCurrentPopup();
    if (!g.online.empty()) {
        ImGui::SameLine();
        if (ui::button("Select all", ImVec2(sp(12.f), sp(5.f)))) {
            for (int i : g.online) if (fl[i].status.lobby_code != A.code) FR.pick.insert(fl[i].uid);
        }
    }
    ImGui::EndPopup();
}
