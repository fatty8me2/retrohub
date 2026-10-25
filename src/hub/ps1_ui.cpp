#include "app.h"

#include "ps1.h"

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

void draw_ps1_dialogs() {
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
