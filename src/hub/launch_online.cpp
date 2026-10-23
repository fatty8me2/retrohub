#include "app.h"

GcOnline GCO;

bool gc_needs_unpack(const hub::GameEntry& e) { return e.issue == "extract" && hub::extracted_3ds_path(e).empty(); }

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

/* Called while a GameCube game is selected in the room: unpack an archived copy (existing progress UI) and fingerprint the
 * disc in the background, so Launch never waits. */
void gc_prefetch(const hub::GameEntry& e) {
    if (gc_needs_unpack(e)) {
        if (UNZ.state == UnzipJob::Idle && !UNZ.th.joinable()) { UNZ.key = e.key; UNZ.quiet = true; start_unzip(e); }
        return;
    }
    gc_fingerprint_ready(launch_path_for(e));
}

/* Host: the NetPlay spec for the selected game. The sha is included when the background hash is already done. */
bool gc_host_spec(const hub::GameEntry& e, int players, gcnp::Spec* out) {
    if (gc_needs_unpack(e)) return false;
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
    return !s.game_id.empty();
}

bool gc_begin(const hub::GameEntry& e, bool host, int slot, int port) {
    if (GCO.active && gc_process_alive()) { set_notice("A GameCube online session is already running."); return false; }
    hubemu::Install inst = hubemu::find(hubemu::Kind::Dolphin, "", "");
    if (inst.exe.empty()) { set_notice("Dolphin isn't installed yet - start any GameCube game once so RetroHub can download it."); return false; }
    std::string game = launch_path_for(e);
    if (game.empty() || gc_needs_unpack(e)) { set_notice("Unpack " + e.title + " first."); return false; }
    if (hubemu::is_running(inst.exe)) { set_notice("Dolphin is already running. Close it first."); return false; }
    if (g_headless) { set_notice("Headless test run: launch skipped."); return false; }
    hubemu::dolphin_migrate_old(hub::data_dir(), hub::exe_dir(), inst.dir);
    hubemu::prepare_dolphin(inst.dir, hubemu::detect_dolphin_pads());   // managed config: no update check, no analytics, no panic alerts
    std::string user = inst.dir + "\\User", ini = user + "\\Config\\Dolphin.ini";
    gcnp::Spec sp = GCO.spec;
    sp.mode = "direct";
    sp.addr = "127.0.0.1";
    sp.port = port;
    gcnp::IniParams ip;
    ip.host = host;
    ip.nickname = member_name(room_my_slot());
    ip.iso_dir = dir_of(game);
    std::string cur;
    { std::ifstream f(ini, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); cur = ss.str(); }
    std::string next = gcnp::ini_patch(cur, sp, ip);
    { std::ofstream f(ini, std::ios::binary | std::ios::trunc); f << next; }
    GCO.run = gcnp::Run();
    std::string extra = std::string("-C Dolphin.Display.Fullscreen=") + (hub::gs::is_fullscreen(gs_values_for(e)) ? "True" : "False");
    if (!gcnp::start(host, inst.exe, user, sp, slot, hub::data_dir() + "\\gcnp", "", extra, &GCO.run)) {
        set_notice("Could not start Dolphin NetPlay: " + GCO.run.error);
        return false;
    }
    GCO.active = true;
    GCO.host = host;
    GCO.ready_sent = false;
    GCO.entry = e;
    push_toast(host ? "Starting GameCube NetPlay as host..." : "Joining the host's GameCube NetPlay...", ui::GOOD, 6.0);
    return true;
}

void gc_stop_all() {
    GCO.fw.stop();
    GCO.active = false;
    GCO.guest_wait = false;
}

