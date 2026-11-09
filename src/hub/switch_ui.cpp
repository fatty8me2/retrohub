#include "app.h"

#include "lichii.h"
#include "nx_runner.h"

/* ---------- Nintendo Switch / Switch 2 (Lichii): the keys + firmware prompt before a game starts, and the Settings block.
 * RetroHub never ships or downloads keys or firmware: the player copies their own (dumped from a console they own) from a file / folder they
 * pick, or from an Eden / yuzu / Ryujinx / Citron / Suyu / Sudachi install already on this PC (only read). docs/EMULATORS.md (Switch). ---------- */

namespace {

struct SwitchAsk {
    bool open = false;
    hubemu::Kind kind = hubemu::Kind::Lichii;   // which emulator the prompt sets up (Lichii, or Fritchii when that is the runner)
    std::string key;          // game to start once set up ("" = opened from Settings / a screenshot)
    std::string message;      // last import result
    bool message_bad = false;
    bool focused = false;
} ASK;

bool g_skip_firmware = false;   // "Play without firmware" for this RetroHub session

struct Look {
    hubemu::Kind kind = hubemu::Kind::Lichii;
    double at = -100;
    hubemu::Install inst;
    hubemu::NxSetup st;
    std::vector<hubemu::NxSource> sources;
};
Look LOOK_L, LOOK_F;   // Lichii and Fritchii: the same setup code, each on its own install folder
Look& look_of(hubemu::Kind k) { LOOK_F.kind = hubemu::Kind::Fritchii; return k == hubemu::Kind::Fritchii ? LOOK_F : LOOK_L; }
Look& LK() { return look_of(ASK.kind); }   // the prompt's emulator

void refresh(Look& L, bool force = false) {
    if (!force && ImGui::GetTime() - L.at < 2.0) return;   // touches the disk
    L.at = ImGui::GetTime();
    L.inst = hubemu::find(L.kind, "", "");
    L.st = L.inst.dir.empty() ? hubemu::NxSetup{} : hubemu::lichii_setup_status(L.inst.dir);
    const char* ad = std::getenv("APPDATA");
    L.sources = hubemu::lichii_find_sources(ad ? ad : "", A.st.rom_folders);
    if (L.kind == hubemu::Kind::Fritchii) {   // Fritchii has its own user folder: the Lichii install is one more place its keys and firmware can come from (copied, only read)
        const hubemu::Install li = hubemu::find(hubemu::Kind::Lichii, "", "");
        if (!li.dir.empty()) {
            const hubemu::NxSetup ls = hubemu::lichii_setup_status(li.dir);
            hubemu::NxSource s;
            s.label = "Lichii";
            if (ls.prod_keys) s.keys_dir = hubemu::lichii_user_dir(li.dir) + "\\keys";
            if (ls.firmware) s.firmware_dir = hubemu::lichii_user_dir(li.dir) + "\\nand\\system\\Contents\\registered";
            if (!s.keys_dir.empty() || !s.firmware_dir.empty()) L.sources.insert(L.sources.begin(), s);
        }
    }
}

std::string status_line(const hubemu::NxSetup& st) {
    std::string s = std::string("Keys: ") + (st.prod_keys ? "prod.keys found" : "missing");
    if (st.prod_keys) s += st.title_keys ? " (+ title.keys)" : "";
    s += std::string("   Firmware: ") + (st.firmware ? (st.firmware_version.empty() ? std::string("installed") : st.firmware_version) : std::string("missing"));
    return s;
}

void report(Look& L, const std::string& err, const std::string& ok_text) {
    ASK.message = err.empty() ? ok_text : err;
    ASK.message_bad = !err.empty();
    refresh(L, true);
}

/* The import buttons, shared by the prompt and Settings. `id` keeps the picker ids apart. */
void import_buttons(Look& LOOK, const char* id) {
    const std::string nm = hubemu::name(LOOK.kind);
    if (LOOK.inst.dir.empty()) { text_wrapped_disabled(nm + " is not downloaded yet: start a Switch game with it once, then add your keys and firmware here."); return; }
    const std::string dir = LOOK.inst.dir;
    for (size_t i = 0; i < LOOK.sources.size(); ++i) {
        const hubemu::NxSource& s = LOOK.sources[i];
        std::string what = !s.keys_dir.empty() && !s.firmware_dir.empty() ? "keys + firmware" : !s.keys_dir.empty() ? "keys" : "firmware";
        std::string label = "Copy " + what + " from " + s.label + "##nxsrc" + std::string(id) + std::to_string(i);
        if (button(label.c_str(), ImVec2(0, 0), i == 0 && !LOOK.st.prod_keys, true, ui::Icon::Folder)) {
            std::string err;
            if (!s.keys_dir.empty()) err = hubemu::lichii_import_keys(dir, s.keys_dir);
            if (err.empty() && !s.firmware_dir.empty()) err = hubemu::lichii_import_firmware(dir, s.firmware_dir);
            report(LOOK, err, "Copied " + what + " from " + s.label + ".");
        }
    }
    {
        std::string p = osk::file(id[0] == 'a' ? "nxkeys_ask" : "nxkeys_set", button((std::string("Choose prod.keys...##") + id).c_str(), ImVec2(0, 0), LOOK.sources.empty() && !LOOK.st.prod_keys, true, ui::Icon::Folder), {".keys"});
        if (!p.empty()) report(LOOK, hubemu::lichii_import_keys(dir, p), "Keys copied.");
    }
    // the next button goes on this row only when it fits (a narrow dialog / big UI scale would cut it off at the edge)
    auto same_line_if_fits = [](const char* next_label) {
        const ImGuiStyle& sty = ImGui::GetStyle();
        const float need = ImGui::CalcTextSize(next_label, nullptr, true).x + ImGui::GetFontSize() * 2.2f + sty.FramePadding.x * 2.f + sty.ItemSpacing.x;
        if (ImGui::GetItemRectMax().x + need <= ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x) ImGui::SameLine();
    };
    same_line_if_fits("Choose firmware folder...");
    {
        std::string p = osk::folder(id[0] == 'a' ? "nxfwd_ask" : "nxfwd_set", button((std::string("Choose firmware folder...##") + id).c_str(), ImVec2(0, 0), false, true, ui::Icon::Folder));
        if (!p.empty()) report(LOOK, hubemu::lichii_import_firmware(dir, p), "Firmware installed.");
    }
    same_line_if_fits("Firmware .zip...");
    {
        std::string p = osk::file(id[0] == 'a' ? "nxfwz_ask" : "nxfwz_set", button((std::string("Firmware .zip...##") + id).c_str(), ImVec2(0, 0), false, true, ui::Icon::Folder), {".zip"});
        if (!p.empty()) report(LOOK, hubemu::lichii_import_firmware(dir, p), "Firmware installed.");
    }
    if (!ASK.message.empty()) ImGui::TextColored(ImColor(ASK.message_bad ? ui::WARN : ui::GOOD), "%s", ASK.message.c_str());
}

void start_game() {
    ASK.open = false;
    if (const hub::GameEntry* g = find_entry(ASK.key)) { hub::GameEntry copy = *g; play_entry(copy); }
}

}  // namespace

