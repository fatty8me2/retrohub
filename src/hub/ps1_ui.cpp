#include "app.h"

#include "ps1.h"

/* ---------- PlayStation 1 (retrorun + Beetle PSX): launch, BIOS discovery / picker, settings block. See docs/EMULATORS.md ---------- */

namespace {

struct Ps1Ask {
    bool open = false;
    std::string key;       // game to start once a BIOS was chosen
    std::string warning;   // last pick was not a known dump
} ASK;

std::string staging_dir() { return hub::data_dir() + "system\\ps1"; }

std::vector<hub::ps1::BiosFound> find_all() {
    const char* home = std::getenv("USERPROFILE");
    const char* appdata = std::getenv("APPDATA");
    return hub::ps1::find_bios(hub::ps1::bios_search_dirs(A.st.rom_folders, A.st.ps1_bios, staging_dir(), home ? home : "", hub::exe_dir(), appdata ? appdata : ""));
}

}  // namespace

/* Number of local pads (multitap "auto"). */
static int local_pad_count() { return (int)A.pads.size(); }

void ps1_play(const hub::GameEntry& e, const RunInfo& ri, const std::string& battery, const std::string& states) {
    if (e.issue == "extract" && hub::extracted_3ds_path(e).empty()) {   // zipped set: ask once, unpack, then start
        UNZ.key = e.key;
        UNZ.state = UnzipJob::Asking;
        return;
    }
    std::string core = find_core("beetle_psx");
    if (core.empty()) { set_notice("The PlayStation emulator core (beetle_psx_libretro.dll) was not found in the cores folder."); return; }
    std::vector<hub::ps1::BiosFound> bios = find_all();
    std::string sysdir = staging_dir();
    if (bios.empty() && !A.st.ps1_openbios) {
        ASK.open = true;
        ASK.key = e.key;
        ASK.warning.clear();
        return;
    }
    if (!bios.empty()) hub::ps1::stage_bios(bios, sysdir);
    else { std::error_code ec; std::filesystem::create_directories(std::filesystem::u8path(sysdir), ec); }
    std::string game = e.issue == "extract" ? hub::extracted_3ds_path(e) : e.path;
    hub::gs::Values gsv = gs_values_for(e);
    std::string args = "--core " + quote(core) + " --rom " + quote(game) + " --system ps1 --save-dir " + quote(battery) + " --state-dir " + quote(states) +
                       " --system-dir " + quote(sysdir) + title_arg(e.title) + save_stem_arg(e) + controls_arg("ps1", e.key);
    args += hubui::retro_extra_args(e.system, gsv);
    auto mt = gsv.find("ps1.multitap");
    if ((mt == gsv.end() || mt->second == "auto") && local_pad_count() >= 3) args += " --opt beetle_psx_enable_multitap_port1=enabled";
    // No real BIOS: the core falls back to its bundled OpenBIOS (fewer games run).
    spawn(hub::exe_dir() + "retrorun.exe", args, hub::exe_dir(), {}, ri);
}

/* Online (launch_online.cpp): retrorun arguments for a PlayStation room, or false + `err`. `net` is the shared " --net ... --ports ..." tail,
 * `ports` the plan's port owners ("0,1,2,3": -1 = nobody). Every PC must run the same BIOS (retrorun compares a fingerprint and refuses a
 * mismatch), so a real BIOS is staged when one is found and OpenBIOS is the fallback only when no PC needs a prompt. The multitap follows
 * the plan: a third or fourth port means the multitap is on, whatever the local setting says (the host's Sync value still forces it on). */
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
    std::string game = e.issue == "extract" ? hub::extracted_3ds_path(e) : e.path;
    *args = "--core " + quote(core) + " --rom " + quote(game) + " --system ps1 --save-dir " + quote(tmp_dir) + " --state-dir " + quote(tmp_dir) +
            " --system-dir " + quote(sysdir) + net + title_arg(e.title) + save_stem_arg(e) + controls_arg("ps1", e.key);
    if (share_save) *args += " --share-host-save";
    *args += " --rollback --rb-pred 6";   // GGPO-style rollback (the core is verified with SyncTest); retrorun falls back to lockstep when the core is not the RetroHub build
    *args += hubui::retro_extra_args(e.system, gsv);
    return true;
}

void draw_ps1_dialogs() {
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
        if (button("Choose BIOS file...", ImVec2(220 * S, 44 * S), true, true, ui::Icon::Folder)) {
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
        ImGui::SameLine();
        if (button("Use built-in OpenBIOS", ImVec2(220 * S, 44 * S))) {
            A.st.ps1_openbios = true;
            mark_dirty();
            ASK.open = false;
            if (const hub::GameEntry* g = find_entry(ASK.key)) { hub::GameEntry copy = *g; play_entry(copy); }
        }
        ImGui::SameLine();
        if (button("Cancel", ImVec2(140 * S, 44 * S))) ASK.open = false;
        if (!ASK.open) ImGui::CloseCurrentPopup();
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
    if (button("Choose PlayStation BIOS...", ImVec2(0, 0), false, true, ui::Icon::Folder)) {
        std::string p = pick_path(A.hwnd, false, L"PlayStation BIOS", L"*.bin");
        if (!p.empty()) { A.st.ps1_bios = p; mark_dirty(); looked_at = -100; }
    }
    if (!A.st.ps1_bios.empty()) {
        ImGui::SameLine();
        if (button("Use automatic##ps1bios", ImVec2(0, 0))) { A.st.ps1_bios.clear(); mark_dirty(); looked_at = -100; }
    }
    if (ImGui::Checkbox("Use the built-in OpenBIOS when no BIOS is found", &A.st.ps1_openbios)) mark_dirty();
}
