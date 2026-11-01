#pragma once
/* Party mode UI plumbing (Plan 4 P1-P3), shared by the files the party lane owns: party_ui.cpp (flow: messages, tick, rounds, OSD),
 * party_view.cpp (drawing: room panel, overlays, shots), lobby.cpp, page_online.cpp and picker_ui.cpp.
 * app.h (Opus-only) declares just the seam hooks (party_on_msg, party_tick, party_on_session_end, party_room_panel, party_draw,
 * party_shot_init); everything else the lane needs is declared here. Include after app.h. */
#include <map>
#include <string>
#include <vector>

#include "party_logic.h"

/* ---- called by lobby.cpp / page_online.cpp / picker_ui.cpp (global, like the app.h hooks) ---- */
void party_on_leave_room();                                    // lobby.cpp leave_room(): the room (and its party) is gone
void party_on_host_change(int promoted_from_slot);             // lobby.cpp HostChange: the server renumbered that member to slot 0
void party_on_host_select(const std::string& game_id);         // picker_ui.cpp: the host picked a game by hand
void party_couch_open();                                       // page_online.cpp home: the "Couch party" button
bool party_couch_active();                                     // a couch party is on (the Play Online home shows "Continue")
std::string party_result_file_for_launch();                    // "" or the path a local Smash launch should write finished matches to (launch.cpp hook, see the lane report)

/* ---- the room page (page_online.cpp) ---- */
std::string room_host_launch_block(bool* can_launch);          // "" + true when the host's Launch button is enabled right now, else why not
std::string room_guest_ready_block();                          // "" or why this PC cannot ready up for the room's game right now

/* ---- picker modes (picker_ui.cpp) ---- */
enum class PickMode { Choose /* the host picks the room's game */, Playlist /* tiles add games to the party playlist */, Vote /* a tile casts a vote */ };
void room_picker_open(PickMode m);                             // opens the big picker in that mode (any member in Vote mode)
std::vector<std::string> room_picker_pool(bool prefer_verified);   // ids of games everyone can play, sorted (the Shuffle pool)
std::vector<std::string> room_picker_shortlist(const std::string& first, size_t n);   // a vote shortlist: `first`, then recently played / favourites everyone can play

namespace partyui {

using State = hubui::party::State;

struct Seat {
    int slot = 0;
    std::string name;
    bool me = false, ready = false, host = false;
    bool can_party = true;     // false: an older RetroHub that ignores pt_* messages
};

/* ---- read ---- */
const State& state();
bool active();                                   // a party is set up / running, or a vote is open
bool is_host();                                  // the room host, or the couch party's owner
bool couch();
int my_slot();                                   // -1 in a couch party
std::vector<Seat> seats();                       // everyone in the party now, in slot order
std::string slot_name(int slot);                 // names in the State win (standings survive a leave), then the room
std::string title_of(const std::string& game_id);
const hub::GameEntry* entry_of(const std::string& game_id);
int secs_left();                                 // the round timer: seconds left, -1 = no timer / not playing
std::vector<std::string> need_update();          // names that cannot do Party mode (older RetroHub)
std::string my_vote();                           // my pick in the open poll ("" = none yet)
bool overlay_wanted();
void overlay_close();
void overlay_open();
std::vector<std::string> shortlist_for_vote();   // candidates for "vote for the next game"
bool host_can_start_round(std::string* why);     // the Launch button of the next round would work right now
/* The tie-break reel (Surprise-me style): the options that tied and the winner, while it is spinning. */
struct Reel { bool on = false; std::vector<std::string> tied; std::string pick; double t0 = 0; double frozen = -1; /* >= 0: screenshots hold the reel at that second */ };
const Reel& reel();
void reel_done();

/* ---- act (the host's buttons; every one goes through the pure reducer, then broadcasts) ---- */
void turn_on();                                  // Off -> Lobby with a shuffled first playlist
void turn_off();                                 // any phase -> Off
void set_options(int rounds, hubui::party::Scoring scoring, int round_secs, bool auto_ready);
void set_shuffle(bool on);
void reshuffle();
void add_game(const std::string& game_id);
void remove_game(size_t index);
void clear_games();
void start();                                    // Lobby -> Intermission: the first game is preselected
void launch_next_round();                        // press Launch for the preselected game (room) / play it (couch)
void set_next_game(const std::string& game_id);  // the host picks another game for the next round
void cast_vote(const std::string& pick);         // any member
void open_next_vote();                           // the host asks the room (shortlist_for_vote)
void close_vote();                               // the host settles the poll now
void decide_winner(const std::vector<int>& slots);   // "who won?": the host (or a couch player) names the winner(s); empty = skip
void skip_round();
void finish_night();                             // Intermission after the last round -> Done (normally automatic)

/* ---- couch party setup (a modal drawn by party_view.cpp) ---- */
struct CouchSetup {
    bool open = false;
    int players = 2;
    std::string names[4];
    int rounds = 5;
    hubui::party::Scoring scoring = hubui::party::Scoring::Auto;
    int round_secs = 0;
    bool shuffle = true;
    std::vector<std::string> list;
};
CouchSetup& couch_setup();
std::vector<std::string> couch_pool();           // library games with 2+ local players
void couch_begin();                              // start the night from couch_setup()

}  // namespace partyui
