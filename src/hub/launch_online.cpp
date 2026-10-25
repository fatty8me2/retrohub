#include "netplay_3ds_join.h"
#include "app.h"
#include <filesystem>

GcOnline GCO;
static hubnp3ds::ResumeGate NP3_resume;   // guest launch waiting for the archive to finish unpacking

bool gc_needs_unpack(const hub::GameEntry& e) { return e.issue == "extract" && hub::extracted_3ds_path(e).empty(); }

/* One wording for "the archive is still being unpacked", everywhere. */
static std::string unpack_msg(const hub::GameEntry& e) { return "Unpacking " + e.title + " first - try again when it is done."; }
/* A launch problem: the host just sees the notice, a guest also tells the host why it cannot start (np_fail). */
/* A host failure after the launch message went out also tells the guests (np_abort), or they would wait for a game that never comes. */
static void host_abort(const std::string& why) {
    if (room_connected() && !g_fake_room) A.lobby.send_msg("{\"kind\":\"np_abort\",\"reason\":\"" + json::escape(why) + "\"}");
}
static void launch_note(bool host, const std::string& m, double secs = 8.0) { if (host) { set_notice(m, secs); host_abort(m); } else launch_fail(m, secs); }
/* spawn(), but a host whose process could not start aborts the guests' wait too. */
static bool spawn_room(bool host, const std::string& exe, const std::string& args, const std::string& cwd,
                       const std::vector<std::pair<std::string, std::string>>& env, const RunInfo& ri) {
    bool ok = spawn(exe, args, cwd, env, ri);
    if (!ok) { if (host) host_abort("the game did not start"); else launch_fail("The game did not start on this PC."); }
    return ok;
}

bool gc_fingerprint_ready(const std::string& path) {
    if (GCO.fp_path == path) return GCO.fp_done;
    if (GCO.fp_th.joinable()) GCO.fp_th.join();
    GCO.fp_path = path;
    GCO.fp_done = false;
    GCO.fp_th = std::thread([path]() {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad);
        GCO.fp_size = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        GCO.fp_sha = hubemu::sha256_file(path);
        GCO.fp_done = true;
    });
    return false;
}

bool gc_process_alive() {
    if (!GCO.run.dolphin_pid) return false;
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, GCO.run.dolphin_pid);
    if (!h) return false;
    bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

void gc_kill() {
    for (DWORD pid : {GCO.run.driver_pid, GCO.run.dolphin_pid}) {
        if (!pid) continue;
        if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid)) { TerminateProcess(h, 0); CloseHandle(h); }
    }
}

/* Drop a launch that is still being prepared (room left, host changed or aborted): GC forwarder + guest wait, 3DS link that
 * has not started Azahar yet, an unpack-then-launch resume. A game that is already running is never touched. */
void online_host_left(const std::string& gone) {
    /* Built-in runners (retrorun / n64run) see the host's socket close and exit 8 by themselves; Smash Classic migrates inside its own
     * lobby. Peer-hosted emulator sessions (Dolphin NetPlay guest, Azahar, Kanto) have lost their server: close them gracefully so the
     * emulator flushes its saves, then poll_process / gc_tick sync the vault and return the player to the room. */
    std::string who = gone.empty() ? std::string("The host") : gone;
    if (GCO.active && !GCO.host && gc_process_alive()) {
        GCO.left_name = who;
        close_pid_windows(GCO.run.dolphin_pid);
        push_toast(who + " left - closing the game", ui::WARN, 8.0, true);
        return;
    }
    if (A.proc && A.run.room) {
        A.run.left_name = who;
        push_toast(who + " left - game ended", ui::WARN, 8.0, true);
        if (A.run.pad_exit && A.run.track) request_game_close();
    }
}

void online_cancel_pending() {
    if (GCO.guest_wait || (GCO.active && !gc_process_alive())) gc_stop_all();
    else if (GCO.fw.active() && !GCO.active) GCO.fw.stop();
    if (NP3_pending && !A.proc) { NP3.stop(); NP3_pending = false; }
    NP3_resume.cancel();
}

/* Called while a GameCube game is selected in the room: unpack an archived copy (existing progress UI) and fingerprint the
 * disc in the background, so Launch never waits. */
void gc_prefetch(const hub::GameEntry& e) {
    if (gc_needs_unpack(e)) {
        if (UNZ.state == UnzipJob::Idle && !UNZ.th.joinable()) { UNZ.key = e.key; UNZ.quiet = true; start_unzip(e); }
        return;
    }
    gc_fingerprint_ready(launch_path_for(e));
}

