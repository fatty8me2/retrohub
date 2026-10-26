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
    bool ask_host = false;         // the ask is "can you host this?" (they do not own the game)
    bool ask = false;              // "<name> wants to join your game": Accept sends them an invite (inv.game_* = the game they asked about)
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
    hubui::AskLimiter ask_lim;                 // our own "Ask to join" requests
    std::map<std::string, std::vector<hubnet::LibGame>> f_libs;   // fake mode (screenshots): friends' game lists
    std::map<std::string, long long> ask_seen; // host side: last ask per person (repeat within 30 s is ignored)
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
void fr_set_auto_friend(bool on);        // Settings toggle: tells the server (also sent in every hello)
std::string fr_lobby_ident_msg();        // "ident" frame for the lobby socket ("" when there is no account yet)
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
bool fr_invite_toast_visible();   // an invite toast is on screen (pad: Start joins it)
bool fr_join_invite_toast();      // joins the newest visible invite toast
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
int fr_request_card(const std::string& name, const std::string& code, float w, float frac, bool shadow, const char* decline,
                    const std::string& title = std::string(), const char* accept = "Accept");
void fr_ask_to_join(const std::string& uid);                       // "Ask to join": rate limited, over the invite channel
void fr_notify_ask(const std::string& uid, const std::string& name, const hubnet::Invite& game, bool host = false);
/* game libraries (friend_games_ui.cpp): who owns what, metadata only */
void fr_library_collect(std::vector<std::pair<std::string, const std::vector<hubnet::LibGame>*>>* out);   // friends (not blocked) with a shared list
uint32_t fr_library_epoch();                                                                              // changes when any friend list changed
void fr_publish_library(bool share, std::vector<hubnet::LibGame> games);
void fr_ask_host(const std::string& uid, const std::string& game_id, const std::string& title, const std::string& system);
void fr_ask_accept(Notif& n);                                       // host accepted: invite sent (needs a room)
bool fr_home_shelf(float avail, float tw, float th, float gap, float bleed);   // Home "Friends are playing" (false = nothing to show)
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

/* ---- username_ui.cpp ---- */
#include "username_logic.h"
bool un_claimed();                       // this PC has a server-confirmed username (A.st.name is it)
bool un_claiming();                      // a claim is in flight
bool un_take_claimed();                  // true once after a claim succeeded (the wizard advances)
bool un_needs_new();                     // the server flagged this older account
hubui::un::Avail un_state();
bool un_text_valid();
std::string un_text();                    // the trimmed text in the box
void un_seed_text();                     // fill the box from the Windows name when empty
void un_open_prompt();                   // the "Pick your username" dialog
void un_open_change();                   // Settings > Account: open the change panel
bool un_claim_start();                   // claim the name in the box (needs "available")
void un_qa_type(const std::string& t);   // QA (userflow.cpp): type into the box
std::string un_qa_status();              // QA: the status line the player sees
bool un_qa_prompt_open();                // QA: the "Pick your username" dialog is up
void un_tick();
void un_on_event(const hubnet::Event& e);
bool un_picker(const char* id, float width);   // box + live status + suggestions; true on Enter
void un_account_block();                 // Settings > Account top block
void draw_username_overlays();           // banner + prompt
