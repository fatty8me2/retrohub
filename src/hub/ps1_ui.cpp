#include "app.h"

#include "ps1.h"
#include "ps2_online.h"
#include "pcsx2.h"

#include <fstream>

/* ---------- PlayStation 1 (retrorun + Beetle PSX): launch, BIOS discovery / picker, settings block. See docs/EMULATORS.md ---------- */

namespace {

struct Ps1Ask {
    bool open = false;
    std::string key;       // game to start once a BIOS was chosen
    std::string warning;   // last pick was not a known dump
    bool focused = false;
} ASK;

struct Ps2Ask {
    bool open = false;
    std::string key;
    std::string warning;
} ASK2;

std::string staging_dir() { return hub::data_dir() + "system\\ps1"; }

std::vector<hub::ps1::BiosFound> find_all() {
    const char* home = std::getenv("USERPROFILE");
    const char* appdata = std::getenv("APPDATA");
    return hub::ps1::find_bios(hub::ps1::bios_search_dirs(A.st.rom_folders, A.st.ps1_bios, staging_dir(), home ? home : "", hub::exe_dir(), appdata ? appdata : ""));
}

/* A BIOS folder chosen with the pad folder browser. Returns true when a PS1 BIOS lives there. */
bool use_ps1_bios_dir(const std::string& dir) {
    std::vector<hub::ps1::BiosFound> found = hub::ps1::find_bios({dir});
    if (found.empty()) return false;
    A.st.ps1_bios = dir;
    mark_dirty();
    return true;
}

}  // namespace

void ps1_bios_ask_shot() { ASK.open = true; ASK.key.clear(); ASK.warning.clear(); }

void ps2_bios_ask(const std::string& key) {
    ASK2.open = true;
    ASK2.key = key;
    ASK2.warning.clear();
}

/* Number of local pads (multitap "auto"). */
static int local_pad_count() { return (int)A.pads.size(); }

static std::string sanitize(std::string k) {
    for (char& c : k) if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) c = '_';
    return k;
}
/* Disc-swap channel for a running game: retrorun watches this file (--disc-ctl) and inserts the next disc whenever it changes. */
static std::string swap_file(const std::string& key) { return staging_dir() + "/m3u/" + sanitize(key) + ".swap"; }

/* A numbered multi-disc set ("Game (Disc 1).cue" + "Game (Disc 2).cue" in one folder) becomes an auto-generated .m3u (named after the chosen
 * file, so its save stays where it was); a real .m3u is passed as it is. Returns the file to hand the core. */
static std::string with_m3u(const hub::GameEntry& e, const std::string& game) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path gp = fs::u8path(game);
    std::vector<std::string> sib;
    for (auto& de : fs::directory_iterator(gp.parent_path(), ec))
        if (!ec && de.is_regular_file(ec)) sib.push_back(de.path().filename().u8string());
    std::vector<std::string> set = hub::ps1::disc_set(gp.filename().u8string(), sib);
    if (set.empty()) return game;
    for (std::string& n : set) n = (gp.parent_path() / fs::u8path(n)).u8string();
    fs::path m = fs::u8path(staging_dir() + "/m3u/" + sanitize(e.key));
    fs::create_directories(m, ec);
    m /= gp.stem().concat(".m3u");
    std::string text = hub::ps1::make_m3u(set);
    std::ofstream f(m, std::ios::binary);
    if (!f) return game;
    f << text;
    return m.u8string();
}

/* " --disc-ctl <file>" (the file is created first so the runner's watch starts from it). */
static std::string disc_ctl_arg(const std::string& key) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(swap_file(key)).parent_path(), ec);
    std::ofstream f(std::filesystem::u8path(swap_file(key)), std::ios::binary | std::ios::trunc);
    f << "0";
    return " --disc-ctl " + quote(swap_file(key));
}

bool ps1_swap_available(const std::string& key) {
    return A.proc && A.run.key == key && std::filesystem::exists(std::filesystem::u8path(swap_file(key)));
}

