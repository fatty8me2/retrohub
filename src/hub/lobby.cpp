#include "app.h"

bool g_fake_room = false;
bool g_shot_rejoin = false;
std::string g_shot_rejoin_why;
bool g_fake_other_host = false;   // HUB_SHOT_HOSTCHANGE=other: Mia (slot 0) is the host, we are slot 1

/* ---------- lobby ---------- */

/* Room accessors: screenshots (HUB_SHOT_FAKE_ROOM) fake a connected room without touching the network. */
bool room_connected() { return g_fake_room || A.lobby.connected(); }
std::vector<hubnet::Member> room_members() {
    if (g_fake_room && std::getenv("HUB_SHOT_ROOM_N")) {   // screenshot aid: 2-4 members; guest view (HUB_SHOT_HOSTCHANGE=other) has Mia as host
        int n = std::max(2, std::min(4, std::atoi(std::getenv("HUB_SHOT_ROOM_N"))));
        std::vector<hubnet::Member> v;
        if (g_fake_other_host) v = {{0, "Mia"}, {1, A.st.name}, {2, "Jake"}, {3, "Kai"}};
        else v = {{0, A.st.name}, {1, "Mia"}, {2, "Jake"}, {3, "Kai"}};
        v.resize(n);
        return v;
    }
    if (g_fake_room && g_fake_other_host) return {{0, "Mia"}, {1, A.st.name}, {2, "Jake"}};
    if (g_fake_room && std::getenv("HUB_SHOT_KT")) return {{0, A.st.name}, {1, "Mia"}};   // Kanto Together screenshots: exactly 2 players
    if (g_fake_room) return {{0, A.st.name}, {1, "Mia"}, {2, "Jake"}};
    return A.lobby.members();
}
/* Room latency, measured only while the room page is open and no game runs, every 8 s: one tiny broadcast per member (the others answer).
 * Server cost is a handful of WebSocket messages per room per minute and nothing per frame. */
void room_ping_tick() {
    static ULONGLONG last = 0;
    if (g_fake_room || !A.lobby.connected() || A.proc || A.page != Page::Online || A.lobby.members().size() < 2) return;
    ULONGLONG t = GetTickCount64();
    if (t - last < 8000) return;
    last = t;
    const char* fr = std::getenv("HUBNET_FORCE_RELAY");
    A.lobby.send_msg("{\"kind\":\"rping\",\"t\":" + std::to_string((unsigned long long)(t & 0xFFFFFFFFu)) + ",\"relay\":" + (fr && fr[0] && fr[0] != '0' ? "1" : "0") + "}");
}
bool room_ping_of(int slot, int* rtt_ms, bool* relay) {
    if (slot == room_my_slot()) return false;
    if (g_fake_room) {   // screenshots: 1 = great, 2 = ok-ish, 3 = relay (HUB_SHOT_FAKE_PING=slot:ms,...,slot:relay overrides)
        static const int def[4] = {24, 38, 132, 0};
        int ms = slot >= 0 && slot < 4 ? def[slot] : 50;
        bool rl = slot == 3;
        if (const char* e = std::getenv("HUB_SHOT_FAKE_PING")) {
            std::string s = e;
            std::string key = std::to_string(slot) + ":";
            size_t p = s.find(key);
            if (p != std::string::npos && (p == 0 || s[p - 1] == ',')) {
                std::string v = s.substr(p + key.size(), s.find(',', p) == std::string::npos ? std::string::npos : s.find(',', p) - p - key.size());
                rl = v == "relay";
                ms = rl ? 0 : std::atoi(v.c_str());
            }
        }
        *rtt_ms = ms; *relay = rl;
        return true;
    }
    auto at = A.ping_at.find(slot);
    bool fresh = at != A.ping_at.end() && wall_ms() - at->second < 25000;
    auto rl = A.ping_relay.find(slot);
    *relay = rl != A.ping_relay.end() && rl->second;
    *rtt_ms = fresh ? A.pings.rtt(slot) : -1;
    return fresh || *relay;
}
int room_my_slot() { return g_fake_room ? (g_fake_other_host ? 1 : 0) : A.lobby.my_slot(); }
int locals_of(int slot) {
    if (slot == room_my_slot()) return A.my_locals;
    auto it = A.locals.find(slot);
    return it == A.locals.end() ? 1 : std::max(1, std::min(2, it->second));
}
std::vector<hubui::MemberPads> member_pads() {
    std::vector<hubui::MemberPads> v;
    for (auto& m : room_members()) v.push_back({m.slot, locals_of(m.slot)});
    return v;
}

