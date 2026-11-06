#include "app.h"

#include "lichii.h"

/* ---------- Nintendo Switch / Switch 2 (Lichii): the keys + firmware prompt before a game starts, and the Settings block.
 * RetroHub never ships or downloads keys or firmware: the player copies their own (dumped from a console they own) from a file / folder they
 * pick, or from an Eden / yuzu / Ryujinx / Citron / Suyu / Sudachi install already on this PC (only read). docs/EMULATORS.md (Switch). ---------- */

namespace {

struct SwitchAsk {
    bool open = false;
    std::string key;          // game to start once set up ("" = opened from Settings / a screenshot)
    std::string message;      // last import result
    bool message_bad = false;
    bool focused = false;
} ASK;

bool g_skip_firmware = false;   // "Play without firmware" for this RetroHub session

struct Look {
    double at = -100;
    hubemu::Install inst;
    hubemu::NxSetup st;
    std::vector<hubemu::NxSource> sources;
} LOOK;

void refresh(bool force = false) {
    if (!force && ImGui::GetTime() - LOOK.at < 2.0) return;   // touches the disk
    LOOK.at = ImGui::GetTime();
    LOOK.inst = hubemu::find(hubemu::Kind::Lichii, "", "");
    LOOK.st = LOOK.inst.dir.empty() ? hubemu::NxSetup{} : hubemu::lichii_setup_status(LOOK.inst.dir);
    const char* ad = std::getenv("APPDATA");
    LOOK.sources = hubemu::lichii_find_sources(ad ? ad : "", A.st.rom_folders);
}

std::string status_line(const hubemu::NxSetup& st) {
    std::string s = std::string("Keys: ") + (st.prod_keys ? "prod.keys found" : "missing");
    if (st.prod_keys) s += st.title_keys ? " (+ title.keys)" : "";
    s += std::string("   Firmware: ") + (st.firmware ? (st.firmware_version.empty() ? std::string("installed") : st.firmware_version) : std::string("missing"));
    return s;
}

void report(const std::string& err, const std::string& ok_text) {
    ASK.message = err.empty() ? ok_text : err;
    ASK.message_bad = !err.empty();
    refresh(true);
}

/* The import buttons, shared by the prompt and Settings. `id` keeps the picker ids apart. */
void import_buttons(const char* id) {
    if (LOOK.inst.dir.empty()) { text_wrapped_disabled("Lichii is not downloaded yet: start any Switch game once, then add your keys and firmware here."); return; }
    const std::string dir = LOOK.inst.dir;
    for (size_t i = 0; i < LOOK.sources.size(); ++i) {
        const hubemu::NxSource& s = LOOK.sources[i];
        std::string what = !s.keys_dir.empty() && !s.firmware_dir.empty() ? "keys + firmware" : !s.keys_dir.empty() ? "keys" : "firmware";
        std::string label = "Copy " + what + " from " + s.label + "##nxsrc" + std::string(id) + std::to_string(i);
        if (button(label.c_str(), ImVec2(0, 0), i == 0 && !LOOK.st.prod_keys, true, ui::Icon::Folder)) {
            std::string err;
            if (!s.keys_dir.empty()) err = hubemu::lichii_import_keys(dir, s.keys_dir);
            if (err.empty() && !s.firmware_dir.empty()) err = hubemu::lichii_import_firmware(dir, s.firmware_dir);
            report(err, "Copied " + what + " from " + s.label + ".");
        }
    }
    {
        std::string p = osk::file(id[0] == 'a' ? "nxkeys_ask" : "nxkeys_set", button((std::string("Choose prod.keys...##") + id).c_str(), ImVec2(0, 0), LOOK.sources.empty() && !LOOK.st.prod_keys, true, ui::Icon::Folder), {".keys"});
        if (!p.empty()) report(hubemu::lichii_import_keys(dir, p), "Keys copied.");
    }
    ImGui::SameLine();
    {
        std::string p = osk::folder(id[0] == 'a' ? "nxfwd_ask" : "nxfwd_set", button((std::string("Choose firmware folder...##") + id).c_str(), ImVec2(0, 0), false, true, ui::Icon::Folder));
        if (!p.empty()) report(hubemu::lichii_import_firmware(dir, p), "Firmware installed.");
    }
    ImGui::SameLine();
    {
        std::string p = osk::file(id[0] == 'a' ? "nxfwz_ask" : "nxfwz_set", button((std::string("Firmware .zip...##") + id).c_str(), ImVec2(0, 0), false, true, ui::Icon::Folder), {".zip"});
        if (!p.empty()) report(hubemu::lichii_import_firmware(dir, p), "Firmware installed.");
    }
    if (!ASK.message.empty()) ImGui::TextColored(ImColor(ASK.message_bad ? ui::WARN : ui::GOOD), "%s", ASK.message.c_str());
}

void start_game() {
    ASK.open = false;
    if (const hub::GameEntry* g = find_entry(ASK.key)) { hub::GameEntry copy = *g; play_entry(copy); }
}

}  // namespace