bool ps1_swap_disc(const std::string& key) {
    if (!ps1_swap_available(key)) return false;
    static unsigned n = 0;
    std::ofstream f(std::filesystem::u8path(swap_file(key)), std::ios::binary | std::ios::trunc);
    f << ++n << " " << GetTickCount64();   // any change of mtime/size counts
    return (bool)f;
}

bool ps1_real_bios_present() { return !find_all().empty(); }

void ps1_play(const hub::GameEntry& e, const RunInfo& ri, const std::string& battery, const std::string& states) {
    std::string core = find_core("beetle_psx");
    if (core.empty()) { set_notice("The PlayStation emulator core (beetle_psx_libretro.dll) was not found in the cores folder."); return; }
    std::vector<hub::ps1::BiosFound> bios = find_all();
    std::string sysdir = staging_dir();
    if (bios.empty() && !A.st.ps1_openbios) {   // BIOS first, so nothing big is unpacked for a game that cannot start yet
        ASK.open = true;
        ASK.key = e.key;
        ASK.warning.clear();
        return;
    }
    if (e.issue == "extract" && hub::extracted_3ds_path(e).empty()) {   // archived set: ask once, unpack, then start
        UNZ.key = e.key;
        UNZ.state = UnzipJob::Asking;
        return;
    }
    if (!bios.empty()) hub::ps1::stage_bios(bios, sysdir);
    else { std::error_code ec; std::filesystem::create_directories(std::filesystem::u8path(sysdir), ec); }
    std::string game = with_m3u(e, e.issue == "extract" ? hub::extracted_3ds_path(e) : e.path);
    hub::gs::Values gsv = gs_values_for(e);
    std::string args = "--core " + quote(core) + " --rom " + quote(game) + " --system ps1 --save-dir " + quote(battery) + " --state-dir " + quote(states) +
                       " --system-dir " + quote(sysdir) + title_arg(e.title) + save_stem_arg(e) + controls_arg("ps1", e.key) + disc_ctl_arg(e.key);
    args += hubui::retro_extra_args(e.system, gsv);
    args += real_boot_retro_args(e, real_boot_plan(e), nullptr);   // PlayStation logo + sound through the player's own BIOS (skip_bios off)
    auto mt = gsv.find("ps1.multitap");
    if ((mt == gsv.end() || mt->second == "auto") && local_pad_count() >= 3) args += " --opt beetle_psx_enable_multitap_port1=enabled";
    args += cheats_launch_args(e, false, true, nullptr);
    args += qm_args(ri, e, false);
    // No real BIOS: the core falls back to its bundled OpenBIOS (fewer games run).
    spawn(hub::exe_dir() + "retrorun.exe", args, hub::exe_dir(), {}, ri);
}

static void draw_ps2_ask() {
    if (!ASK2.open) return;
    bool open = false;
    emu_modal_begin("PlayStation 2 BIOS", &open);
    if (!open) return;
    if (ui::modal_back()) ASK2.open = false;
    ImGui::TextWrapped("PS2 needs a BIOS file.");
    ImGui::Dummy(ImVec2(0, 2 * S));
    text_wrapped_disabled(hubemu::pcsx2_bios_missing_text());
    if (!ASK2.warning.empty()) { ImGui::Dummy(ImVec2(0, 2 * S)); ImGui::TextColored(ImColor(ui::WARN), "%s", ASK2.warning.c_str()); }
    ImGui::Dummy(ImVec2(0, 8 * S));
    static bool seen2 = false;
    if (!seen2) { ImGui::SetKeyboardFocusHere(0); seen2 = true; }   // a pad lands on "Choose", not on Cancel
    const bool choose = button("Choose BIOS folder...", ImVec2(240 * S, 44 * S), true, true, ui::Icon::Folder);
    std::string dir = osk::folder("ps2bios", choose);
    if (!dir.empty()) {
        hubemu::Bios b = hubemu::pcsx2_find_bios(hubemu::pcsx2_bios_search_dirs(dir, A.st.rom_folders));
        if (b.file.empty()) ASK2.warning = "No PS2 BIOS (scph*.bin) was found in that folder. Pick the folder that holds it.";
        else {
            A.st.ps2_bios_dir = dir;
            mark_dirty();
            ASK2.open = false;
            if (const hub::GameEntry* g = find_entry(ASK2.key)) { hub::GameEntry copy = *g; play_entry(copy); }
        }
    }
    ImGui::SameLine();
    if (button("Cancel", ImVec2(140 * S, 44 * S))) ASK2.open = false;
    if (!ASK2.open) { ImGui::CloseCurrentPopup(); seen2 = false; }
    ImGui::EndPopup();
}