/* 3DS: an archived (.zip) image must be unpacked before Azahar can load it; do it as soon as the game is selected in the room
 * (existing quiet progress UI), so neither the host's Launch nor a guest's launch handler ever waits on it. */
void n3ds_prefetch(const hub::GameEntry& e) {
    if (gc_needs_unpack(e) && UNZ.state == UnzipJob::Idle && !UNZ.th.joinable()) { UNZ.key = e.key; UNZ.quiet = true; start_unzip(e); }
}

/* Host: the NetPlay spec for the selected game. The sha is included when the background hash is already done. */
bool gc_host_spec(const hub::GameEntry& e, int players, gcnp::Spec* out) {
    if (gc_needs_unpack(e)) { set_notice(unpack_msg(e)); return false; }
    std::string path = launch_path_for(e);
    bool have = gc_fingerprint_ready(path);
    const GameInfo* g = hub::find_game(e.catalog_id);
    gcnp::Spec s;
    s.mode = "direct";
    s.addr = "127.0.0.1";
    if (!GCO.port) GCO.port = 27000 + (int)(std::random_device{}() % 2000);
    s.port = GCO.port;
    s.game_id = !e.serial.empty() ? e.serial : (g && !g->serials.empty() ? g->serials[0] : "");
    s.title = e.title;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad);
    s.size = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    if (have) s.sha256 = GCO.fp_sha;
    s.players = std::max(2, std::min(4, players));
    *out = s;
    if (s.game_id.empty()) { set_notice("Can't identify " + e.title + " (no GameCube disc ID) - re-add the game in your library."); return false; }
    return true;
}

bool gc_begin(const hub::GameEntry& e, bool host, int slot, int port) {
    autotest_log(std::string("gc_begin request ") + (host ? "host" : "guest") + " slot " + std::to_string(slot));
    if (GCO.active && gc_process_alive()) { launch_note(host, "A GameCube online session is already running."); return false; }
    hubemu::Install inst = hubemu::find(hubemu::Kind::Dolphin, "", "");
    if (inst.exe.empty()) { launch_note(host, "Dolphin isn't installed yet - start any GameCube game once so RetroHub can download it."); return false; }
    std::string game = launch_path_for(e);
    if (game.empty() || gc_needs_unpack(e)) { launch_note(host, unpack_msg(e)); return false; }
    /* is_running() matches by image name, so on one PC (RETROHUB_AUTOTEST: several RetroHubs, each with its own Dolphin) it would always refuse the 2nd player. */
    if (!autotest_active() && hubemu::is_running(inst.exe)) { launch_note(host, "Dolphin is already running. Close it first."); return false; }
    if (g_headless) { launch_note(host, "Headless test run: launch skipped."); return false; }
    hubemu::dolphin_migrate_old(hub::data_dir(), hub::exe_dir(), inst.dir);
    { hubctl::Profile pg = controls_profile("gc", e.key); hubemu::prepare_dolphin(inst.dir, dolphin_pads_for(pg), pg); }   // managed config: no update check, no analytics, no panic alerts
    std::string user = inst.dir + "\\User", ini = user + "\\Config\\Dolphin.ini";
    gcnp::Spec sp = GCO.spec;
    sp.mode = "direct";
    sp.addr = "127.0.0.1";
    sp.port = port;
    gcnp::IniParams ip;
    ip.host = host;
    ip.nickname = member_name(room_my_slot());
    ip.iso_dir = dir_of(game);
    ip.write_save = host && A.gc_write_save;
    ip.mute = autotest_active();
    std::string cur;
    { std::ifstream f(ini, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); cur = ss.str(); }
    std::string next = gcnp::ini_patch(cur, sp, ip);
    { std::ofstream f(ini, std::ios::binary | std::ios::trunc); f << next; }
    GCO.run = gcnp::Run();
    /* Memory card: the host plays on (and syncs to everyone) its real vault card, the same folder offline play uses; whether the
     * session's changes stay is Dolphin's WriteSaveData (the host's room choice). Guests get a wiped scratch folder: Dolphin
     * replaces their card with the host's copy in its own Netplay folder, and their real card is never opened. */
    std::string card;
    if (host) card = hubemu::dolphin_card_root(inst.dir, e.key) + "\\GCI";
    else {
        card = no_slash(hub::data_dir()) + "\\netplay\\gc\\guest-card";
        remove_tree(card);
    }
    make_dirs(card);
    std::string extra = std::string("-C Dolphin.Display.Fullscreen=") + (hub::gs::is_fullscreen(gs_values_for(e)) ? "True" : "False") +
                        " -C \"Dolphin.Core.GCIFolderAPathOverride=" + card + "\"";
    if (!gcnp::start(host, inst.exe, user, sp, slot, hub::data_dir() + "\\gcnp", "", extra, &GCO.run)) {
        launch_note(host, "Could not start Dolphin NetPlay: " + GCO.run.error);
        return false;
    }
    GCO.active = true;
    GCO.host = host;
    autotest_log(std::string("gc_begin ") + (host ? "host" : "guest") + " card=" + card);
    GCO.ready_sent = false;
    GCO.entry = e;
    push_toast(host ? "Starting GameCube NetPlay as host..." : "Joining the host's GameCube NetPlay...", ui::GOOD, 6.0);
    return true;
}