std::string member_name(int slot) {
    for (auto& m : room_members()) if (m.slot == slot) return m.name;
    return "Player " + std::to_string(slot + 1);
}

/* ---------- Smash match picker (room state; the panel itself is drawn by match_panel) ---------- */
bool match_active() { return hubui::match_game_supported(A.selected_game); }
bool match_remix_roster() { return A.selected_game == "remix"; }   // "ssb64rom" will use the 12-fighter vanilla roster
std::map<int, hubui::MemberPick> all_picks() {
    std::map<int, hubui::MemberPick> m = A.picks;
    m[room_my_slot()] = A.my_pick;
    return m;
}
void send_my_pick() { A.lobby.send_msg(hubui::pick_json(A.my_pick)); }
void send_rules() { A.lobby.send_msg(hubui::rules_json(A.rules)); }

/* A game from this room ended (or never started): nobody stays "ready" for the next one, so the host cannot relaunch a rematch
 * before the others are back, and the old ready flags are not mistaken for fresh ones. Everyone re-announces their own state. */
void room_session_ended() {
    if (!A.in_room) return;
    A.my_ready = false;
    A.ready.clear();
    A.hello_dirty = true;
}

void leave_room() {
    online_cancel_pending();
    /* Leaving the room ends our online game too: Smash's lobby is part of this room, so its player has to leave with us.
     * (Only our own online child process; emulator sessions are not touched.) */
    if (A.proc && A.run.online && !A.run.after_exit) TerminateProcess(A.proc, 0);
    kt_leave_room();
    group_left_room();
    A.lobby.stop();
    A.in_room = false;
    A.peers.clear();
    A.ready.clear();
    A.ingame.clear();
    A.prev_proc = false;
    A.last_slots.clear();
    A.chat.clear();
    A.selected_game.clear();
    A.my_ready = false;
    A.locals.clear();
    A.my_locals = 1;
    A.picks.clear();
    A.my_pick = hubui::MemberPick();
    A.rules = hubui::MatchRules();
    A.pick_edit = 0;
    A.share_host_save = true;
    A.was_connected = false;
    A.code.clear();
}

void enter_room(bool host, const std::string& code_in) {
    const std::string code = code_in;   // may alias A.code, which leave_room() clears
    if (A.st.name.empty()) { set_notice("Pick a player name in Settings first."); return; }
    if (g_headless) { set_notice("Headless test run: online skipped."); return; }
    if (A.proc) { set_notice("Close the running game first."); return; }
    /* Remember the room we are in: the single lobby socket has to be closed to open the new one, so if the new room
     * cannot be entered (wrong code, full, server down) lobby_tick puts the player back into the old one. */
    std::string pc = A.code;
    bool ph = A.room_role_host, had_room = A.in_room && A.lobby.connected() && !pc.empty() && pc != code;
    leave_room();
    A.prev_code = had_room ? pc : std::string();
    A.prev_host = ph;
    A.code = code;
    A.room_role_host = host;
    A.entering_room = true;
    A.lobby.set_ident_msg(A.st.auto_friend ? fr_lobby_ident_msg() : std::string());   // toggle off: stay anonymous in the room
    if (!A.lobby.start(server_url(), code, host, A.st.name)) {
        A.entering_room = false;
        set_notice("Could not connect to the lobby server.");
        if (!A.prev_code.empty()) { std::string c = A.prev_code; bool h = A.prev_host; A.prev_code.clear(); enter_room(h, c); }
        return;
    }
    A.in_room = true;
    A.page = Page::Online;
    if (host && !A.pending_room_game.empty() && find_room_entry(A.pending_room_game)) A.selected_game = A.pending_room_game;
    else if (host && !A.st.last_game.empty() && find_room_entry(A.st.last_game)) A.selected_game = A.st.last_game;
    A.pending_room_game.clear();
}

void rejoin_room() {
    if (A.code.empty()) return;
    enter_room(A.room_role_host, A.code);
}

void launch_fail(const std::string& why, double secs) {
    set_notice(why, secs);
    if (room_connected() && !is_host())
        A.lobby.send_msg("{\"kind\":\"np_fail\",\"reason\":\"" + json::escape(why) + "\"}");
}

std::string ready_block_reason_now() {
    if (is_host()) return "";
    std::string host_app;
    bool host_hello = false;
    for (auto& p : peer_list()) if (p.slot == 0) { host_app = p.app; host_hello = p.hello; }
    return hubui::ready_block_reason(HUB_VERSION_STR, host_app, host_hello, allow_version_mismatch());
}

bool is_host() { return room_connected() && room_my_slot() == 0; }

