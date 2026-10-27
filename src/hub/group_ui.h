#pragma once
/* group_ui.h - play again / recent groups / notification history (group_ui.cpp). Included at the end of app.h.
 * Pure rules live in group_logic.h (unit tested). */
#include "group_logic.h"
#include "rivals_logic.h"

struct GroupUi {
    hubui::group::RecentGroups groups;
    hubui::group::NotifHistory hist;
    bool loaded = false;
    double saved_at = -100;
    bool again = false;                       // a session just ended in this room: banner offers PLAY AGAIN
    std::string again_game;                   // room game id of that session
    std::vector<std::string> pending_invite;  // "Invite last group": uids to invite once the new room is up
    double pending_at = 0;
    int bell_frame = -10;                     // last frame the bell list was drawn (marks everything read when it closes)
    bool bell_seen = false;
    int last_conflicts = 0;
    bool last_update = false;
};
extern GroupUi GRP;

void group_tick();                              // every frame (main loop)
void group_session_ended(unsigned long long secs);   // an online session just ended (poll_process / GameCube exit)
void group_left_room();                         // leave_room(): drop the play-again state
bool group_play_again_active();                 // banner: offer PLAY AGAIN now
void group_decorate_room_action(bool update_mode, bool is_launch, bool ready_on, bool enabled, std::string& label, std::string& headline);
void group_invite_last();                       // Play Online: open a room and invite the last group
const hubui::group::RecentGroup* group_last();  // newest group (nullptr = none)
std::string group_last_label();                 // "Invite last group (Mia, Jake)"
std::string group_together_text(const std::string& uid);   // "Last played together: ..." or ""
/* rivalries (Wave 4 S2; rivals_logic.h) */
void rivals_ingest(const std::string& result_file);                 // the game exited: read + store its match result (file is deleted)
std::string rivals_record_text(const std::string& uid, const std::string& name);   // "You 12 - 9 Mickey" ("" = no games yet)
std::string rivals_mains_text(const std::string& uid);              // "Mario vs Fox"
std::string rivals_last5(const std::string& uid);                   // "WLWWL" oldest to newest
std::string rivals_sessions_text(const std::string& uid);           // other games: "3 sessions together"
void rivals_draw_card();                                            // Your stats card: top rivals (draws nothing when empty)
void rivals_fake_seed();                                            // screenshots
int group_unread();                             // bell badge
void group_note(hubui::group::Kind k, const std::string& uid, const std::string& name, const std::string& game_id,
                const std::string& title, const std::string& code, const std::string& text, long long expires_at);
void group_note_invite(const std::string& uid, const std::string& name, const hubnet::Invite& inv);
void group_note_ask_join(const std::string& uid, const std::string& name, const std::string& code, const std::string& title);   // for Lane 3
void group_resolve(hubui::group::Kind k, const std::string& uid, const std::string& code = std::string());
void group_draw_bell_list(float cw);            // body of the bell popup
void group_fake_seed();                         // screenshots (HUB_SHOT_FAKE_FRIENDS)