void gc_stop_all() {
    if (GCO.run.driver_pid) {   // the UI-automation driver can outlive Dolphin; never leave it behind
        if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, GCO.run.driver_pid)) { TerminateProcess(h, 0); CloseHandle(h); }
        GCO.run.driver_pid = 0;
    }
    GCO.fw.stop();
    GCO.active = false;
    GCO.guest_wait = false;
}

/* Each frame: keep the forwarder alive, tell guests when the host's Dolphin is listening, check the guest's disc. */
void gc_tick() {
    if (GCO.fw.active()) {
        GCO.fw.update();
        static std::string last_fw;
        if (autotest_active() && GCO.fw.status() != last_fw) { last_fw = GCO.fw.status(); autotest_log("fw " + last_fw); }
        if (GCO.fw.failed()) {
            launch_fail("GameCube online: " + GCO.fw.status(), 12.0);
            if (GCO.host) host_abort("GameCube online: " + GCO.fw.status());
            gc_kill();
            gc_stop_all();
            return;
        }
    }
    if (!GCO.active) {
        if (room_connected() && !A.selected_game.empty()) {
            const hub::GameEntry* g = find_room_entry(A.selected_game);
            if (g && g->system == hub::System::GC) gc_prefetch(*g);
            if (g && g->system == hub::System::N3DS) n3ds_prefetch(*g);
        }
        return;
    }
    if (!gc_process_alive()) {   // Dolphin closed: session over. Host: its vault card changed, sync it; everyone: back to un-ready for the next game.
        bool was_host = GCO.host;
        if (!GCO.left_name.empty()) { set_notice(hubui::player_left_text(GCO.left_name), 20.0); GCO.left_name.clear(); }
        gc_stop_all();
        room_session_ended();
        if (was_host) { A.vault_dirty = true; auto_sync(); }
        return;
    }
    if (GCO.host && !GCO.ready_sent && gcnp::read_status(GCO.run).find("stage:netplay-open") != std::string::npos) {
        GCO.ready_sent = true;
        A.lobby.send_msg("{\"kind\":\"np_gc_ready\"}");
    }
    if (!GCO.host && !GCO.checked && GCO.fp_done && GCO.fp_path == launch_path_for(GCO.entry)) {
        GCO.checked = true;
        gcnp::Match m = gcnp::compare_disc(GCO.spec, GCO.fp_size, GCO.fp_sha);
        if (m == gcnp::Match::DifferentDisc) { autotest_log("warn:different-disc"); push_toast(gcnp::match_text(m), ui::WARN, 20.0, true); }   // normally blocked before Dolphin starts (gc_guest_poll)
    }
}

void gc_guest_prepare(const hub::GameEntry& e, const gcnp::Spec& s, int launch_no) {
    if (GCO.active && gc_process_alive()) return;
    GCO.spec = s;
    GCO.entry = e;
    GCO.launch_no = launch_no;
    GCO.guest_wait = true;
    GCO.host_ready = false;
    GCO.checked = false;
    autotest_log("gc_guest_prepare launch " + std::to_string(launch_no));
    gc_prefetch(e);   // archive: unpack automatically; disc fingerprint in the background
    hubnp3ds::Plan plan;
    plan.port = s.port;
    plan.expected = (int)A.lobby.members().size();
    std::string err;
    if (!GCO.fw.start_guest(server_url(), A.code, launch_no, plan, room_my_slot(), &err, "gc")) {
        launch_fail("GameCube online: " + err, 12.0);
        GCO.guest_wait = false;
    }
}