/* launch.cpp: true when the game may start now; otherwise the prompt is opened for it. */
bool switch_ready_or_ask(const hub::GameEntry& e, const std::string& install_dir) {
    hubemu::NxSetup st = hubemu::lichii_setup_status(install_dir);
    const bool keys_ok = st.prod_keys || !hubemu::lichii_needs_keys(e.path);
    if (keys_ok && (st.firmware || g_skip_firmware)) return true;
    ASK.open = true;
    ASK.key = e.key;
    ASK.message.clear();
    refresh(true);
    return false;
}

void switch_setup_ask_shot() { ASK.open = true; ASK.key.clear(); ASK.message.clear(); refresh(true); }

void draw_switch_dialogs() {
    if (!ASK.open) return;
    bool open = false;
    emu_modal_begin("Switch keys and firmware", &open);
    if (!open) return;
    refresh();
    const hub::GameEntry* g = ASK.key.empty() ? nullptr : find_entry(ASK.key);
    const bool needs_keys = g ? hubemu::lichii_needs_keys(g->path) : true;
    const bool keys_ok = LOOK.st.prod_keys || !needs_keys;
    if (ui::modal_back()) ASK.open = false;
    ImGui::TextWrapped("%s", needs_keys ? "Switch games need your own keys (prod.keys) and firmware, dumped from a Switch you own."
                                        : "This game needs no keys, but most games also want the Switch firmware, dumped from a Switch you own.");
    ImGui::Dummy(ImVec2(0, 2 * S));
    text_wrapped_disabled("RetroHub does not include or download them. Copy them once from your own dump (a prod.keys file and a firmware folder or .zip) "
                          "or from an emulator already on this PC; they are kept in RetroHub's own Lichii folder.");
    ImGui::Dummy(ImVec2(0, 4 * S));
    ImGui::TextUnformatted(status_line(LOOK.st).c_str());
    ImGui::Dummy(ImVec2(0, 4 * S));
    if (!ASK.focused) { ImGui::SetKeyboardFocusHere(0); ASK.focused = true; }   // a pad lands on the first import button
    import_buttons("ask");
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

/* Settings > Playing (emulators list). */
void switch_settings_ui() {
    refresh();
    const hubemu::Install& li = LOOK.inst;
    if (li.exe.empty()) {
        text_wrapped_disabled("Lichii (Switch / Switch 2, " + std::string(hubemu::release(hubemu::Kind::Lichii).version) + "): not installed - downloads the first time you play a Switch game (about " +
                              std::to_string((int)(hubemu::release(hubemu::Kind::Lichii).size / 1048576)) + " MB). Very experimental.");
        return;
    }
    text_wrapped_disabled("Lichii (Switch / Switch 2): " + li.exe);
    text_wrapped_disabled(status_line(LOOK.st));
    import_buttons("set");
}
