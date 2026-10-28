/* Drop-in: "Join game" on a friend's running 3-4 port NES/SNES party game that still has a free controller port. Like Watch, this PC takes a
 * lobby seat in the friend's room (role=watch&dropin=1: not a member, costs the room nothing), gets the host's launch message (stored by the
 * server) and starts retrorun with --dropin: the runner asks the host for a snapshot of the running state and takes the free port (the first
 * -1 of the plan gets our lobby slot K). Refused here when the launch shares the host save, uses cheats, has no "dropin" count (old host), or
 * the game is not a rollback launch; refused by the server with no-such-game / host-too-old / no-free-port. Never touches a running game. */
#include "app.h"

bool g_dropin_launch = false;
std::string g_dropin_code, g_dropin_ports;
int g_dropin_slot = 0, g_dropin_players = 0;

namespace {
struct DropState {
    hubnet::LobbyClient lc;
    bool active = false;
    std::string code, game_id, title, friend_name;
    double since = 0;
} DP;

void drop_stop() {
    DP.lc.stop();
    DP.active = false;
}
}  // namespace

bool dropin_pending() { return DP.active; }

bool dropin_supported(const hub::GameEntry& e) {
    return !is_native(e) && (e.system == hub::System::NES || e.system == hub::System::SNES);
}

void dropin_start(const std::string& code, const std::string& game_id, const std::string& title, const std::string& friend_name) {
    if (DP.active) return;
    if (A.proc) { set_notice("A game is already running on this PC.", 6.0); return; }
    if (A.in_room) { set_notice("Leave your room first, then you can join a friend's game.", 8.0); return; }
    if (A.st.name.empty()) { set_notice("Pick a name first (Settings), then you can join.", 8.0); return; }
    const hub::GameEntry* g = find_room_entry(game_id);
    if (!g || is_remote_stand_in(*g)) { set_notice("You need your own copy of " + (title.empty() ? std::string("this game") : title) + " to join it.", 8.0); return; }
    if (!dropin_supported(*g)) { set_notice("Joining a game in progress isn't available for " + g->title + ".", 8.0); return; }
    DP.code = code; DP.game_id = game_id; DP.title = title; DP.friend_name = friend_name;
    if (!DP.lc.start_dropin(server_url(), code, A.st.name)) { set_notice("Couldn't reach the server.", 6.0); return; }
    DP.active = true;
    DP.since = ImGui::GetTime();
    set_notice("Joining " + friend_name + "'s game...", 6.0);
}

void dropin_tick() {
    if (!DP.active) return;
    DP.lc.update();
    if (DP.lc.failed()) {
        std::string why = DP.lc.status();
        drop_stop();
        set_notice("Can't join right now: " + why, 10.0);
        return;
    }
    if (!DP.lc.connected()) {
        if (ImGui::GetTime() - DP.since > 40.0) { drop_stop(); set_notice("Couldn't connect to the room.", 8.0); }
        return;
    }
    hubnet::LobbyEvent e;
    while (DP.lc.poll_event(&e)) {
        if (e.type != hubnet::LobbyEvent::Msg || e.from_slot != 0) continue;
        hubui::Json j;
        if (!json::parse(e.body, &j) || j.type != hubui::Json::Object || j.str("kind") != "launch") continue;
        const hub::GameEntry* g = find_room_entry(j.str("game"));
        if (!g || is_remote_stand_in(*g)) { drop_stop(); set_notice("You don't have the game " + DP.friend_name + " is playing.", 8.0); return; }
        if (!dropin_supported(*g)) { drop_stop(); set_notice("Joining a game in progress isn't available for " + g->title + ".", 8.0); return; }
        if (A.proc) { drop_stop(); set_notice("A game is already running on this PC.", 6.0); return; }
        std::string why, ports;
        int slot = 0, players = 0;
        if (!hubui::parse_dropin_launch(j, &why, &slot, &ports, &players)) { drop_stop(); set_notice(why, 10.0); return; }
        set_notice("Joining " + DP.friend_name + "'s game in progress...", 10.0);
        g_dropin_launch = true;
        g_dropin_code = DP.code;
        g_dropin_slot = slot;
        g_dropin_ports = ports;
        g_dropin_players = players;
        launch_online(*g, (int)j.num("launch", 1), 0, false, j.str("core"), false, nullptr, &j, nullptr, &e.body);
        g_dropin_launch = false;
        g_dropin_code.clear();
        g_dropin_ports.clear();
        drop_stop();   // the runner holds its own seat in the game room
        return;
    }
    if (ImGui::GetTime() - DP.since > 60.0) { drop_stop(); set_notice("Nothing to join - the game may have ended.", 8.0); }
}