/* Guest: start Dolphin once the host's Dolphin listens, the session reaches the host, and the game is unpacked. */
void gc_guest_poll() {
    if (!GCO.guest_wait) return;
    if (gc_needs_unpack(GCO.entry)) { gc_prefetch(GCO.entry); return; }
    if (!GCO.host_ready || !GCO.fw.ready()) return;
    /* A different disc image desyncs Dolphin NetPlay: wait for the fingerprint and refuse to start on a mismatch. */
    if (!gc_fingerprint_ready(launch_path_for(GCO.entry))) return;
    gcnp::Match dm = gcnp::compare_disc(GCO.spec, GCO.fp_size, GCO.fp_sha);
    if (dm == gcnp::Match::DifferentDisc) {
        autotest_log("warn:different-disc");
        GCO.guest_wait = false;
        launch_fail(std::string(gcnp::match_text(dm)) + " Not starting.", 20.0);
        gc_stop_all();
        return;
    }
    GCO.checked = true;
    GCO.guest_wait = false;
    if (!gc_begin(GCO.entry, false, room_my_slot(), GCO.fw.connect_port())) gc_stop_all();
}

/* 3DS: bring up the room server (host) + UDP forwarder, then poll_np3ds() starts Azahar once everyone is reachable. */
void launch_online_3ds(const hub::GameEntry& e, int launch_no, bool is_host, const hubui::Json* msg, const hubnp3ds::Plan* resumed) {
    if (A.proc || NP3.active()) { launch_note(is_host, "A game is already running."); return; }
    hubemu::Install inst = hubemu::find(hubemu::Kind::Azahar, A.st.cemu_exe, e.path);
    if (inst.exe.empty()) { launch_note(is_host, "Install Azahar first (start the game offline once), then join the room again.", 10.0); return; }
    if (gc_needs_unpack(e)) {   // normally done already (n3ds_prefetch on selection); never let Azahar's "unpack?" prompt swallow the launch
        n3ds_prefetch(e);
        hubnp3ds::Plan hp;
        if (!is_host && !resumed && msg && hubnp3ds::parse_plan(*msg, &hp)) {   // guest: remember the launch, start it when the unpack finishes
            NP3_resume.arm(e.key, launch_no, hp, GetTickCount());
            set_notice("Unpacking " + e.title + " - the online game starts by itself when it is done.", 10.0);   // guest resume path (ResumeGate)
        } else launch_note(is_host, unpack_msg(e), 10.0);
        return;
    }
    /* The room rejects a duplicate nickname, and every fresh Azahar is called "AZAHAR": give this PC's 3DS a unique username
     * (RetroHub profile name + slot) in the NAND config before it starts. The file exists once a game ran offline once. */
    if (!hubnp3ds::set_console_username_file(inst.dir, hubnp3ds::console_username(A.st.name, room_my_slot()))) {
        launch_note(is_host, "Start this game once offline in Azahar (to create its 3DS settings), then join the room again.", 10.0);
        return;
    }
    hubnp3ds::Plan plan;
    std::string err;
    if (is_host) {
        plan.expected = (int)A.lobby.members().size();
        plan.port = hubnp3ds::kDefaultRoomPort;
        std::string room = hubnp3ds::room_exe_for(inst.exe);
        std::string wd = no_slash(hub::data_dir()) + "\\netplay\\3ds";
        make_dirs(wd);
        if (!NP3.start_host(server_url(), A.code, launch_no, plan, room, wd, &err)) { launch_note(is_host, "3DS online: " + err, 12.0); return; }
    } else {
        if (resumed) plan = *resumed;
        else if (!msg || !hubnp3ds::parse_plan(*msg, &plan)) { launch_note(is_host, "The host's 3DS room settings are missing - ask them to update RetroHub.", 10.0); return; }
        if (!NP3.start_guest(server_url(), A.code, launch_no, plan, room_my_slot(), &err)) { launch_note(is_host, "3DS online: " + err, 12.0); return; }
    }
    NP3_mp = hubnp3ds::multiplayer_arg(hubnp3ds::nickname(A.st.name, room_my_slot()), plan.password, "127.0.0.1", NP3.connect_port());
    hubnp3ds::seed_join_config_file(inst.dir, "", "127.0.0.1", NP3.connect_port());   // prefill Direct Connect before Azahar starts
    NP3_game = e;
    NP3_pending = true;
    set_notice("Connecting the 3DS wireless link...", 20.0);
}
void poll_np3ds() {
    if (NP3_resume.armed()) {
        if (A.lobby.members().empty()) NP3_resume.cancel();   // left the room
        else if (const hub::GameEntry* g = find_entry(NP3_resume.key())) {
            int no = 0;
            hubnp3ds::Plan p;
            if (NP3_resume.take(!gc_needs_unpack(*g), GetTickCount(), &no, &p)) { hub::GameEntry copy = *g; launch_online_3ds(copy, no, false, nullptr, &p); }
        } else NP3_resume.cancel();
    }
    if (!NP3.active()) return;
    NP3.update();
    if (NP3.failed()) {
        launch_fail("3DS online: " + NP3.status(), 12.0);
        if (is_host()) host_abort("3DS online: " + NP3.status());
        NP3.stop();
        NP3_pending = false;
        return;
    }
    if (NP3_pending && NP3.ready()) {   // never start Azahar earlier: a failed join raises a modal error dialog in it
        NP3_pending = false;
        autotest_log("3ds: forwarders ready, starting Azahar");
        RunInfo r3 = run_info_for(NP3_game, true);
        r3.room = true;
        launch_external(NP3_game, r3, "", NP3_mp);
        /* Azahar's Qt frontend ignores -m: join the room through its Direct Connect dialog (UI Automation helper). */
        if (!A.proc) {   // Azahar did not start: nothing to drive, and the room link would sit idle
            launch_fail("Azahar did not start - 3DS online cancelled.", 12.0);
            NP3.stop();
        } else {
            std::string st;
            make_dirs(no_slash(hub::data_dir()) + "\\netplay\\3ds");   // only the host created it before: a guest's helper could not write its status file
            hubnp3ds::start_join_driver(GetProcessId(A.proc), no_slash(hub::data_dir()) + "\\netplay\\3ds", hubnp3ds::nickname(A.st.name, room_my_slot()),
                                        "", "127.0.0.1", NP3.connect_port(), L"", &st);
        }
    }
}
void launch_online(const hub::GameEntry& e, int launch_no, int delay, bool is_host, const std::string& core_in, bool share_save,
                          const hubui::PortPlan* host_plan, const hubui::Json* launch_msg,
                          const hubui::MatchSpec* host_match, const std::string* launch_body) {
    A.st.last_game = room_id(e);
    mark_dirty();
    shot_trace("launch_online", std::string(is_host ? "host " : "guest ") + sys_short(e) + " " + room_id(e));
    if (g_headless && g_fake_room && (is_kanto(e) || e.system == hub::System::GC || e.system == hub::System::N3DS)) {   // peer-hosted routes need sockets: stop at the route
        shot_trace("online-route", std::string(is_kanto(e) ? "kanto" : e.system == hub::System::GC ? "dolphin-netplay" : "azahar-np3ds"));
        return;
    }
    if (is_kanto(e)) { kt_launch(e, launch_no, is_host); return; }
    if (e.system == hub::System::PS2) { launch_fail("PS2 online isn't supported yet - PCSX2 has no netplay. Play it offline from your library.", 12.0); return; }
    if (e.system == hub::System::GC) {   // Dolphin NetPlay: the host starts now; guests start from the launch handler
        if (is_host) {
            gcnp::Spec s;
            if (!gc_host_spec(e, (int)A.lobby.members().size(), &s)) return;
            GCO.spec = s;
            hubnp3ds::Plan plan;
            plan.port = s.port;
            plan.expected = (int)A.lobby.members().size();
            std::string err;
            if (!GCO.fw.start_host_forward(server_url(), A.code, launch_no, plan, "gc", &err)) { launch_note(is_host, "GameCube online: " + err, 12.0); return; }
            if (!gc_begin(e, true, room_my_slot(), s.port)) GCO.fw.stop();
        }
        return;
    }
    RunInfo ri = run_info_for(e, true);
    ri.room = true;
    hub::gs::Values gsv = hubui::online_values(e.system, gs_values_for(e), is_host, launch_body ? *launch_body : std::string());
    if (e.system == hub::System::N3DS) { launch_online_3ds(e, launch_no, is_host, launch_msg, nullptr); return; }
    if (is_native(e)) {
        std::string exe = smash_exe_path();
        std::vector<std::pair<std::string, std::string>> env;
        env.push_back({is_host ? "SSB64_LOBBY_HOST" : "SSB64_LOBBY_JOIN", server_url()});
        env.push_back({"SSB64_LOBBY_CODE", A.code});
        /* Smash's own lobby is driven by this room: it asks the server for the slot we hold here, and the server drops us from
         * it when we leave this room (and follows this room's host changes). */
        env.push_back({"SSB64_LOBBY_HUBSLOT", std::to_string(room_my_slot())});
        for (auto& kv : smash_env(exe)) env.push_back(kv);   // online uses the same vault save as offline
        spawn_room(is_host, exe, "--from-hub --no-update-check", dir_of(exe), env, ri);   // RetroHub's updater is the only one
        return;
    }
    /* port i -> i-th member in slot order (slots may have gaps); --slot stays the real lobby slot */
    /* The host builds the plan once and ships it; guests use it verbatim (fallback: derive locally for older hosts). */
    hubui::PortPlan plan;
    if (host_plan) plan = *host_plan;
    else if (launch_msg && hubui::parse_launch_plan(*launch_msg, &plan.ports, &plan.players)) {}
    else plan = hubui::build_ports(member_pads());
    {
        bool mine = false;   // this PC's slot must own a port, else the launch cannot work
        std::string tok;
        for (size_t i = 0; i <= plan.ports.size(); ++i) {
            if (i == plan.ports.size() || plan.ports[i] == ',') { if (tok == std::to_string(room_my_slot())) mine = true; tok.clear(); }
            else tok += plan.ports[i];
        }
        if (!mine) { launch_note(is_host, "The host's port plan doesn't include this PC."); return; }
    }
    /* Smash match picker: the host's resolved spec rides in the launch message; every PC passes the identical --match. */
    std::string match_args;
    A.run_fighter.clear();
    if (e.system == hub::System::N64 && hubui::match_game_supported(e.catalog_id)) {
        hubui::MatchSpec ms;
        bool have = host_match != nullptr;
        if (have) ms = *host_match;
        else if (launch_msg && launch_msg->get("match")) {
            have = hubui::parse_match(*launch_msg, plan.ports, e.catalog_id == "remix", &ms);
            if (!have) { launch_note(is_host, "The host's match setup is invalid - ask them to update RetroHub."); return; }
        }
        if (have) {
            match_args = " --match " + quote(hubui::match_arg(ms));
            int port = 0, my = -1;
            std::string tok;
            for (size_t i = 0; i <= plan.ports.size() && my < 0; ++i) {
                if (i == plan.ports.size() || plan.ports[i] == ',') {
                    if (tok == std::to_string(room_my_slot())) my = port;
                    if (!tok.empty() && tok != "-1") ++port;
                    tok.clear();
                } else tok += plan.ports[i];
            }
            if (my >= 0 && my < (int)ms.players.size()) A.run_fighter = hubui::fighter_name(ms.players[(size_t)my].fighter);
        }
    }
    std::string net = " --net " + quote(server_url()) + " --room " + A.code + "-" + std::to_string(launch_no) + " --slot " +
                      std::to_string(room_my_slot()) + " --players " + std::to_string(plan.players) + " --ports " +
                      plan.ports + hubui::delay_arg(delay);
    if (e.system == hub::System::N64) {
        std::string args = "--rom " + quote(e.path) + " --game " + (e.catalog_id.empty() ? "unknown" : e.catalog_id) + net + title_arg(e.title) + controls_arg("n64", e.key);
        args += hubui::n64_extra_args(gsv);
        args += cheats_launch_args(e, true, is_host, launch_body);
        args += match_args;
        if (share_save) {   // every PC must agree; only the host (slot 0) supplies its battery dir
            args += " --share-host-save";
            /* n64run's save-sharing role is "lobby slot 0" - which is the host, also after a host change. */
            if (is_host || room_my_slot() == 0) {
                std::string bd = no_slash(hub::saves::battery_dir(sys_short(e), e.key));
                if (!dir_exists(bd)) set_notice("No saved progress for " + e.title + " on this PC yet - everyone starts with an empty save.", 8.0);
                args += " --host-save-dir " + quote(bd);
            }
        }
        if (!file_exists(hub::exe_dir() + "emu\\mupen64plus.dll")) {
            std::string emu = find_upward("deps\\emu\\mupen64plus.dll");
            if (!emu.empty()) args += " --emu-dir " + quote(no_slash(dir_of(emu)));
        }
        spawn_room(is_host, n64run_path(), args, hub::exe_dir(), {}, ri);
        return;
    }
    bool is_gb_family = e.system == hub::System::GB || e.system == hub::System::GBC || e.system == hub::System::GBA;
    if (is_gb_family && hub::link_cable_game(e.catalog_id) && !is_host && !(launch_msg && launch_msg->get("np_link"))) {
        /* Never fall into the plain lockstep path for a link-cable game: both PCs would run different setups. */
        launch_note(false, "The host's launch lacks link-cable data - ask them to update RetroHub.");
        return;
    }
    if (is_gb_family && hub::link_cable_game(e.catalog_id)) {
        /* Link cable: every PC runs BOTH Game Boys in one SameBoy simulation (np_link); each PC loads its own vault save into its
         * console, the saves are exchanged through the room, and only the own console's save is written back. */
        /* GBA: mgba_link_libretro = N mGBA cores joined by mGBA's lockstep SIO driver (src/mgba_link). */
        const char* lcore = e.system == hub::System::GBA ? "mgba_link" : "sameboy_rb";
        std::string core = find_core(lcore);
        if (core.empty() && e.system != hub::System::GBA) { lcore = "sameboy"; core = find_core(lcore); }   // no rollback build: lockstep
        if (core.empty()) { launch_note(is_host, std::string("Emulator core '") + lcore + "' not found (cores folder)."); return; }
        std::vector<std::string> owners;
        {
            std::string tok;
            for (size_t i = 0; i <= plan.ports.size(); ++i) {
                if (i == plan.ports.size() || plan.ports[i] == ',') {
                    if (!tok.empty() && tok != "-1" && std::find(owners.begin(), owners.end(), tok) == owners.end()) owners.push_back(tok);
                    tok.clear();
                } else tok += plan.ports[i];
            }
        }
        if (owners.size() != 2) { launch_note(is_host, "Link cable games need exactly 2 players."); return; }
        std::string lnet = " --net " + quote(server_url()) + " --room " + A.code + "-" + std::to_string(launch_no) + " --slot " +
                           std::to_string(room_my_slot()) + " --players 2 --ports " + owners[0] + "," + owners[1] + ",-1,-1" + hubui::delay_arg(delay);
        std::string battery = no_slash(hub::saves::battery_dir(sys_short(e), e.key));
        std::string tmp = no_slash(hub::data_dir()) + "\\netplay\\" + sys_short(e);
        make_dirs(tmp);
        ri.online = false;   // the vault save is used and written back: track/sync it like an offline session
        std::string args = "--core " + quote(core) + " --rom " + quote(e.path) + " --system " + sys_short(e) + " --link --save-dir " + quote(battery) +
                           " --state-dir " + quote(tmp) + lnet + title_arg(e.title) + save_stem_arg(e) + controls_arg("retro", e.key);
        /* GGPO-style rollback (retrorun --rollback) for the GBA link core, whose savestates passed SyncTest and bad-network runs.
         * SameBoy uses the patched sameboy_rb core (src/sameboy_rb); Gambatte stays on lockstep. */
        if (e.system == hub::System::GBA || std::string(lcore) == "sameboy_rb") args += " --rollback";
        args += hubui::retro_extra_args(e.system, gsv);
        args += cheats_launch_args(e, true, is_host, launch_body);
        spawn_room(is_host, hub::exe_dir() + "retrorun.exe", args, hub::exe_dir(), {}, ri);
        return;
    }
    if (e.system == hub::System::GBA || e.system == hub::System::GB || e.system == hub::System::GBC) {
        std::string corename = e.system == hub::System::GBA ? "mgba" : (core_in.empty() ? A.st.gb_core : core_in);
        bool gb_rb = false;
        if (e.system != hub::System::GBA && corename == "sameboy" && !find_core("sameboy_rb").empty()) { corename = "sameboy_rb"; gb_rb = true; }
        std::string core = find_core(corename);
        if (core.empty()) { launch_note(is_host, "Emulator core '" + corename + "' not found (cores folder)."); return; }
        /* Netplay starts from an empty, identical save on every PC: use a throwaway dir. */
        std::string tmp = no_slash(hub::data_dir()) + "\\netplay\\" + sys_short(e);
        remove_tree(tmp);
        make_dirs(tmp);
        /* Save policy (docs/SAVES.md): with "use host's save" the host's vault battery files are COPIED into the throwaway dir and
         * retrorun --share-host-save broadcasts them (guests get the bytes before frame 0). Nobody writes into a vault. */
        if (share_save && (is_host || room_my_slot() == 0)) {
            std::error_code ec;
            std::filesystem::path bd = hub::saves::battery_dir(sys_short(e), e.key);
            if (std::filesystem::is_directory(bd, ec)) {
                for (auto& de : std::filesystem::directory_iterator(bd, ec))
                    if (de.is_regular_file(ec)) std::filesystem::copy_file(de.path(), std::filesystem::path(tmp) / de.path().filename(), std::filesystem::copy_options::overwrite_existing, ec);
            } else launch_note(true, "No saved progress for " + e.title + " on this PC yet - everyone starts with an empty save.", 8.0);
        }
        std::string args = "--core " + quote(core) + " --rom " + quote(e.path) + " --system " + sys_short(e) + " --save-dir " + quote(tmp) +
                           " --state-dir " + quote(tmp) + net + title_arg(e.title) + save_stem_arg(e) + controls_arg("retro", e.key);
        if (share_save) args += " --share-host-save";
        if (gb_rb) args += " --rollback";
        args += hubui::retro_extra_args(e.system, gsv);
        args += cheats_launch_args(e, true, is_host, launch_body);
        spawn_room(is_host, hub::exe_dir() + "retrorun.exe", args, hub::exe_dir(), {}, ri);
        return;
    }
    if (e.system == hub::System::NDS) {
        /* DS local wireless: no lockstep. Every PC runs its own melonDS core and only the wireless frames travel through
         * the room (retrorun --ds-link), so the game's own save is used and trades/battles are real. */
        std::string core = find_core("melondsds");
        if (core.empty()) { launch_note(is_host, "Emulator core for Nintendo DS not found (cores folder)."); return; }
        std::string mode;
        if (launch_msg && launch_msg->get("np_ds") && !hubui::parse_np_ds(*launch_msg, &mode)) {
            launch_note(is_host, "The host's DS launch is newer than this RetroHub - update to join.");
            return;
        }
        ri.online = false;   // real save: play time and the vault see this like an offline session
        std::string sys = sys_short(e);
        std::string battery = no_slash(hub::saves::battery_dir(sys, e.key));
        std::string states = no_slash(hub::saves::states_dir(sys, e.key));
        std::string args = "--core " + quote(core) + " --rom " + quote(e.path) + " --system nds --save-dir " + quote(battery) +
                           " --state-dir " + quote(states) + " --ds-link --net " + quote(server_url()) + " --room " + A.code + "-" +
                           std::to_string(launch_no) + " --slot " + std::to_string(room_my_slot()) + " --players " +
                           std::to_string(plan.players) + " --ports " + plan.ports + title_arg(e.title) + save_stem_arg(e);   // --ports: the real member slots (gaps after a leave)
        args += hubui::retro_extra_args(e.system, gsv);
        spawn_room(is_host, hub::exe_dir() + "retrorun.exe", args, hub::exe_dir(), {}, ri);
        return;
    }
    if (e.system == hub::System::PS1) {
        /* PlayStation (Beetle PSX): every PC runs the whole console; inputs (buttons + both analog sticks) travel through the room, the
         * host's memory card is the shared session save (docs/SAVES.md). 3-4 players use the emulated multitap (ps1_online_args). */
        std::string tmp = no_slash(hub::data_dir()) + "\\netplay\\" + sys_short(e);
        remove_tree(tmp);
        make_dirs(tmp);
        if (share_save && (is_host || room_my_slot() == 0)) {
            std::error_code ec;
            std::filesystem::path bd = hub::saves::battery_dir(sys_short(e), e.key);
            if (std::filesystem::is_directory(bd, ec)) {
                for (auto& de : std::filesystem::directory_iterator(bd, ec))
                    if (de.is_regular_file(ec)) std::filesystem::copy_file(de.path(), std::filesystem::path(tmp) / de.path().filename(), std::filesystem::copy_options::overwrite_existing, ec);
            } else launch_note(true, "No saved progress for " + e.title + " on this PC yet - everyone starts with an empty memory card.", 8.0);
        }
        std::string args, err;
        if (!ps1_online_args(e, net, plan.ports, tmp, share_save, gsv, &args, &err)) { launch_note(is_host, err, 12.0); return; }
        spawn_room(is_host, hub::exe_dir() + "retrorun.exe", args, hub::exe_dir(), {}, ri);
        return;
    }
    launch_note(is_host, "Online play is not available for this system yet.");
}

