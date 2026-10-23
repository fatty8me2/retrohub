/* RetroHub.exe - the launcher front door.
 *
 * Layout: full-window custom UI (no floating windows): left sidebar nav + content pages
 * (Library, Play Online, Saves, Controllers, Settings, About). Drawn with Dear ImGui on SDL_Renderer.
 *
 * Launch contracts:
 *  - N64 (emulated):  n64run.exe --rom <path> --game <id|unknown> --save-dir <battery dir>
 *                     [--net <server> --room <CODE>-<launch> --slot <lobby slot> --players <n>
 *                      --ports <p0,p1,p2,p3> --delay <d>]
 *  - GBA/GB/GBC/DS:   retrorun.exe --core <x_libretro.dll> --rom <path> --system <gba|gb|gbc|nds>
 *                     --save-dir <battery dir> --state-dir <states dir> --resume [--fullscreen]
 *                     [--net ... same flags as n64run; online uses a fresh temp save dir, no --resume]
 *  - GameCube:        Dolphin (RetroHub download, portable) -b -e <game>; see hubemu::dolphin_args
 *  - 3DS:             <data>\emulators\azahar\azahar.exe -f <game>   (src/hub/emulators.cpp; downloaded on demand)
 *  - Wii U:           Cemu.exe -g <game> -f  (an existing install, else <data>\emulators\cemu with a portable folder)
 *  - Smash (native):  BattleShip.exe --from-hub --no-update-check
 *                     (ALWAYS passed: skips handing back to the hub, and RetroHub's updater is the only one)
 *                     Online, environment variables:
 *                       SSB64_LOBBY_HOST=<server>  (host)   or  SSB64_LOBBY_JOIN=<server> (joiners)
 *                       SSB64_LOBBY_CODE=<CODE>             (BattleShip joins/hosts this exact code;
 *                                                           players pick fighter/stage in its Online window)
 *                       (no autostart: the host presses Start match in Smash)
 */
#include "invite.h"
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "third_party/stb_image.h"
#include "app.h"

hubemu::Installer EMU;   // download-on-demand job + its dialog state
UnzipJob UNZ;
hubnp3ds::Runner NP3;                 // 3DS local-wireless forwarder (np_3ds)
std::string NP3_mp;                    // "-m ..." for Azahar once NP3 is ready
hub::GameEntry NP3_game;
bool NP3_pending = false;

/* ---------- process launching ---------- */

bool spawn(const std::string& exe, const std::string& args, const std::string& cwd,
                  const std::vector<std::pair<std::string, std::string>>& env, const RunInfo& ri) {
    if (g_headless) { set_notice("Headless test run: launch skipped."); return false; }
    if (A.proc) { set_notice("A game is already running."); return false; }
    if (!file_exists(exe)) { set_notice("Cannot find " + file_name(exe) + " (" + exe + ")"); return false; }
    std::wstring cmd = L"\"" + W(exe) + L"\" " + W(args);
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    for (auto& e : env) SetEnvironmentVariableW(W(e.first).c_str(), W(e.second).c_str());
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(W(exe).c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr, W(cwd).c_str(), &si, &pi);
    DWORD err = GetLastError();
    for (auto& e : env) SetEnvironmentVariableW(W(e.first).c_str(), nullptr);
    if (!ok) { set_notice("Could not start " + file_name(exe) + " (error " + std::to_string(err) + ")"); return false; }
    CloseHandle(pi.hThread);
    if (ri.after_exit) {   // external emulator: follow every process it starts, so a relaunch / hand-off keeps the session open
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (job && !AssignProcessToJobObject(job, pi.hProcess)) { CloseHandle(job); job = nullptr; }
        A.job_h = job;
    }
    A.proc = pi.hProcess;
    A.proc_title = ri.title;
    A.run = ri;
    A.run.start_unix = unix_now();
    A.run.start_tick = GetTickCount64();
    return true;
}

void poll_process() {
    if (!A.proc) return;
    if (WaitForSingleObject(A.proc, 0) != WAIT_OBJECT_0) return;
    if (A.job_h) {   // the emulator may have handed the game to a child: wait until the whole job is gone
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION ji{};
        if (QueryInformationJobObject(A.job_h, JobObjectBasicAccountingInformation, &ji, sizeof ji, nullptr) && ji.ActiveProcesses > 0) return;
        CloseHandle(A.job_h);
        A.job_h = nullptr;
    }
    DWORD code = 0;
    GetExitCodeProcess(A.proc, &code);
    CloseHandle(A.proc);
    A.proc = nullptr;
    if (A.run.after_exit) { auto fn = std::move(A.run.after_exit); A.run.after_exit = nullptr; fn(); }
    uint64_t secs = (GetTickCount64() - A.run.start_tick) / 1000;
    if (!A.run.key.empty()) {
        if (secs >= 1) hub::record_play(A.run.key, A.run.start_unix, secs);
        if (hubui::exit_should_touch_game(!A.run.key.empty(), A.run.track, A.run.online, A.run.system_short))
            hub::saves::touch_game(A.run.system_short, A.run.key, A.run.title);
        hub::flush_library_state();
        refresh_state(A.run.key);
    }
    A.vault_dirty = true;
    A.info_at = -100;
    set_notice(A.proc_title + ": " + (code == 0 ? std::string("closed.") : hubui::exit_code_text(code)), 20.0);
    auto_sync();
}