/* launch.cpp: true when the game may start now; otherwise the prompt is opened for it. */
bool switch_ready_or_ask(const hub::GameEntry& e, const std::string& install_dir, hubemu::Kind kind) {
    hubemu::NxSetup st = hubemu::lichii_setup_status(install_dir);
    const bool keys_ok = st.prod_keys || !hubemu::lichii_needs_keys(e.path);
    if (keys_ok && (st.firmware || g_skip_firmware)) return true;
    ASK.open = true;
    ASK.kind = kind;
    ASK.key = e.key;
    ASK.message.clear();
    refresh(look_of(kind), true);
    return false;
}

void switch_setup_ask_shot() { ASK.open = true; ASK.kind = hubemu::Kind::Lichii; ASK.key.clear(); ASK.message.clear(); refresh(LOOK_L, true); }

void draw_switch_dialogs() {
    if (!ASK.open) return;
    bool open = false;
    emu_modal_begin("Switch keys and firmware", &open);
    if (!open) return;
    Look& LOOK = LK();
    refresh(LOOK);
    const hub::GameEntry* g = ASK.key.empty() ? nullptr : find_entry(ASK.key);
    const bool needs_keys = g ? hubemu::lichii_needs_keys(g->path) : true;
    const bool keys_ok = LOOK.st.prod_keys || !needs_keys;
    if (ui::modal_back()) ASK.open = false;
    ImGui::TextWrapped("%s", needs_keys ? "Switch games need your own keys (prod.keys) and firmware, dumped from a Switch you own."
                                        : "This game needs no keys, but most games also want the Switch firmware, dumped from a Switch you own.");
    ImGui::Dummy(ImVec2(0, 2 * S));
    text_wrapped_disabled("RetroHub does not include or download them. Copy them once from your own dump (a prod.keys file and a firmware folder or .zip) "
                          "or from an emulator already on this PC; they are kept in RetroHub's own " + std::string(hubemu::name(ASK.kind)) + " folder.");
    ImGui::Dummy(ImVec2(0, 4 * S));
    ImGui::TextUnformatted(status_line(LOOK.st).c_str());
    ImGui::Dummy(ImVec2(0, 4 * S));
    if (!ASK.focused) { ImGui::SetKeyboardFocusHere(0); ASK.focused = true; }   // a pad lands on the first import button
    import_buttons(LOOK, "ask");
    ImGui::Dummy(ImVec2(0, 8 * S));
    if (g && keys_ok && LOOK.st.firmware) { if (button("Play", ImVec2(160 * S, 44 * S), true, true)) start_game(); ImGui::SameLine(); }
    else if (g && keys_ok) {
        if (button("Play without firmware", ImVec2(240 * S, 44 * S))) { g_skip_firmware = true; start_game(); }
        ImGui::SameLine();
    }
    if (button(g ? "Cancel" : "Close", ImVec2(140 * S, 44 * S))) ASK.open = false;
    if (!ASK.open) { ImGui::CloseCurrentPopup(); ASK.focused = false; }
    ImGui::EndPopup();
}

