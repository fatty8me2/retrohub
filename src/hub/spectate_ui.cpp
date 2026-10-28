/* Spectate: "Watch" on a friend's room. This PC joins the friend's lobby room as a spectator seat (role=watch; it is not a member and
 * costs the room nothing), gets the host's launch message (stored by the server, or live if the host launches while we wait), and starts
 * n64run / retrorun with --spectate. retrorun joins live: the host sends a snapshot of its confirmed state and the confirmed inputs from
 * there (src/n64run/spectate.h, kPktJoin), the watcher fast-forwards the few buffered frames and follows in real time. n64run has no state
 * transfer: it replays the host's confirmed inputs from the start at full speed until it reaches the live game (the notice says so). Limits: 2 spectators per room, refused when the server is over 50% of its daily budget, and games that
 * share a host save or use cheats can't be followed (the watcher would start from a different state; the runner would report a desync).
 * Supported: N64 (not native Smash 64), GB/GBC/GBA (no link cable), NES, SNES. */
#include "app.h"

bool g_spectate_launch = false;
std::string g_spectate_code;

namespace {
struct SpecState {
    hubnet::LobbyClient lc;
    bool active = false;
    std::string code, game_id, title, friend_name;
    double since = 0;
} SP;

void spec_stop() {
    SP.lc.stop();
    SP.active = false;
}
}  // namespace

bool spectate_pending() { return SP.active; }

bool spectate_supported(const hub::GameEntry& e) {
    if (is_native(e)) return false;
    switch (e.system) {
        case hub::System::N64: case hub::System::NES: case hub::System::SNES: return true;
        case hub::System::GB: case hub::System::GBC: case hub::System::GBA: return !hub::link_cable_game(e.catalog_id);
        default: return false;
    }
}

void spectate_start(const std::string& code, const std::string& game_id, const std::string& title, const std::string& friend_name) {
    if (SP.active) return;
    if (A.proc) { set_notice("A game is already running on this PC.", 6.0); return; }
    if (A.in_room) { set_notice("Leave your room first, then you can watch a friend's game.", 8.0); return; }
    if (A.st.name.empty()) { set_notice("Pick a name first (Settings), then you can watch.", 8.0); return; }
    const hub::GameEntry* g = find_room_entry(game_id);
    if (!g || is_remote_stand_in(*g)) { set_notice("You need your own copy of " + (title.empty() ? std::string("this game") : title) + " to watch it.", 8.0); return; }
    if (!spectate_supported(*g)) { set_notice("Watching isn't available for " + g->title + " yet.", 8.0); return; }
    SP.code = code; SP.game_id = game_id; SP.title = title; SP.friend_name = friend_name;
    if (!SP.lc.start_watch(server_url(), code, A.st.name)) { set_notice("Couldn't reach the server.", 6.0); return; }
    SP.active = true;
    SP.since = ImGui::GetTime();
    set_notice("Joining " + friend_name + "'s game as a spectator...", 6.0);
}

void spectate_tick() {
    if (!SP.active) return;
    SP.lc.update();
    if (SP.lc.failed()) {
        std::string why = SP.lc.status();
        spec_stop();
        set_notice("Can't watch right now: " + why, 10.0);
        return;
    }
    if (!SP.lc.connected()) {
        if (ImGui::GetTime() - SP.since > 40.0) { spec_stop(); set_notice("Couldn't connect to the room.", 8.0); }
        return;
    }
    hubnet::LobbyEvent e;
    while (SP.lc.poll_event(&e)) {
        if (e.type != hubnet::LobbyEvent::Msg || e.from_slot != 0) continue;
        hubui::Json j;
        if (!json::parse(e.body, &j) || j.type != hubui::Json::Object || j.str("kind") != "launch") continue;
        const hub::GameEntry* g = find_room_entry(j.str("game"));
        if (!g || is_remote_stand_in(*g)) { spec_stop(); set_notice("You don't have the game " + SP.friend_name + " is playing.", 8.0); return; }
        if (!spectate_supported(*g)) { spec_stop(); set_notice("Watching isn't available for " + g->title + " yet.", 8.0); return; }
        const hubui::Json* ss = j.get("share_save");
        std::string ports; int players = 0;
        if (A.proc) { spec_stop(); set_notice("A game is already running on this PC.", 6.0); return; }
        if (ss && ss->type == hubui::Json::Bool && ss->b) { spec_stop(); set_notice("This game shares the host's save, so it can't be watched.", 8.0); return; }
        if (!hubui::parse_launch_plan(j, &ports, &players)) { spec_stop(); set_notice(SP.friend_name + "'s RetroHub is too old to be watched - ask them to update.", 8.0); return; }
        if (!hubui::watch_joins_live(g->system == hub::System::N64)) set_notice("Watching " + SP.friend_name + "." + std::string(hubui::watch_join_note(true, true)), 10.0);
        g_spectate_launch = true;
        g_spectate_code = SP.code;
        launch_online(*g, (int)j.num("launch", 1), 0, false, j.str("core"), false, nullptr, &j, nullptr, &e.body);
        g_spectate_launch = false;
        g_spectate_code.clear();
        spec_stop();   // the runner holds its own seat in the game room; this lobby seat is no longer needed
        return;
    }
    if (ImGui::GetTime() - SP.since > 1800.0) { spec_stop(); set_notice("Nothing started to watch.", 8.0); }
}