/* Core DLL: <exe>\cores\ (packaged) or <Hub>\deps\cores\ (dev). */
std::string find_core(const std::string& name) {
    std::string f = name + "_libretro.dll";
    std::string p = hub::exe_dir() + "cores\\" + f;
    if (file_exists(p)) return p;
    return find_upward("deps\\cores\\" + f);
}
std::string quote(const std::string& s) { return "\"" + s + "\""; }

/* Game window caption: "<title> — RetroHub" (UTF-8 em dash), quoted for the command line. */
std::string title_arg(const std::string& game_title) {
    std::string t = game_title;
    t.erase(std::remove(t.begin(), t.end(), '"'), t.end());   // a quote would break the argument
    return " --title " + quote(t + " \xE2\x80\x94 RetroHub");
}

/* retrorun names the DS save <stem>.srm after the ROM file. A zipped ROM is extracted as rom.<ext>, so pass the inner name's stem. */
std::string save_stem_arg(const hub::GameEntry& e) {
    return e.save_stem.empty() ? std::string() : " --save-stem " + quote(e.save_stem);
}

/* Env for every Smash Classic launch: ROM hand-off + vault save path. Migrates the legacy save once (copy, never delete). */
std::vector<std::pair<std::string, std::string>> smash_env(const std::string& exe) {
    std::vector<hubui::RomRef> refs;
    for (auto& g : A.lib) refs.push_back({g.catalog_id, g.serial, g.path});
    std::string rom = hubui::find_smash_rom(refs);
    std::string save = hubui::smash_save_file(hub::saves::battery_dir(hubui::kSmashSaveSystem, hubui::kSmashSaveKey));
    std::string legacy = dir_of(exe) + "ssb64_save.bin";
    bool vault_has = file_exists(save), legacy_has = file_exists(legacy);
    if (hubui::smash_migrate_legacy_save(legacy, save)) {
        // copied once; the vault owns the save from now on
    } else if (!A.st.smash_save_notice_shown &&
               hubui::smash_save_conflict(vault_has, legacy_has, read_text_file(save), read_text_file(legacy))) {
        // both exist and differ: the vault wins; tell the player once where the old local save is
        A.st.smash_save_notice_shown = true;
        mark_dirty();
        push_toast(hubui::smash_save_conflict_text(legacy), ui::WARN, 12.0);
    }
    if (!file_exists(dir_of(exe) + "BattleShip.o2r")) push_toast("Preparing Smash Classic for first use (about 30 s)...", ui::WARN, 12.0);
    return hubui::smash_launch_env(rom, save);
}

RunInfo run_info_for(const hub::GameEntry& e, bool online) {
    RunInfo ri;
    ri.key = e.key;
    ri.system_short = is_native(e) ? std::string(hubui::kSmashSaveSystem) : sys_short(e);
    ri.title = e.title;
    ri.track = true;   // the native port is tracked too: its save lives in the vault (n64 / native:smash64)
    ri.online = online;
    return ri;
}


/* ---------- 3DS (Azahar) and Wii U (Cemu): external emulators, see docs/EMULATORS.md ---------- */



/* Size of the image inside a zipped 3DS game (read once from the zip's directory). */
uint64_t unpacked_size(const hub::GameEntry& e) {
    if (e.system == hub::System::GC) return e.unpacked ? e.unpacked : e.size;   // from the scan cache; never runs tar on the UI thread
    static std::map<std::string, uint64_t> cache;
    auto it = cache.find(e.path);
    if (it == cache.end()) it = cache.emplace(e.path, hub::zip_3ds_unpacked_size(e)).first;
    return it->second ? it->second : e.size;
}

std::string launch_path_for(const hub::GameEntry& e) {
    if (e.issue == "extract") return hub::extracted_3ds_path(e);
    return e.path;
}