/* Settings > Playing: Fritchii (opt-in) and which program Switch games play with. */
static void fritchii_settings_ui() {
    using hubemu::Kind;
    refresh(look_of(Kind::Fritchii));   // (sets LOOK_F.kind)
    const hubemu::Install& fr = LOOK_F.inst;
    const hubemu::Release& rel = hubemu::release(Kind::Fritchii);
    ImGui::Dummy(ImVec2(0, 4 * S));
    ImGui::TextUnformatted("Fritchii");
    text_wrapped_disabled("Fritchii: an Ocarina of Time-focused version of Lichii by Frizti. It has its own folder, keys and settings copy; your saves are shared with Lichii and kept up to date both ways. Lichii stays the default.");
    const bool busy = EMU.state != hubemu::Installer::Idle || hubemu::is_running(fr.exe.empty() ? std::string("lichii.exe") : fr.exe) || A.proc != nullptr;
    if (hubemu::release_is_placeholder(Kind::Fritchii)) {
        text_wrapped_disabled("Not available to download yet.");
    } else if (fr.exe.empty()) {
        text_wrapped_disabled(std::string("Not installed (") + rel.version + ", about " + std::to_string((int)(rel.size / 1048576)) + " MB). It downloads the first time you play with it, or now:");
        if (button("Install Fritchii", ImVec2(0, 0), false, !busy, ui::Icon::Refresh)) {
            EMU.kind = Kind::Fritchii;
            EMU.game_key.clear();
            EMU.updating = false;
            EMU.state = hubemu::Installer::Asking;
        }
    } else {
        text_wrapped_disabled("Fritchii " + std::string(rel.version) + ": " + fr.exe);
        const bool repair = !hubemu::update_leftovers(Kind::Fritchii, fr.dir).empty();
        if (fr.outdated || repair) {
            text_wrapped_disabled(repair ? std::string("Fritchii was left half-updated by its own updater. Reinstalling the tested version fixes it; your keys, firmware, saves and settings are kept.")
                                         : std::string("A newer Fritchii is available (") + rel.version + ", about " + std::to_string((int)(rel.size / 1048576)) + " MB). Your keys, firmware, saves and settings are kept.");
            if (button(repair ? "Reinstall Fritchii" : "Update Fritchii", ImVec2(0, 0), true, !busy, ui::Icon::Refresh)) {
                EMU.kind = Kind::Fritchii;
                EMU.game_key.clear();
                EMU.updating = true;
                EMU_UPDATE_SKIP[(int)Kind::Fritchii] = false;
                EMU.state = hubemu::Installer::Asking;
            }
        }
        text_wrapped_disabled(status_line(LOOK_F.st) + "   (copied from Lichii the first time you play with Fritchii)");
        ASK.kind = Kind::Fritchii;   // (the buttons report into the prompt's message line)
        import_buttons(LOOK_F, "setf");
        ASK.kind = Kind::Lichii;
    }
    // which program Switch and Switch 2 games play with: the Nintendo Switch default (Switch 2 inherits it unless set for Switch 2 itself); a game can still override it on its own Settings tab (Ocarina of Time)
    ImGui::Dummy(ImVec2(0, 4 * S));
    const hub::gs::Values rv = GSTORE.resolve(hub::System::Switch, "");
    auto it = rv.find(hubemu::kRunnerKey);
    const std::string cur = it == rv.end() ? "lichii" : it->second;
    const hub::gs::Option* o = hub::gs::find_option(hubemu::kRunnerKey);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Switch and Switch 2 games play with");
    ImGui::SameLine();
    std::string shown = cur;
    if (o) for (const auto& c : o->choices) if (c.value == cur) shown = c.label;
    ImGui::SetNextItemWidth(260 * S);
    if (o && ImGui::BeginCombo("##nxrunner", shown.c_str())) {
        for (const auto& c : o->choices) {
            if (ImGui::Selectable(c.label.c_str(), c.value == cur) && c.value != cur) gs_set_system_value(hub::System::Switch, hubemu::kRunnerKey, c.value);
            if (c.value == cur) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    text_wrapped_disabled("Ocarina of Time plays with Fritchii unless you pick a program for it. Hookshot only plays Ocarina of Time (once built on its game page). Fritchii needs to be installed; until then Play offers to download it.");
}

/* Settings > Playing (emulators list). */
void switch_settings_ui() {
    refresh(LOOK_L);
    Look& LOOK = LOOK_L;
    const hubemu::Install& li = LOOK.inst;
    if (li.exe.empty()) {
        text_wrapped_disabled("Lichii (Nintendo Switch & Switch 2, " + std::string(hubemu::release(hubemu::Kind::Lichii).version) + "): not installed - downloads the first time you play a Switch game (about " +
                              std::to_string((int)(hubemu::release(hubemu::Kind::Lichii).size / 1048576)) + " MB). Very experimental.");
        fritchii_settings_ui();
        return;
    }
    text_wrapped_disabled("Lichii (Nintendo Switch & Switch 2): " + li.exe);
    if (li.outdated) {
        const hubemu::Release& rel = hubemu::release(hubemu::Kind::Lichii);
        text_wrapped_disabled(std::string("A newer Lichii is available (") + rel.version + ", about " + std::to_string((int)(rel.size / 1048576)) +
                              " MB). Your keys, firmware, saves and settings are kept.");
        const bool busy = EMU.state != hubemu::Installer::Idle || hubemu::is_running(li.exe) || A.proc != nullptr;
        if (button("Update Lichii", ImVec2(0, 0), true, !busy, ui::Icon::Refresh)) {
            EMU.kind = hubemu::Kind::Lichii;
            EMU.game_key.clear();
            EMU.updating = true;
            EMU_UPDATE_SKIP[(int)hubemu::Kind::Lichii] = false;
            EMU.state = hubemu::Installer::Asking;
        }
        if (hubemu::is_running(li.exe)) text_wrapped_disabled("Close Lichii first: it is not updated while it runs.");
    }
    text_wrapped_disabled(status_line(LOOK.st));
    import_buttons(LOOK, "set");
    fritchii_settings_ui();
}
