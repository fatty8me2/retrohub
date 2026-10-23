#include "app.h"

bool g_fake_room = false;
bool g_fake_other_host = false;   // HUB_SHOT_HOSTCHANGE=other: Mia (slot 0) is the host, we are slot 1

/* ---------- lobby ---------- */

/* Room accessors: screenshots (HUB_SHOT_FAKE_ROOM) fake a connected room without touching the network. */
bool room_connected() { return g_fake_room || A.lobby.connected(); }
std::vector<hubnet::Member> room_members() {
    if (g_fake_room && g_fake_other_host) return {{0, "Mia"}, {1, A.st.name}, {2, "Jake"}};
    if (g_fake_room) return {{0, A.st.name}, {1, "Mia"}, {2, "Jake"}};
    return A.lobby.members();
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

void leave_room() {
    A.lobby.stop();
    A.in_room = false;
    A.peers.clear();
    A.ready.clear();
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

void enter_room(bool host, const std::string& code) {
    if (A.st.name.empty()) { set_notice("Pick a player name in Settings first."); return; }
    if (g_headless) { set_notice("Headless test run: online skipped."); return; }
    if (A.proc) { set_notice("Close the running game first."); return; }
    leave_room();
    A.code = code;
    if (!A.lobby.start(server_url(), code, host, A.st.name)) {
        set_notice("Could not connect to the lobby server.");
        return;
    }
    A.in_room = true;
    A.page = Page::Online;
    if (host && !A.pending_room_game.empty() && find_room_entry(A.pending_room_game)) A.selected_game = A.pending_room_game;
    else if (host && !A.st.last_game.empty() && find_room_entry(A.st.last_game)) A.selected_game = A.st.last_game;
    A.pending_room_game.clear();
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
            if (g_fake_room && m.slot == 1 && std::getenv("HUB_SHOT_FAKE_APP")) p.app = std::getenv("HUB_SHOT_FAKE_APP");   // screenshot aid: fake version mismatch (e.g. 1.9.1)
        }
        p.slot = m.slot;
        p.name = m.name;
        v.push_back(p);
    }
    return v;
}

void lobby_tick() {
    if (!A.in_room) return;
    A.lobby.update();
    if (!A.lobby.connected()) return;

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
            hubui::remap_slot_key(A.peers, ev.from_slot, 0);
            hubui::remap_slot_key(A.ready, ev.from_slot, 0);
            hubui::remap_slot_key(A.locals, ev.from_slot, 0);
            hubui::remap_slot_key(A.picks, ev.from_slot, 0);
            A.hello_dirty = true;
            bool me = A.lobby.my_slot() == 0;
            A.chat.push_back({"", hubui::host_changed_text(ev.name, me)});
            A.chat_scroll = true;
            push_toast(hubui::host_changed_text(ev.name, me), ui::GOOD, 8.0, /*top=*/true);
            continue;
        }
        int from = ev.from_slot;
        const std::string& body = ev.body;
        bool before_hc = last_hc != evs.size() && k < last_hc;
        hubui::Json j;
        if (!hubui::json_parse(body, &j) || j.type != hubui::Json::Obj) continue;
        if (before_hc && j.str("kind") == "launch") continue;
        std::string kind = j.str("kind");
        if (kind == "hello") {
            hubui::PeerInfo& p = A.peers[from];
            hubui::parse_hello(j, &p);
        } else if (kind == "select") {
            if (from == 0) A.selected_game = j.str("game");
        } else if (kind == "ready") {
            const hubui::Json* r = j.get("ready");
            A.ready[from] = r && r->type == hubui::Json::Bool && r->b;
        } else if (kind == "locals") {
            A.locals[from] = std::max(1, std::min(2, (int)j.num("n", 1)));
        } else if (kind == "opts") {
            const hubui::Json* r = j.get("share_save");
            if (from == 0 && r && r->type == hubui::Json::Bool) A.share_host_save = r->b;
        } else if (kind == "pick") {
            hubui::MemberPick mp;
            if (from != room_my_slot() && hubui::parse_pick(j, &mp)) A.picks[from] = mp;
        } else if (kind == "rules") {
            hubui::MatchRules mr;
            if (from == 0 && !is_host() && hubui::parse_rules(j, &mr)) A.rules = mr;
        } else if (kind == "np_gc_ready") {
            if (from == 0 && !is_host() && GCO.guest_wait) GCO.host_ready = true;
        } else if (kind == "chat") {
            A.chat.push_back({member_name(from), j.str("text")});
            if (A.chat.size() > 200) A.chat.erase(A.chat.begin());
            A.chat_scroll = true;
        } else if (kind == "launch") {
            if (from != 0 || is_host()) continue;
            const hub::GameEntry* g = find_room_entry(j.str("game"));
            if (!g) { set_notice("The host started a game you don't have."); continue; }
            if (A.proc) continue;
            if (g->system == hub::System::GC) {
                gcnp::Spec gs;
                if (!gcnp::spec_parse(gcnp::extract_from_launch(body), &gs)) { set_notice("The host's GameCube launch is invalid - ask them to update RetroHub."); continue; }
                gc_guest_prepare(*g, gs, (int)j.num("launch", 1));
                continue;
            }
            const hubui::Json* ss = j.get("share_save");
            launch_online(*g, (int)j.num("launch", 1), (int)j.num("delay", 0), false, j.str("core"),
                          ss && ss->type == hubui::Json::Bool && ss->b, nullptr, &j);   // --match comes from j["match"]
        }

    }

    gc_tick();
    gc_guest_poll();
    // Membership changes -> forget departed peers, re-announce ourselves.
    std::vector<int> slots;
    for (auto& m : A.lobby.members()) slots.push_back(m.slot);
    std::sort(slots.begin(), slots.end());
    bool changed = !A.was_connected || slots != A.last_slots;
    if (changed) {
        std::set<int> alive(slots.begin(), slots.end());
        for (auto it = A.peers.begin(); it != A.peers.end();) it = alive.count(it->first) ? std::next(it) : A.peers.erase(it);
        for (auto it = A.ready.begin(); it != A.ready.end();) it = alive.count(it->first) ? std::next(it) : A.ready.erase(it);
        for (auto it = A.locals.begin(); it != A.locals.end();) it = alive.count(it->first) ? std::next(it) : A.locals.erase(it);
        for (auto it = A.picks.begin(); it != A.picks.end();) it = alive.count(it->first) ? std::next(it) : A.picks.erase(it);
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
            A.lobby.send_msg("{\"kind\":\"select\",\"game\":\"" + hubui::json_escape(A.selected_game) + "\"}");
    }

}

