/* RETROHUB_AUTOTEST: drives RetroHub's real room flow without a person (QA only, never set in normal use).
 *   RETROHUB_AUTOTEST=host|join   RETROHUB_AUTOTEST_CODE=<room>   RETROHUB_AUTOTEST_GAME=<room game id> (host)
 *   RETROHUB_AUTOTEST_PLAYERS=<n> (host: launch once this many are in the room and ready)   RETROHUB_AUTOTEST_WRITE=1 (host keeps card)
 * It calls enter_room / the ready flag / host_launch_selected(), i.e. the same code the buttons run, so launch_online and the
 * Dolphin NetPlay driver are exercised end to end. Progress goes to <data>\autotest.log. */
#include "app.h"

static const char* env(const char* k) { const char* v = std::getenv(k); return v && *v ? v : nullptr; }

bool autotest_active() { static bool on = env("RETROHUB_AUTOTEST") != nullptr; return on; }

void autotest_log(const std::string& line) {
    if (!autotest_active()) return;
    static std::string path = hub::data_dir() + "autotest.log";
    std::ofstream f(path, std::ios::app | std::ios::binary);
    f << line << "\n";
}

void autotest_tick() {
    if (!autotest_active()) return;
    static int stage = 0;
    static std::string seen_status;
    static double t0 = 0;
    static std::string role = env("RETROHUB_AUTOTEST");
    static ULONGLONG started = GetTickCount64();
    ULONGLONG now = GetTickCount64();
    (void)t0;
    if (GCO.active) {   // mirror the Dolphin driver's status lines into our log
        std::string st = gcnp::read_status(GCO.run);
        if (st != seen_status) {
            std::istringstream a(seen_status), b(st);
            std::string l;
            size_t n = std::count(seen_status.begin(), seen_status.end(), '\n');
            size_t i = 0;
            while (std::getline(b, l)) if (i++ >= n) autotest_log("driver " + l);
            seen_status = st;
        }
    }
    if (stage == 0) {
        if (A.scanning || now - started < 3000) return;
        autotest_log("lib " + std::to_string(A.lib.size()) + " games");
        for (auto& e : A.lib) autotest_log("game " + room_id(e) + " | " + e.title + " | " + e.path);
        A.st.name = env("RETROHUB_AUTOTEST_NAME") ? env("RETROHUB_AUTOTEST_NAME") : (role == "host" ? "AtHost" : "AtGuest");
        if (env("RETROHUB_AUTOTEST_WRITE")) A.gc_write_save = true;
        if (role == "host") A.pending_room_game = env("RETROHUB_AUTOTEST_GAME") ? env("RETROHUB_AUTOTEST_GAME") : "";
        enter_room(role == "host", env("RETROHUB_AUTOTEST_CODE") ? env("RETROHUB_AUTOTEST_CODE") : "ATGC01");
        stage = 1;
        started = now;
        return;
    }
    if (stage == 1) {
        if (!room_connected()) { if (now - started > 30000) { autotest_log("error:room connect timeout"); stage = 99; } return; }
        autotest_log("connected slot " + std::to_string(room_my_slot()));
        if (role == "host") {
            if (env("RETROHUB_AUTOTEST_GAME")) {
                A.selected_game = env("RETROHUB_AUTOTEST_GAME");
                A.lobby.send_msg("{\"kind\":\"select\",\"game\":\"" + json::escape(A.selected_game) + "\"}");
            }
        } else {
            A.my_ready = true;
            A.hello_dirty = true;
            if (env("RETROHUB_AUTOTEST_GAME")) A.my_ready = false;   // guests turn ready once the selected archive is unpacked (stage 2)
        }
        stage = 2;
        started = now;
        return;
    }
    if (stage == 2 && role != "host" && !A.my_ready) {
        const hub::GameEntry* rg = A.selected_game.empty() ? nullptr : find_room_entry(A.selected_game);
        if (rg && !((rg->system == hub::System::GC || rg->system == hub::System::N3DS) && gc_needs_unpack(*rg))) { A.my_ready = true; A.hello_dirty = true; autotest_log("guest ready"); }
        return;
    }
    if (stage == 2 && role == "host") {
        int want = env("RETROHUB_AUTOTEST_PLAYERS") ? std::atoi(env("RETROHUB_AUTOTEST_PLAYERS")) : 2;
        const hub::GameEntry* rg = find_room_entry(A.selected_game);
        if (!rg) { if (now - started > 20000) { autotest_log("error:host game not in library"); stage = 99; } return; }
        if ((int)room_members().size() < want) return;
        std::vector<hubui::PeerInfo> peers = peer_list();
        auto s = hubui::game_selectable(A.selected_game, max_players_for(*rg), peers, 0);
        if (!s.ok) { if (now - started > 60000) { autotest_log("error:not selectable: " + s.reason); stage = 99; } return; }
        for (auto& p : peers) if (p.slot != 0 && !A.ready[p.slot]) return;
        if ((rg->system == hub::System::GC || rg->system == hub::System::N3DS) && gc_needs_unpack(*rg)) return;
        if (rg->system == hub::System::GC && !gc_fingerprint_ready(launch_path_for(*rg))) return;   // np_gc then carries the disc sha
        autotest_log("launching " + rg->title + " with " + std::to_string(room_members().size()) + " players");
        host_launch_selected();
        stage = 3;
    }
}