/* Online (launch_online.cpp): retrorun arguments for a PlayStation room, or false + `err`. `net` is the shared " --net ... --ports ..." tail,
 * `ports` the plan's port owners ("0,1,2,3": -1 = nobody). Every PC must run the same BIOS (retrorun compares a fingerprint and refuses a
 * mismatch), so a real BIOS is staged when one is found and OpenBIOS is the fallback only when no PC needs a prompt. The multitap follows
 * the plan: a third or fourth port means the multitap is on, whatever the local setting says (the host's Sync value still forces it on). */
/* Why this PC cannot start a PlayStation room game ("" = fine): the room READY/LAUNCH buttons show it, so a guest without a BIOS
 * is told up front instead of failing after the host launches. Same checks as ps1_online_args; cached 2 s (touches the disk). */
std::string ps1_online_block() {
    static double at = -10; static std::string cached;
    double now = ImGui::GetTime();
    if (now - at < 2.0) return cached;
    at = now;
    cached.clear();
    if (find_core("beetle_psx").empty()) cached = "The PlayStation emulator core (beetle_psx_libretro.dll) is missing from the cores folder.";
    else if (find_all().empty() && !A.st.ps1_openbios) cached = "Your PC has no PlayStation BIOS (scph5501.bin) - add it, or turn on OpenBIOS, in Settings > Playing. Everyone in the room needs the same setup.";
    return cached;
}

bool ps1_online_args(const hub::GameEntry& e, const std::string& net, const std::string& ports, const std::string& tmp_dir, bool share_save,
                     hub::gs::Values gsv, std::string* args, std::string* err) {
    if (e.issue == "extract" && hub::extracted_3ds_path(e).empty()) { *err = "Unpacking " + e.title + " first - try again when it is done."; return false; }
    std::string core = find_core("beetle_psx");
    if (core.empty()) { *err = "The PlayStation emulator core (beetle_psx_libretro.dll) was not found in the cores folder."; return false; }
    std::vector<hub::ps1::BiosFound> bios = find_all();
    std::string sysdir = staging_dir();
    if (bios.empty() && !A.st.ps1_openbios) {
        *err = "PlayStation online needs a BIOS file (scph5501.bin) on every PC, or the built-in OpenBIOS on every PC. Set it up in Settings > Playing.";
        return false;
    }
    if (!bios.empty()) hub::ps1::stage_bios(bios, sysdir);
    else { std::error_code ec; std::filesystem::create_directories(std::filesystem::u8path(sysdir), ec); }
    const bool tap = hub::ps1::ports_need_multitap(ports);
    if (tap) gsv["ps1.multitap"] = "on";
    else if (gsv["ps1.multitap"] != "on") gsv["ps1.multitap"] = "off";
    std::string game = with_m3u(e, e.issue == "extract" ? hub::extracted_3ds_path(e) : e.path);
    *args = "--core " + quote(core) + " --rom " + quote(game) + " --system ps1 --save-dir " + quote(tmp_dir) + " --state-dir " + quote(tmp_dir) +
            " --system-dir " + quote(sysdir) + net + title_arg(e.title) + save_stem_arg(e) + controls_arg("ps1", e.key) + disc_ctl_arg(e.key);
    if (share_save) *args += " --share-host-save";
    *args += " --rollback --rb-pred 6";   // GGPO-style rollback (the core is verified with SyncTest); retrorun falls back to lockstep when the core is not the RetroHub build
    *args += hubui::retro_extra_args(e.system, gsv);
    return true;
}