bool allow_version_mismatch() {   // dev override
    const char* e = std::getenv("RETROHUB_ALLOW_VERSION_MISMATCH");
    return e && *e && std::string(e) != "0";
}

std::vector<hubui::PeerInfo> peer_list() {
    std::vector<hubui::PeerInfo> v;
    int me = room_my_slot();
    for (auto& m : room_members()) {
        hubui::PeerInfo p;
        auto it = A.peers.find(m.slot);
        if (it != A.peers.end()) p = it->second;
        if (m.slot == me || g_fake_room) {
            p.hello = true; p.games.clear(); p.keys.clear(); my_games(&p.games, &p.keys);
            p.app = HUB_VERSION_STR;
            if (g_fake_room && m.slot == 1 && std::getenv("HUB_SHOT_PEER_MISSING")) {   // screenshot aid: peer 1 lacks every Nth game (picker "missing" state)
                int n = std::max(2, std::atoi(std::getenv("HUB_SHOT_PEER_MISSING")));
                std::vector<std::string> kept;
                for (size_t ki = 0; ki < p.keys.size(); ++ki) if ((int)(ki % n) != 0) kept.push_back(p.keys[ki]);
                p.keys.swap(kept); p.games.clear();
            }
            if (g_fake_room && m.slot == 1 && std::getenv("HUB_SHOT_FAKE_APP")) p.app = std::getenv("HUB_SHOT_FAKE_APP");   // screenshot aid: fake version mismatch (e.g. 1.9.1)
        }
        p.slot = m.slot;
        p.name = m.name;
        v.push_back(p);
    }
    return v;
}

/* A guest handles the host's launch message (also driven headless by HUB_SHOT_GUEST_MSG). */
void guest_apply_launch(const std::string& body) {
    hubui::Json j;
    if (!json::parse(body, &j) || j.type != hubui::Json::Object) return;
    const hub::GameEntry* g = find_room_entry(j.str("game"));
    if (!g) { launch_fail("The host started a game you don't have."); return; }
    if (A.proc) { launch_fail("The host started a game, but a game is already running on this PC."); return; }
    if (g->system == hub::System::GC) {
        gcnp::Spec gs;
        if (!gcnp::spec_parse(gcnp::extract_from_launch(body), &gs)) { launch_fail("The host's GameCube launch is invalid - ask them to update RetroHub."); return; }
        if (g_headless) { shot_trace("guest-route", "gc dolphin-netplay-guest"); return; }
        GCO.launch_body = body;
        gc_guest_prepare(*g, gs, (int)j.num("launch", 1));
        return;
    }
    const hubui::Json* ss = j.get("share_save");
    launch_online(*g, (int)j.num("launch", 1), (int)j.num("delay", 0), false, j.str("core"),
                  ss && ss->type == hubui::Json::Bool && ss->b, nullptr, &j, nullptr, &body);   // --match comes from j["match"]
}