void launch_external(const hub::GameEntry& e, const RunInfo& ri, const std::string& battery, const std::string& mp_arg) {
    hubemu::Kind kind = hubemu::kind_for(e.system);
    if ((e.system == hub::System::N3DS || e.system == hub::System::GC) && e.issue == "extract" && hub::extracted_3ds_path(e).empty()) {   // archived image: ask once, unpack, then start
        UNZ.key = e.key;
        UNZ.state = UnzipJob::Asking;
        return;
    }
    hubemu::Install inst = hubemu::find(kind, A.st.cemu_exe, e.path);
    if (inst.exe.empty()) {   // not installed: ask, download, then start this game
        EMU.kind = kind;
        EMU.game_key = e.key;
        EMU.state = hubemu::Installer::Asking;
        return;
    }
    if (g_headless) { set_notice("Headless test run: launch skipped."); return; }
    if (hubemu::is_running(inst.exe)) {   // a second launch would hand over to the running instance and end the session early
        set_notice(std::string(hubemu::name(kind)) + " is already running. Close it first, then start the game from RetroHub.", 10.0);
        return;
    }
    if (kind == hubemu::Kind::Azahar) {
        if (inst.managed) hubemu::prepare_azahar(inst.dir, controls_profile("3ds", e.key));
    } else if (kind == hubemu::Kind::Dolphin) {
        hubemu::dolphin_migrate_old(hub::data_dir(), hub::exe_dir(), inst.dir);
        hubemu::prepare_dolphin(inst.dir, hubemu::detect_dolphin_pads(), controls_profile("gc", e.key));
    } else if (inst.managed) {
        hubemu::prepare_cemu_managed(inst.dir, controls_profile("wiiu", e.key));
    }
    hub::gs::Values gsv = gs_values_for(e);
    hubemu::apply_game_settings(kind, inst.dir, gsv);
    bool fs = hub::gs::is_fullscreen(gsv);
    std::string game = launch_path_for(e);
    auto sess = std::make_shared<hubemu::SaveSession>(hubemu::begin_session(e, inst, battery, ""));
    std::string args = kind == hubemu::Kind::Azahar ? hubemu::azahar_args(game, fs, mp_arg)
                       : kind == hubemu::Kind::Dolphin ? hubemu::dolphin_args(game, fs, hubemu::dolphin_card_root(inst.dir, e.key) + "\\GCI")
                                                       : hubemu::cemu_args(game, fs);
    if (e.issue == "encrypted") set_notice("This 3DS image is encrypted: Azahar needs your own AES keys (aes_keys.txt in its user\\sysdata folder).", 12.0);
    if (e.issue == "keys") set_notice("Disc images (.wud/.wux) need your disc key in Cemu's keys.txt. A .wua needs no keys.", 12.0);
    RunInfo r = ri;
    r.after_exit = [sess]() {   // emulator save folder -> vault, before the sync runs
        hubemu::end_session(*sess);
        NP3.stop();
        if (!sess->error.empty()) push_toast("Could not copy the save into the vault: " + sess->error + " (your emulator's copy is untouched)", ui::WARN, 15.0);
    };
    if (!spawn(inst.exe, args, inst.dir, {}, r)) NP3.stop();
}

void start_unzip(const hub::GameEntry& e) {
    if (UNZ.th.joinable()) UNZ.th.join();
    UNZ.done = false;
    UNZ.ok = false;
    UNZ.cancel = false;
    UNZ.got = 0;
    UNZ.total = unpacked_size(e);
    UNZ.error.clear();
    UNZ.state = UnzipJob::Working;
    hub::GameEntry copy = e;
    UNZ.th = std::thread([copy]() {
        std::string out, err;
        bool ok = hub::extract_3ds_zip(copy, [](uint64_t done, uint64_t total) { UNZ.got = done; UNZ.total = total; return !UNZ.cancel.load(); }, &out, &err);
        if (!ok) { std::lock_guard<std::mutex> lk(UNZ.mu); UNZ.error = err; }
        UNZ.ok = ok;
        UNZ.done = true;
    });
}
void flash_taskbar() {
    if (!A.hwnd || g_headless) return;
    FLASHWINFO fi{};
    fi.cbSize = sizeof(fi);
    fi.hwnd = A.hwnd;
    fi.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;   // taskbar button only, until the user looks at RetroHub; no activation
    fi.uCount = 6;
    FlashWindowEx(&fi);
}