void draw_ps1_dialogs() {
    draw_missing_rom_dialog();
    draw_ps2_ask();
    if (!ASK.open) return;
    bool open = false;
    emu_modal_begin("PlayStation BIOS", &open);
    if (open) {
        ImGui::TextWrapped("PS1 needs a BIOS file (scph5501.bin).");
        ImGui::Dummy(ImVec2(0, 2 * S));
        text_wrapped_disabled("RetroHub does not include one. Choose your own dump (scph5501.bin for US, scph5502.bin for Europe, scph5500.bin for Japan; "
                              "scph1001 / scph7001 / scph7003 also work). Putting the file in a BIOS folder next to your games (for example Desktop\\Everything\\BIOS) "
                              "is picked up automatically.");
        if (!ASK.warning.empty()) { ImGui::Dummy(ImVec2(0, 2 * S)); ImGui::TextColored(ImColor(ui::WARN), "%s", ASK.warning.c_str()); }
        ImGui::Dummy(ImVec2(0, 8 * S));
        if (ui::modal_back()) ASK.open = false;
        const bool pad = A.pad_mode;
        if (!ASK.focused) { ImGui::SetKeyboardFocusHere(0); ASK.focused = true; }   // a pad lands on "Choose", not on a later button
        const bool choose = button(pad ? "Choose BIOS folder..." : "Choose BIOS file...", ImVec2(220 * S, 44 * S), true, true, ui::Icon::Folder);
        std::string picked;
        if (pad) picked = osk::folder("ps1bios", choose);
        else if (choose) {
            std::string p = pick_path(A.hwnd, false, L"PlayStation BIOS", L"*.bin");
            if (!p.empty()) {
                std::vector<hub::ps1::BiosFound> found = hub::ps1::find_bios({std::filesystem::u8path(p).parent_path().u8string()});
                bool is_it = false, known = false;
                for (auto& f : found)
                    if (_stricmp(std::filesystem::u8path(f.path).filename().u8string().c_str(), std::filesystem::u8path(p).filename().u8string().c_str()) == 0) { is_it = true; known = f.known != nullptr; }
                if (!is_it) ASK.warning = "That does not look like a PlayStation BIOS (it should be a 512 KB scph*.bin file).";
                else {
                    A.st.ps1_bios = p;
                    mark_dirty();
                    if (!known) push_toast("That BIOS has an unrecognised checksum; trying it anyway.", ui::WARN, 8.0);
                    ASK.open = false;
                    if (const hub::GameEntry* g = find_entry(ASK.key)) { hub::GameEntry copy = *g; play_entry(copy); }
                }
            }
        }
        if (!picked.empty()) {
            if (!use_ps1_bios_dir(picked)) ASK.warning = "No PlayStation BIOS (scph*.bin) was found in that folder. Pick the folder that holds it.";
            else {
                ASK.open = false;
                if (const hub::GameEntry* g = find_entry(ASK.key)) { hub::GameEntry copy = *g; play_entry(copy); }
            }
        }
        ImGui::SameLine();
        if (button("Use built-in OpenBIOS", ImVec2(220 * S, 44 * S))) {
            A.st.ps1_openbios = true;
            mark_dirty();
            ASK.open = false;
            if (const hub::GameEntry* g = find_entry(ASK.key)) { hub::GameEntry copy = *g; play_entry(copy); }
        }
        if (button("Cancel", ImVec2(140 * S, 44 * S))) ASK.open = false;
        if (!ASK.open) { ImGui::CloseCurrentPopup(); ASK.focused = false; }
        ImGui::EndPopup();
    }
}

