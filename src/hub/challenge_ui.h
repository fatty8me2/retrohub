#pragma once
/* challenge_ui.h - friend challenges in the hub (Plan 4 p4-social). Pure rules/cards: challenge_logic.h; state machines: challenge_flow.h;
 * file transfer: attach_xfer.h. friends_ui.cpp only calls challenge_on_message / challenge_draw_card; quickmenu.cpp drives the rest. */
#include "net/friends.h"
#include "challenge_logic.h"
#include "challenge_flow.h"

#include <functional>
#include <string>
#include <vector>

namespace hubui {

/* ---- chat side (the contract friends_ui uses) ---- */
/* Called for every message that arrives or is loaded into a conversation with a non-empty msg.card: remembers Result cards so a challenge
 * card's leaderboard fills in. Cheap and idempotent per message id. */
void challenge_on_message(const hubnet::Message& m);
/* Draws a challenge/result card inline in the chat. `width` = available width. false = m.card is not a challenge/result card. */
bool challenge_draw_card(const hubnet::Message& m, float width);

/* ---- game menu side (quickmenu.cpp) ---- */
struct ChallengeHost {
    std::function<void(int slot)> save_state;                          // enqueue runctl save_state for a slot
    std::function<void(int corner, const std::string& text)> corner;   // enqueue runctl osd_corner ("" clears)
    std::function<void(const std::string& text)> osd;                  // enqueue runctl osd_msg
};
void challenge_set_host(ChallengeHost h);
/* Every frame (from qm_tick). menu_open = the game menu is showing (the game is paused: the clock does not run). */
void challenge_tick(bool menu_open);

/* Sender flow. begin() picks a scratch slot and asks the host to save a state into it. */
struct ChallengeSendView {
    hub::challenge::SendPhase phase = hub::challenge::SendPhase::Idle;
    std::string status;     // progress line while Saving / Uploading / Sending, the result when Sent
    std::string error;      // plain English when Failed
    std::string title;
    hub::challenge::Goal goal;
    std::vector<std::string> selected;   // friend uids
    std::string thumb;      // path of the picture that goes with the challenge ("" = none)
};
const ChallengeSendView& challenge_send_view();
bool challenge_send_begin(const std::string& game_key, const std::string& title, const std::string& system, const std::string& state_dir);
void challenge_send_goal(hub::challenge::GoalKind k);
bool challenge_send_score(const std::string& typed);   // false = not a number
bool challenge_send_toggle_friend(const std::string& uid);   // false = already 8
void challenge_send_go();                              // PickFriends -> upload + send
void challenge_send_back();
void challenge_send_cancel();
void challenge_send_reset();                           // after Sent / Failed was shown

/* Run flow (the friend who accepted). */
bool challenge_running();                      // a challenge is being played (rows "I did it" / "Give up")
bool challenge_busy();                         // downloading or launching or running: no second one
bool challenge_goal_is_score();
void challenge_run_finish(bool did_it, const std::string& typed_score);   // posts the result card and stops the clock
bool challenge_pending_no_ra();                // achievements_launch_args(): true once for the launch of a challenge run (RetroAchievements stay off)

/* Screenshot aid (HUB_SHOT_CHALLENGE=card|results|cant): draws a preview of the chat cards. Called from draw_quick_menu while headless. */
void challenge_shot_draw();

} // namespace hubui