void add_game_shortcut(const hub::GameEntry& e) {
    std::wstring dir = shell_shortcut_dir();
    if (dir.empty()) { set_notice("Desktop shortcuts are only created by an installed RetroHub."); return; }
    std::wstring key = W(e.key);
    if (!invite_is_valid_game_key(key)) { set_notice("This game cannot be added to the desktop."); return; }
    std::wstring icon;
    if (hubart::enabled()) {   // cover art as the icon when it is already cached
        std::string name, png;
        int w = 0, h = 0;
        std::string err;
        if (hubart::detail::resolve_name(e, &name, nullptr) &&
            hubart::detail::fetch_decode(sys_short(e), name, hubart::Kind::Boxart, &w, &h, &err, &png)) {
            int iw = 0, ih = 0, comp = 0;
            std::ifstream pf(std::filesystem::path(W(png)), std::ios::binary);
            std::string bytes((std::istreambuf_iterator<char>(pf)), std::istreambuf_iterator<char>());
            unsigned char* px = bytes.empty() ? nullptr : stbi_load_from_memory((const unsigned char*)bytes.data(), (int)bytes.size(), &iw, &ih, &comp, 4);
            if (px) {
                std::wstring icodir = W(hub::data_dir() + "icons\\");
                CreateDirectoryW(icodir.c_str(), nullptr);
                std::wstring ico = icodir + key + L".ico";
                if (shell_write_ico(ico, px, iw, ih)) icon = ico;
                stbi_image_free(px);
            }
        }
    }
    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, exe, (DWORD)(sizeof(exe) / sizeof(exe[0])));
    std::wstring lnk = dir + L"\\" + shell_game_shortcut_name(W(e.title));
    if (shell_write_game_shortcut(lnk, std::wstring(exe, n), key, W(e.title), icon)) set_toast("Added " + e.title + " to your desktop.");
    else set_notice("Could not create the shortcut.");
}

void play_entry(const hub::GameEntry& e) {
    if (A.proc) { set_notice("A game is already running."); return; }
    if (A.job.running) {
        std::string key = e.key;
        A.after_job = [key]() { if (const hub::GameEntry* g = find_entry(key)) { A.sel_game = key; hub::GameEntry copy = *g; play_entry(copy); } };
        set_notice("Syncing your saves first, then starting " + e.title + "...", 10.0);
        return;
    }
    A.st.last_game = room_id(e);
    mark_dirty();
    RunInfo ri = run_info_for(e, false);
    std::string sys = sys_short(e);
    if (is_native(e)) {
        std::string exe = smash_exe_path();
        spawn(exe, "--from-hub --no-update-check", dir_of(exe), smash_env(exe), ri);   // RetroHub's updater is the only one
        return;
    }
    std::string battery = no_slash(hub::saves::battery_dir(sys, e.key));
    std::string states = no_slash(hub::saves::states_dir(sys, e.key));
    switch (e.system) {
        case hub::System::N64: {
            std::string args = "--rom " + quote(e.path) + " --game " + (e.catalog_id.empty() ? "unknown" : e.catalog_id) +
                               " --save-dir " + quote(battery) + title_arg(e.title) + controls_arg("n64", e.key);
            args += hubui::n64_extra_args(gs_values_for(e));
            if (!file_exists(hub::exe_dir() + "emu\\mupen64plus.dll")) {
                std::string emu = find_upward("deps\\emu\\mupen64plus.dll");
                if (!emu.empty()) args += " --emu-dir " + quote(no_slash(dir_of(emu)));
            }
            spawn(n64run_path(), args, hub::exe_dir(), {}, ri);
            break;
        }
        case hub::System::GBA:
        case hub::System::GB:
        case hub::System::GBC: {
            std::string core = find_core(e.system == hub::System::GBA ? "mgba" : A.st.gb_core);
            if (core.empty()) { set_notice("Emulator core for " + std::string(hub::system_name(e.system)) + " not found (cores folder)."); return; }
            std::string args = "--core " + quote(core) + " --rom " + quote(e.path) + " --system " + sys + " --save-dir " + quote(battery) +
                               " --state-dir " + quote(states) + " --resume" + title_arg(e.title) + save_stem_arg(e) + controls_arg("retro", e.key);
            args += hubui::retro_extra_args(e.system, gs_values_for(e));
            spawn(hub::exe_dir() + "retrorun.exe", args, hub::exe_dir(), {}, ri);
            break;
        }
        case hub::System::NDS: {   // melonDS DS core; no --resume (a stale auto state could overwrite a save synced from another PC)
            std::string core = find_core("melondsds");
            if (core.empty()) { set_notice("Emulator core for Nintendo DS not found (cores folder)."); return; }
            std::string args = "--core " + quote(core) + " --rom " + quote(e.path) + " --system nds --save-dir " + quote(battery) +
                               " --state-dir " + quote(states) + title_arg(e.title) + save_stem_arg(e) + controls_arg("retro", e.key);
            args += hubui::retro_extra_args(e.system, gs_values_for(e));
            spawn(hub::exe_dir() + "retrorun.exe", args, hub::exe_dir(), {}, ri);
            break;
        }
        case hub::System::GC:
        case hub::System::N3DS:
        case hub::System::WiiU:
            launch_external(e, ri, battery);
            break;
        default: set_notice("Unsupported system."); break;
    }
}