/* Settings > Playing: BIOS status + picker. */
void ps1_settings_ui() {
    static std::vector<hub::ps1::BiosFound> found;
    static double looked_at = -100;
    static std::string looked_key;
    std::string key = A.st.ps1_bios + "|" + std::to_string(A.st.rom_folders.size());
    if (ImGui::GetTime() - looked_at > 3.0 || looked_key != key) {   // touches the disk: refresh every 3 s
        found = find_all();
        looked_at = ImGui::GetTime();
        looked_key = key;
    }
    if (found.empty()) text_wrapped_disabled(A.st.ps1_openbios ? "PlayStation BIOS: none found - using the built-in OpenBIOS (fewer games run)"
                                                              : "PlayStation BIOS: not found - PS1 needs a BIOS file (scph5501.bin). Put it in a BIOS folder next to your games, or choose it below.");
    else {
        const hub::ps1::BiosFound& b = found[0];
        text_wrapped_disabled("PlayStation BIOS: " + b.path + (b.known ? std::string("  (") + b.known->desc + ")" : std::string("  (unrecognised checksum)")));
    }
    {
        const bool choose = button(A.pad_mode ? "Choose PlayStation BIOS folder..." : "Choose PlayStation BIOS...", ImVec2(0, 0), false, true, ui::Icon::Folder);
        if (A.pad_mode) {
            std::string d = osk::folder("ps1bios_set", choose);
            if (!d.empty()) { if (!use_ps1_bios_dir(d)) push_toast("No PlayStation BIOS (scph*.bin) in that folder.", ui::WARN, 6.0); looked_at = -100; }
        } else if (choose) {
            std::string p = pick_path(A.hwnd, false, L"PlayStation BIOS", L"*.bin");
            if (!p.empty()) { A.st.ps1_bios = p; mark_dirty(); looked_at = -100; }
        }
    }
    if (!A.st.ps1_bios.empty()) {
        ImGui::SameLine();
        if (button("Use automatic##ps1bios", ImVec2(0, 0))) { A.st.ps1_bios.clear(); mark_dirty(); looked_at = -100; }
    }
    if (ImGui::Checkbox("Use the built-in OpenBIOS when no BIOS is found", &A.st.ps1_openbios)) mark_dirty();
}

/* ---------- PlayStation 2 online (retrorun + LRPS2): core download, readiness, launch arguments. docs/PS2_ONLINE.md ---------- */

namespace {
hubemu::Installer LR_DL;          // on-demand download of pcsx2_libretro.dll (quiet: progress shows in the room banner, no modal)
double lr_failed_at = -1000;
std::string lr_error;

std::string lrps2_core() {
    hubemu::Install in = hubemu::find(hubemu::Kind::Lrps2, "", "");
    if (!in.exe.empty()) return in.exe;
    return find_core("pcsx2");   // dev builds / manual installs: pcsx2_libretro.dll in the cores folder
}
hubemu::Bios ps2_bios_now() { return hubemu::pcsx2_find_bios(hubemu::pcsx2_bios_search_dirs(A.st.ps2_bios_dir, A.st.rom_folders)); }
}  // namespace

