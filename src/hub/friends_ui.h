#pragma once
/* friends_ui.h - friends list panel, chat, lobby invites, notifications (friends_ui.cpp). Included at the end of app.h. */

#include "friends.h"

#include <chrono>
#include <set>

struct Notif {
    int id = 0;
    bool invite = true;            // false = friend-request toast (the bell lists live requests instead)
    std::string uid, name;
    hubnet::Invite inv;
    double t0 = 0;                 // ImGui time of arrival
    bool toast = true;             // toast still showing
    bool done = false;
};

struct FriendsUi {
    hubnet::FriendsClient cli;
    bool started = false, fake = false;
    std::vector<hubnet::Friend> f_friends;
    std::vector<hubnet::Request> f_in, f_out;
    std::map<std::string, std::vector<hubnet::Message>> f_conv;
    std::map<std::string, int> f_unread;
    /* Render caches: the source lists run through hubnet::dedupe_* once per frame, so a person is never drawn twice. */
    std::vector<hubnet::Friend> d_friends;
    std::vector<hubnet::Request> d_in, d_out;
    std::vector<hubnet::Friend> d_stale;   // CONFIRMED old identities (server alias map / retired-ids.txt): hidden until merged (bulk merge is safe)
    std::vector<hubnet::Friend> d_ask;     // UNCONFIRMED same-name guesses: stay visible, each needs its own "Remove the old ...?" confirm
    std::set<std::string> kept_uids;       // user answered "Keep" for these this session
    bool show_dupes = false;
    int d_frame = -1;

    std::string chat_uid;
    int chat_unread = 0;           // unread count when the chat was opened ("New messages" marker)
    size_t chat_seen = 0;
    char chat_buf[512] = {0};
    char add_buf[48] = {0};
    std::string add_msg;
    double add_msg_until = 0;

    float panel_t = -1;
    bool want_focus = false, panel_focused = false;
    std::vector<Notif> notifs;
    int next_id = 1;
    hubnet::Status last_status;
    bool status_sent = false;
    struct PendingJoin { std::string code, game_id, title, name; } pending_join;   // queued while our own game runs
    std::set<std::string> hist_asked;
    std::string confirm_kind, confirm_uid, confirm_name;
    bool confirm_open = false;
    std::set<std::string> pick;
    std::map<std::string, double> invited_at;
    bool open_picker = false;
    bool shot_bell = false;
    std::string last_name;
    double name_changed = 0;
};

enum class FrIcon { Bell, People, Chat, UserPlus, Copy, More, Send };

extern FriendsUi FR;


long long wall_ms();
const std::vector<hubnet::Friend>& fr_friends();
const std::vector<hubnet::Request>& fr_incoming();
const std::vector<hubnet::Request>& fr_outgoing();
const std::vector<hubnet::Message>& fr_conv(const std::string& uid);
int fr_unread(const std::string& uid);
int fr_total_unread();
void fr_mark_read(const std::string& uid);
bool fr_connected();
std::string fr_status_text();
std::string fr_my_code();
const hubnet::Friend* fr_find(const std::string& uid);
hubui::FriendRow fr_row(const hubnet::Friend& f);
ImU32 fr_state_col(int st);
ImU32 fr_avatar_col(const std::string& name);
void fr_avatar(ImDrawList* dl, ImVec2 c, float r, const std::string& name, int state, ImU32 ring);
void fr_text_c(ImDrawList* dl, ImFont* f, ImVec2 c, ImU32 col, const char* s);
float fr_tw(ImFont* f, const std::string& s);
void fr_icon(ImDrawList* dl, FrIcon k, ImVec2 c, float s, ImU32 col);
bool fr_icon_button(const char* id, FrIcon k, float size, bool active, int badge, ImU32 badge_col, const char* tip, ImVec2* rect_max = nullptr);
bool fr_pill_button(const char* id, const char* label, ImVec2 pos, float h, bool primary, bool enabled = true, float* width_out = nullptr);
void fr_game_thumb(ImDrawList* dl, const std::string& game_id, const std::string& title, ImVec2 p0, ImVec2 p1, float rad);
std::string fr_time(long long ms);
int fr_day(long long ms);
std::string fr_day_label(long long ms);
hubnet::Invite fr_make_invite();
bool fr_in_my_room();
void fr_invite_uids(const std::vector<std::string>& uids);
void fr_join_lobby(const std::string& code, const std::string& game_id, const std::string& title, const std::string& who = "");
void fr_open_chat(const std::string& uid);
void fr_toggle_panel();
void fr_notify(bool invite, const std::string& uid, const std::string& name, const hubnet::Invite& inv);
int fr_pending_count();
bool fr_panel_shown();
std::string fr_error_text(const std::string& r, const std::string& detail = std::string());
hubnet::Status fr_desired_status();
void fr_update_presence();
void fr_start();
void fr_run_pending_join();
void fr_adopt_identity_after_vault_change();
void fr_tick();
void fr_fake_init();
float fr_panel_w(float disp_w);
void fr_anim_panel();
float fr_invite_card_h();
int fr_invite_card(const Notif& n, float w, float frac, bool shadow);
float fr_request_card_h();
int fr_request_card(const std::string& name, const std::string& code, float w, float frac, bool shadow, const char* decline);
void fr_accept(const std::string& uid, const std::string& name);
void fr_join_notif(Notif& n);
void fr_draw_toasts(float right_x, float bottom_y);
void fr_top_bar();
void fr_confirm(const char* kind, const hubnet::Friend& f);
void fr_friend_row(const hubnet::Friend& f, const hubui::FriendRow& fr, float w, long long now);
void fr_section(const std::string& title);
void fr_chat_view(float cw);
void fr_draw_panel(float disp_w, float disp_h);
bool fr_handle_back();
void fr_invite_button_and_picker(float btn_w, float btn_h);