void lobby_tick() {
    if (!A.in_room) return;
    A.lobby.update();
    if (A.entering_room) {
        if (A.lobby.connected()) { A.entering_room = false; A.prev_code.clear(); }
        else if (A.lobby.failed()) {
            A.entering_room = false;
            if (!A.prev_code.empty()) {   // the new room did not work out: go back to the one the player left
                std::string c = A.prev_code, why = A.lobby.status();
                bool h = A.prev_host;
                A.prev_code.clear();
                set_notice(why + " - going back to your previous room.");
                enter_room(h, c);
                return;
            }
        }
    }
    if (!A.lobby.connected()) { A.was_connected = false; return; }   // dropped: re-announce ourselves when the link comes back
    /* A guest on a different RetroHub version must not stay readied up. */
    if (A.my_ready && !ready_block_reason_now().empty()) { A.my_ready = false; A.hello_dirty = true; }

    /* Lobby events are handled strictly in arrival order. A HostChange moves per-slot state from the promoted
     * member's old slot to 0 and drops the departed host's, so every message queued before it (old numbering)
     * is applied first and nothing from before the change can recreate state under a stale slot. A `launch`
     * that arrived in the same batch before the HostChange came from the departed host: it is dropped (its
     * port plan uses the old slots); the remaining players just stay in the room under the new host. */
    std::vector<hubnet::LobbyEvent> evs;
    {
        hubnet::LobbyEvent e;
        while (A.lobby.poll_event(&e)) evs.push_back(std::move(e));   // poll applies the slot change when it pops a HostChange
    }
    size_t last_hc = evs.size();
    for (size_t k = 0; k < evs.size(); ++k) if (evs[k].type == hubnet::LobbyEvent::HostChange) last_hc = k;
    for (size_t k = 0; k < evs.size(); ++k) {
        const hubnet::LobbyEvent& ev = evs[k];
        if (ev.type == hubnet::LobbyEvent::HostChange) {
            std::string gone;   // the departed host's name, before the maps are renumbered
            { auto pit = A.peers.find(0); if (pit != A.peers.end()) gone = pit->second.name; }
            hubui::host_left_remap(ev.from_slot, A.peers, A.ready, A.ingame, A.locals, A.picks);
            A.pings.clear(); A.ping_at.clear(); A.ping_relay.clear();     // slots were renumbered: re-measure
            A.ready.clear();                                              // every ready flag belonged to the old host's game: all re-announce
            online_host_left(gone);
            A.hello_dirty = true;
            if (!A.proc) { online_cancel_pending(); kt_leave_room(); }   // a launch the departed host was preparing is void
            A.my_ready = false;                                           // the new host-to-be picks again; nobody is stuck "ready" for the old host's game
            bool me = A.lobby.my_slot() == 0;
            if (me) A.room_role_host = true;
            A.chat.push_back({"", hubui::host_changed_text(ev.name, me)});
            A.chat_scroll = true;
            push_toast(hubui::host_changed_text(ev.name, me), ui::GOOD, 8.0, /*top=*/true);
            continue;
        }
        int from = ev.from_slot;
        const std::string& body = ev.body;
        bool before_hc = last_hc != evs.size() && k < last_hc;
        hubui::Json j;
        if (!json::parse(body, &j) || j.type != hubui::Json::Object) continue;
        if (before_hc && j.str("kind") == "launch") {
            set_notice("The host changed while the game was starting - ask the new host to launch again.");
            continue;
        }
        std::string kind = j.str("kind");
        if (kind == "hello") {
            hubui::PeerInfo& p = A.peers[from];
            hubui::parse_hello(j, &p);
        } else if (kind == "select") {
            if (from == 0) A.selected_game = j.str("game");
        } else if (kind == "ready") {
            const hubui::Json* r = j.get("ready");
            A.ready[from] = r && r->type == hubui::Json::Bool && r->b;
        } else if (kind == "ingame") {
            const hubui::Json* r = j.get("v");
            A.ingame[from] = r && r->type == hubui::Json::Bool && r->b;
        } else if (kind == "locals") {
            A.locals[from] = std::max(1, std::min(2, (int)j.num("n", 1)));
        } else if (kind == "opts") {
            const hubui::Json* r = j.get("share_save");
            if (from == 0 && r && r->type == hubui::Json::Bool) A.share_host_save = r->b;
        } else if (kind == "pick") {
            hubui::MemberPick mp;
            if (from != room_my_slot() && hubui::parse_pick(j, &mp)) A.picks[from] = mp;
        } else if (kind == "cheats") {
            if (from == 0 && !is_host()) {
                std::vector<std::string> names;
                if (const hubui::Json* a = j.get("names")) for (auto& x : a->a) if (x.type == hubui::Json::String) names.push_back(x.s);
                cheats_on_room_msg(j.str("game"), names);
            }
        } else if (kind == "rules") {
            hubui::MatchRules mr;
            if (from == 0 && !is_host() && hubui::parse_rules(j, &mr)) A.rules = mr;
        } else if (kind == "np_abort") {   // the host could not start the game it announced: stop waiting for it
            if (from == 0 && !is_host()) {
                online_cancel_pending();
                set_notice("The host couldn't start the game" + (j.str("reason").empty() ? std::string(".") : ": " + j.str("reason")), 10.0);
            }
        } else if (kind == "np_fail") {
            if (is_host()) set_notice(member_name(from) + " can't start: " + j.str("reason"), 12.0);
        } else if (kind == "np_gc_ready") {
            autotest_log("np_gc_ready from " + std::to_string(from) + " guest_wait=" + std::to_string((int)GCO.guest_wait));
            if (from == 0 && !is_host() && GCO.guest_wait) GCO.host_ready = true;
        } else if (kind == "np_kt") {
            if (from == 0 && !is_host()) kt_on_code(j.str("code"), (int)j.num("launch", 0), j.str("pkg"));
        } else if (kind == "np_kt_ver") {
            kt_on_ver(from, j.str("id"), j.str("built"));
        } else if (kind == "np_kt_fail") {
            kt_on_peer_fail(from, j.str("reason"));
        } else if (kind == "np_kt_req") {
            kt_on_request(from);
        } else if (kind == "rping") {   // room latency probe: answer it (the asker times the round trip)
            A.ping_relay[from] = j.num("relay", 0) != 0;
            A.lobby.send_msg("{\"kind\":\"rpong\",\"to\":" + std::to_string(from) + ",\"t\":" + std::to_string((long long)j.num("t", 0)) + "}");
        } else if (kind == "rpong") {
            if ((int)j.num("to", -1) == room_my_slot()) {
                long long rtt = (long long)(uint32_t)(GetTickCount64() & 0xFFFFFFFFu) - (long long)j.num("t", 0);
                if (rtt < 0) rtt += 4294967296LL;
                if (rtt >= 0 && rtt < 10000) { A.pings.add(from, (int)rtt); A.ping_at[from] = wall_ms(); }
            }
        } else if (kind == "chat") {
            A.chat.push_back({member_name(from), j.str("text")});
            if (A.chat.size() > 200) A.chat.erase(A.chat.begin());
            A.chat_scroll = true;
        } else if (kind == "launch") {
            if (from != 0 || is_host()) continue;
            guest_apply_launch(body);
        }

    }

    /* Tell the room when our game starts / ends: a player who quit the game is back in the room (not in game) and
     * has to ready up again. Their game's own lobby (Smash) has already dropped them by then. */
    if ((A.proc != nullptr) != A.prev_proc) {
        A.prev_proc = A.proc != nullptr;
        A.ingame[room_my_slot()] = A.prev_proc;
        A.lobby.send_msg(std::string("{\"kind\":\"ingame\",\"v\":") + (A.prev_proc ? "true" : "false") + "}");
        if (!A.prev_proc && A.my_ready) { A.my_ready = false; A.hello_dirty = true; }
    }
    gc_tick();
    gc_guest_poll();
    room_ping_tick();
    // Membership changes -> forget departed peers, re-announce ourselves.
    std::vector<int> slots;
    for (auto& m : A.lobby.members()) slots.push_back(m.slot);
    std::sort(slots.begin(), slots.end());
    bool changed = !A.was_connected || slots != A.last_slots;
    if (changed) {
        std::set<int> alive(slots.begin(), slots.end());
        if (A.proc && A.run.room && A.run.left_name.empty() && A.was_connected)   // a player vanished from the room during the game: the exit notice names them
            for (int s : A.last_slots)
                if (!alive.count(s)) { auto pit = A.peers.find(s); A.run.left_name = pit != A.peers.end() && !pit->second.name.empty() ? pit->second.name : std::string("A player"); if (s != 0) push_toast(A.run.left_name + " disconnected (the game plays on if it can)", ui::WARN, 6.0); break; }
        for (auto it = A.peers.begin(); it != A.peers.end();) it = alive.count(it->first) ? std::next(it) : A.peers.erase(it);
        for (auto it = A.ready.begin(); it != A.ready.end();) it = alive.count(it->first) ? std::next(it) : A.ready.erase(it);
        for (auto it = A.ingame.begin(); it != A.ingame.end();) it = alive.count(it->first) ? std::next(it) : A.ingame.erase(it);
        for (auto it = A.locals.begin(); it != A.locals.end();) it = alive.count(it->first) ? std::next(it) : A.locals.erase(it);
        for (auto it = A.picks.begin(); it != A.picks.end();) it = alive.count(it->first) ? std::next(it) : A.picks.erase(it);
        for (int s = 0; s < 4; ++s) if (!alive.count(s)) { A.pings.drop(s); A.ping_at.erase(s); A.ping_relay.erase(s); }
        A.last_slots = slots;
        A.hello_dirty = true;
        A.was_connected = true;
    }
    if (A.hello_dirty) {
        A.hello_dirty = false;
        {
            std::map<std::string, std::string> games;
            std::vector<std::string> keys;
            my_games(&games, &keys);
            for (auto& m : hubui::hello_messages("2", games, keys, 3000, HUB_VERSION_STR)) A.lobby.send_msg(m);
        }
        A.lobby.send_msg(std::string("{\"kind\":\"ready\",\"ready\":") + (A.my_ready ? "true" : "false") + "}");
        A.lobby.send_msg("{\"kind\":\"locals\",\"n\":" + std::to_string(A.my_locals) + "}");
        if (is_host()) A.lobby.send_msg(std::string("{\"kind\":\"opts\",\"share_save\":") + (A.share_host_save ? "true" : "false") + "}");
        A.lobby.send_msg(hubui::pick_json(A.my_pick));
        if (is_host()) A.lobby.send_msg(hubui::rules_json(A.rules));
        if (is_host() && !A.selected_game.empty())
            A.lobby.send_msg("{\"kind\":\"select\",\"game\":\"" + json::escape(A.selected_game) + "\"}");
    }

}