/* Each frame: keep the forwarder alive, tell guests when the host's Dolphin is listening, check the guest's disc. */
void gc_tick() {
    if (GCO.fw.active()) {
        GCO.fw.update();
        if (GCO.fw.failed()) {
            set_notice("GameCube online: " + GCO.fw.status(), 12.0);
            gc_kill();
            gc_stop_all();
            return;
        }
    }
    if (!GCO.active) {
        if (room_connected() && !A.selected_game.empty()) {
            const hub::GameEntry* g = find_room_entry(A.selected_game);
            if (g && g->system == hub::System::GC) gc_prefetch(*g);
        }
        return;
    }
    if (!gc_process_alive()) { gc_stop_all(); return; }
    if (GCO.host && !GCO.ready_sent && gcnp::read_status(GCO.run).find("stage:netplay-open") != std::string::npos) {
        GCO.ready_sent = true;
        A.lobby.send_msg("{\"kind\":\"np_gc_ready\"}");
    }
    if (!GCO.host && !GCO.checked && GCO.fp_done && GCO.fp_path == launch_path_for(GCO.entry)) {
        GCO.checked = true;
        gcnp::Match m = gcnp::compare_disc(GCO.spec, GCO.fp_size, GCO.fp_sha);
        if (m == gcnp::Match::DifferentDisc) push_toast(gcnp::match_text(m), ui::WARN, 20.0, true);
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
    gc_prefetch(e);   // archive: unpack automatically; disc fingerprint in the background
    hubnp3ds::Plan plan;
    plan.port = s.port;
    plan.expected = (int)A.lobby.members().size();
    std::string err;
    if (!GCO.fw.start_guest(server_url(), A.code, launch_no, plan, A.lobby.my_slot(), &err, "gc")) {
        set_notice("GameCube online: " + err, 12.0);
        GCO.guest_wait = false;
    }
}

/* Guest: start Dolphin once the host's Dolphin listens, the session reaches the host, and the game is unpacked. */
void gc_guest_poll() {
    if (!GCO.guest_wait) return;
    if (gc_needs_unpack(GCO.entry)) { gc_prefetch(GCO.entry); return; }
    if (!GCO.host_ready || !GCO.fw.ready()) return;
    gc_fingerprint_ready(launch_path_for(GCO.entry));
    GCO.guest_wait = false;
    if (!gc_begin(GCO.entry, false, room_my_slot(), GCO.fw.connect_port())) gc_stop_all();
}

/* 3DS: bring up the room server (host) + UDP forwarder, then poll_np3ds() starts Azahar once everyone is reachable. */
void launch_online_3ds(const hub::GameEntry& e, int launch_no, bool is_host, const hubui::Json* msg) {
    if (A.proc || NP3.active()) { set_notice("A game is already running."); return; }
    hubemu::Install inst = hubemu::find(hubemu::Kind::Azahar, A.st.cemu_exe, e.path);
    if (inst.exe.empty()) { set_notice("Install Azahar first (start the game offline once), then join the room again.", 10.0); return; }
    hubnp3ds::Plan plan;
    std::string err;
    if (is_host) {
        plan.expected = (int)A.lobby.members().size();
        plan.port = hubnp3ds::kDefaultRoomPort;
        std::string room = hubnp3ds::room_exe_for(inst.exe);
        std::string wd = no_slash(hub::data_dir()) + "\netplay\3ds";
        make_dirs(wd);
        if (!NP3.start_host(server_url(), A.code, launch_no, plan, room, wd, &err)) { set_notice("3DS online: " + err, 12.0); return; }
    } else {
        if (!msg || !hubnp3ds::parse_plan(*msg, &plan)) { set_notice("The host's 3DS room settings are missing - ask them to update RetroHub.", 10.0); return; }
        if (!NP3.start_guest(server_url(), A.code, launch_no, plan, A.lobby.my_slot(), &err)) { set_notice("3DS online: " + err, 12.0); return; }
    }
    NP3_mp = hubnp3ds::multiplayer_arg(hubnp3ds::nickname(A.st.name, A.lobby.my_slot()), plan.password, "127.0.0.1", NP3.connect_port());
    NP3_game = e;
    NP3_pending = true;
    set_notice("Connecting the 3DS wireless link...", 20.0);
}
void poll_np3ds() {
    if (!NP3.active()) return;
    NP3.update();
    if (NP3.failed()) { set_notice("3DS online: " + NP3.status(), 12.0); NP3.stop(); NP3_pending = false; return; }
    if (NP3_pending && NP3.ready()) {   // never start Azahar earlier: a failed join raises a modal error dialog in it
        NP3_pending = false;
        launch_external(NP3_game, run_info_for(NP3_game, true), "", NP3_mp);
    }
}
void launch_online(const hub::GameEntry& e, int launch_no, int delay, bool is_host, const std::string& core_in, bool share_save,
                          const hubui::PortPlan* host_plan, const hubui::Json* launch_msg,
                          const hubui::MatchSpec* host_match, const std::string* launch_body) {
    A.st.last_game = room_id(e);
    mark_dirty();
    if (e.system == hub::System::GC) {   // Dolphin NetPlay: the host starts now; guests start from the launch handler
        if (is_host) {
            gcnp::Spec s;
            if (!gc_host_spec(e, (int)A.lobby.members().size(), &s)) return;
            GCO.spec = s;
            hubnp3ds::Plan plan;
            plan.port = s.port;
            plan.expected = (int)A.lobby.members().size();
            std::string err;
            if (!GCO.fw.start_host_forward(server_url(), A.code, launch_no, plan, "gc", &err)) { set_notice("GameCube online: " + err, 12.0); return; }
            if (!gc_begin(e, true, 0, s.port)) GCO.fw.stop();
        }
        return;
    }
    RunInfo ri = run_info_for(e, true);
    hub::gs::Values gsv = hubui::online_values(e.system, gs_values_for(e), is_host, launch_body ? *launch_body : std::string());
    if (e.system == hub::System::N3DS) { launch_online_3ds(e, launch_no, is_host, launch_msg); return; }
    if (is_native(e)) {
        std::string exe = smash_exe_path();
        std::vector<std::pair<std::string, std::string>> env;
        env.push_back({is_host ? "SSB64_LOBBY_HOST" : "SSB64_LOBBY_JOIN", server_url()});
        env.push_back({"SSB64_LOBBY_CODE", A.code});
        for (auto& kv : smash_env(exe)) env.push_back(kv);   // online uses the same vault save as offline
        spawn(exe, "--from-hub --no-update-check", dir_of(exe), env, ri);   // RetroHub's updater is the only one
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
            if (i == plan.ports.size() || plan.ports[i] == ',') { if (tok == std::to_string(A.lobby.my_slot())) mine = true; tok.clear(); }
            else tok += plan.ports[i];
        }
        if (!mine) { set_notice("The host's port plan doesn't include this PC."); return; }
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
            if (!have) { set_notice("The host's match setup is invalid - ask them to update RetroHub."); return; }
        }
        if (have) {
            match_args = " --match " + quote(hubui::match_arg(ms));
            int port = 0, my = -1;
            std::string tok;
            for (size_t i = 0; i <= plan.ports.size() && my < 0; ++i) {
                if (i == plan.ports.size() || plan.ports[i] == ',') {
                    if (tok == std::to_string(A.lobby.my_slot())) my = port;
                    if (!tok.empty() && tok != "-1") ++port;
                    tok.clear();
                } else tok += plan.ports[i];
            }
            if (my >= 0 && my < (int)ms.players.size()) A.run_fighter = hubui::fighter_name(ms.players[(size_t)my].fighter);
        }
    }
    std::string net = " --net " + quote(server_url()) + " --room " + A.code + "-" + std::to_string(launch_no) + " --slot " +
                      std::to_string(A.lobby.my_slot()) + " --players " + std::to_string(plan.players) + " --ports " +
                      plan.ports + hubui::delay_arg(delay);
    if (e.system == hub::System::N64) {
        std::string args = "--rom " + quote(e.path) + " --game " + (e.catalog_id.empty() ? "unknown" : e.catalog_id) + net + title_arg(e.title);
        args += hubui::n64_extra_args(gsv);
        args += match_args;
        if (share_save) {   // every PC must agree; only the host (slot 0) supplies its battery dir
            args += " --share-host-save";
            if (A.lobby.my_slot() == 0)
                args += " --host-save-dir " + quote(no_slash(hub::saves::battery_dir(sys_short(e), e.key)));
        }
        if (!file_exists(hub::exe_dir() + "emu\\mupen64plus.dll")) {
            std::string emu = find_upward("deps\\emu\\mupen64plus.dll");
            if (!emu.empty()) args += " --emu-dir " + quote(no_slash(dir_of(emu)));
        }
        spawn(n64run_path(), args, hub::exe_dir(), {}, ri);
        return;
    }
    if ((e.system == hub::System::GB || e.system == hub::System::GBC) && hub::link_cable_game(e.catalog_id) &&
        (is_host || (launch_msg && launch_msg->get("np_link")))) {
        /* Link cable: every PC runs BOTH Game Boys in one SameBoy simulation (np_link); each PC loads its own vault save into its
         * console, the saves are exchanged through the room, and only the own console's save is written back. */
        std::string core = find_core("sameboy");
        if (core.empty()) { set_notice("Emulator core 'sameboy' not found (cores folder)."); return; }
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
        if (owners.size() != 2) { set_notice("Link cable games need exactly 2 players."); return; }
        std::string lnet = " --net " + quote(server_url()) + " --room " + A.code + "-" + std::to_string(launch_no) + " --slot " +
                           std::to_string(A.lobby.my_slot()) + " --players 2 --ports " + owners[0] + "," + owners[1] + ",-1,-1" + hubui::delay_arg(delay);
        std::string battery = no_slash(hub::saves::battery_dir(sys_short(e), e.key));
        std::string tmp = no_slash(hub::data_dir()) + "\\netplay\\" + sys_short(e);
        make_dirs(tmp);
        ri.online = false;   // the vault save is used and written back: track/sync it like an offline session
        std::string args = "--core " + quote(core) + " --rom " + quote(e.path) + " --system " + sys_short(e) + " --link --save-dir " + quote(battery) +
                           " --state-dir " + quote(tmp) + lnet + title_arg(e.title) + save_stem_arg(e);
        args += hubui::retro_extra_args(e.system, gsv);
        spawn(hub::exe_dir() + "retrorun.exe", args, hub::exe_dir(), {}, ri);
        return;
    }
    if (e.system == hub::System::GBA || e.system == hub::System::GB || e.system == hub::System::GBC) {
        std::string corename = e.system == hub::System::GBA ? "mgba" : (core_in.empty() ? A.st.gb_core : core_in);
        std::string core = find_core(corename);
        if (core.empty()) { set_notice("Emulator core '" + corename + "' not found (cores folder)."); return; }
        /* Netplay starts from an empty, identical save on every PC: use a throwaway dir. */
        std::string tmp = no_slash(hub::data_dir()) + "\\netplay\\" + sys_short(e);
        remove_tree(tmp);
        make_dirs(tmp);
        std::string args = "--core " + quote(core) + " --rom " + quote(e.path) + " --system " + sys_short(e) + " --save-dir " + quote(tmp) +
                           " --state-dir " + quote(tmp) + net + title_arg(e.title) + save_stem_arg(e);
        args += hubui::retro_extra_args(e.system, gsv);
        spawn(hub::exe_dir() + "retrorun.exe", args, hub::exe_dir(), {}, ri);
        return;
    }
    set_notice("Online play is not available for this system yet.");
}