/* Why this PC cannot start a PS2 room game ("" = fine). Starts the core download the first time it is asked (3.6 MB, hash-pinned). */
std::string ps2_online_block(const hub::GameEntry& e) {
    static double at = -10; static std::string cached; static std::string cached_key;
    bool ok = false;
    if (LR_DL.poll(&ok) && !ok) { std::lock_guard<std::mutex> lk(LR_DL.mu); lr_error = LR_DL.error; lr_failed_at = ImGui::GetTime(); }
    double now = ImGui::GetTime();
    const bool dl = LR_DL.state == hubemu::Installer::Working;
    if (!dl && now - at < 2.0 && cached_key == e.key) return cached;
    at = now;
    cached_key = e.key;
    hubps2::OnlineState st;
    st.core_present = !lrps2_core().empty();
    st.core_downloading = dl;
    st.percent = LR_DL.phase > 0 ? 100 : (LR_DL.total ? (int)(LR_DL.got * 100 / LR_DL.total) : 0);
    st.download_error = lr_error;
    st.bios_found = !ps2_bios_now().file.empty();
    st.needs_unpack = e.issue == "extract" && hub::extracted_3ds_path(e).empty();
    st.title = e.title;
    if (!g_headless && !st.core_present && !dl && (lr_error.empty() || now - lr_failed_at > 30.0)) {   // first ask, or a retry 30 s after a failure
        lr_error.clear();
        LR_DL.start(hubemu::Kind::Lrps2, hubemu::managed_root());
        st.core_downloading = true;
    }
    cached = hubps2::online_block_reason(st);
    return cached;
}

/* retrorun arguments for a PS2 room. `tmp_dir` is the throwaway session dir (netplay\ps2, emptied by the caller); the BIOS and the host's memory
 * card are staged under tmp_dir\sys. The host's PCSX2 card is a plain 8 MB .ps2 (8,650,752 bytes, ECC) that LRPS2 reads as Mcd001.ps2. */
bool ps2_online_args(const hub::GameEntry& e, const std::string& net, const std::string& tmp_dir, bool share_save, bool is_host_card,
                     std::string* args, std::string* err) {
    if (e.issue == "extract" && hub::extracted_3ds_path(e).empty()) { *err = "Unpacking " + e.title + " first - try again when it is done."; return false; }
    std::string core = lrps2_core();
    if (core.empty()) { *err = "The PlayStation 2 online core is not downloaded yet - wait for the room's status line to clear."; return false; }
    hubemu::Bios bios = ps2_bios_now();
    if (bios.file.empty()) { *err = hubemu::pcsx2_bios_missing_text(); return false; }
    std::string sysdir = tmp_dir + "\\sys";
    std::string bios_dir = sysdir + "\\" + hubps2::staged_bios_dir_rel();
    make_dirs(bios_dir);
    make_dirs(sysdir + "\\pcsx2\\memcards");
    std::error_code ec;
    std::filesystem::copy_file(std::filesystem::u8path(bios.dir + "\\" + bios.file), std::filesystem::u8path(bios_dir + "\\" + bios.file), std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) { *err = "Could not stage the PlayStation 2 BIOS: " + ec.message(); return false; }
    if (share_save && is_host_card) {   // the host's vault card is COPIED; retrorun sends it to every guest, nothing is written back
        std::filesystem::path card = std::filesystem::path(hub::saves::battery_dir(sys_short(e), e.key)) / std::filesystem::u8path(hubps2::vault_card_rel(hubemu::pcsx2_safe_key(e.key)));
        if (std::filesystem::is_regular_file(card, ec)) std::filesystem::copy_file(card, std::filesystem::u8path(sysdir + "\\" + hubps2::staged_card_rel()), std::filesystem::copy_options::overwrite_existing, ec);
        else set_notice("No memory card for " + e.title + " on this PC yet - everyone starts with a blank card.", 8.0);
    }
    const hub::GameInfo* cat = hub::find_game(e.catalog_id);
    std::string serial = !e.serial.empty() ? e.serial : (cat && !cat->serials.empty() ? cat->serials[0] : "");
    std::string game = e.issue == "extract" ? hub::extracted_3ds_path(e) : e.path;
    *args = "--core " + quote(core) + " --rom " + quote(game) + " --save-dir " + quote(tmp_dir) + " --state-dir " + quote(tmp_dir) + " --system-dir " + quote(sysdir) +
            net + title_arg(e.title) + save_stem_arg(e) + controls_arg("ps1", e.key);
    *args += hubps2::retrorun_setup_args(bios.file, serial, share_save);
    return true;
}
