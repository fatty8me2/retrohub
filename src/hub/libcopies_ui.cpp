#include "app.h"

#include "resume_more.h"
#include "prefs.h"
#include "hidden_games.h"
#include "hidden_ui.h"
#include "library_scan_info.h"


/* Library file problems: a ROM that vanished (popup at Play) and duplicate copies of one game (details > About).
 * The choices live in the synced prefs (preferred_copy, hidden); nothing here touches library.json. */

namespace {

struct MissingAsk {
    bool open = false;
    bool seen = false;
    std::string key, title, path;
    std::string warning;
} MA;

std::string vault() { return hub::saves::vault_dir(); }

hub::prefs::Prefs load_prefs() {
    hub::prefs::Prefs p;
    hub::prefs::load(vault(), &p);
    return p;
}

void store_prefs(const hub::prefs::Prefs& p) {
    hub::prefs::save(vault(), p);
    A.vault_dirty = true;
}

std::string ext_of(const std::string& p) {
    size_t d = p.find_last_of('.'), s = p.find_last_of("/\\");
    if (d == std::string::npos || (s != std::string::npos && d < s)) return "";
    std::string e = p.substr(d);
    for (char& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}

bool path_present(const std::string& p) {
    std::error_code ec;
    return !p.empty() && std::filesystem::exists(std::filesystem::u8path(p), ec);
}

struct UndoRemove {
    std::string key, title;
    double until = 0;
} UNDO;

std::map<std::string, std::string> g_hidden_titles;   // key -> title, remembered this session for the Hidden games list

void drop_from_lib(const std::string& key) {
    A.lib.erase(std::remove_if(A.lib.begin(), A.lib.end(), [&](const hub::GameEntry& g) { return g.key == key; }), A.lib.end());
    A.lib_index.clear();
    for (size_t i = 0; i < A.lib.size(); ++i) A.lib_index.emplace(A.lib[i].key, (int)i);
    A.vis_dirty = true;
}

void remove_here(const std::string& key, const std::string& title) {
    hub::local_removed_set(key, true);
    g_hidden_titles[key] = title;
    drop_from_lib(key);
    UNDO.key = key;
    UNDO.title = title;
    UNDO.until = ImGui::GetTime() + 10.0;
    push_toast("Removed " + title + " from this PC's library. Your other PCs still show it.", ui::WARN, 10.0);
}

std::vector<hub::GameEntry> g_stash;   // the hidden games the last scan took out of the library (for "Show hidden")

void hide_game(const std::string& key, const std::string& title) {
    if (const hub::GameEntry* e = find_entry(key)) {
        g_stash.erase(std::remove_if(g_stash.begin(), g_stash.end(), [&](const hub::GameEntry& g) { return g.key == key; }), g_stash.end());
        g_stash.push_back(*e);   // copy before drop_from_lib() invalidates it
    }
    hub::prefs::Prefs p = load_prefs();
    if (hub::hidden::hide(&p, key)) store_prefs(p);
    g_hidden_titles[key] = title;
    drop_from_lib(key);
    UNDO.key = key;
    UNDO.title = title;
    UNDO.until = ImGui::GetTime() + 10.0;
    push_toast("Hid " + title + " on all your PCs.", ui::WARN, 10.0);
}

void unhide_game(const std::string& key) {
    hub::prefs::Prefs p = load_prefs();
    if (hub::hidden::unhide(&p, key)) store_prefs(p);
    hub::local_removed_set(key, false);
    g_stash.erase(std::remove_if(g_stash.begin(), g_stash.end(), [&](const hub::GameEntry& g) { return g.key == key; }), g_stash.end());
    start_scan();   // the game comes back with the rescan
}

/* "Show hidden" is a per-PC choice (the hidden list itself syncs): a flag file in this PC's data folder. */
std::string show_flag_path() { return hub::data_dir() + "show_hidden.flag"; }
int g_show_hidden = -1;   // -1 = not read yet

struct HiddenList {
    bool open = false, seen = false;
    std::vector<std::string> keys;
    double loaded = -100;
} HL;

void refresh_hidden() {
    if (HL.loaded > ImGui::GetTime() - 2.0 && HL.loaded <= ImGui::GetTime()) return;
    HL.loaded = ImGui::GetTime();
    if (g_headless && std::getenv("HUB_SHOT_HIDDEN")) { HL.keys = {"n64:0123456789abcdef0123456789abcdef", "gba:agbj-1a2b3c4d", "gc:gm4e01-0"}; return; }
    HL.keys = load_prefs().hidden;
    for (const std::string& k : hub::local_removed()) if (std::find(HL.keys.begin(), HL.keys.end(), k) == HL.keys.end()) HL.keys.push_back(k);
}

std::string hidden_label(const std::string& key) {
    auto it = g_hidden_titles.find(key);
    if (it != g_hidden_titles.end()) return it->second;
    if (g_headless && std::getenv("HUB_SHOT_HIDDEN")) return key == "gba:agbj-1a2b3c4d" ? "Test Game 5" : key == "gc:gm4e01-0" ? "Test Game 3" : "Test Game 1";
    return key;
}

}  // namespace

bool rom_missing(const hub::GameEntry& e) {
    if (g_headless && !std::getenv("HUB_REAL_ROMCHECK")) return false;   // headless runs use made-up paths and trace the spawn instead; HUB_REAL_ROMCHECK=1 (QA) runs the real check
    if (is_native(e) || is_kanto(e) || !e.issue.empty()) return false;   // exe / package / archived images have their own checks
    return !path_present(e.path);
}

void missing_rom_ask(const std::string& key) {
    const hub::GameEntry* e = find_entry(key);
    MA = MissingAsk();
    MA.open = true;
    MA.key = key;
    if (e) { MA.title = e->title; MA.path = e->path; }
}

bool missing_rom_open() { return MA.open; }

void missing_rom_ask_shot() {
    MA = MissingAsk();   // screenshot aid: the library is not scanned yet when shot.cpp runs, so use made-up values
    MA.open = true;
    MA.key = "fake:shot";
    MA.title = "Test Game 3";
    MA.path = "D:/Roms/N64/Test Game 3.z64";
}

std::map<std::string, std::string> preferred_copies() { return load_prefs().preferred_copy; }

std::set<std::string> hidden_game_keys() {
    hub::prefs::Prefs p = load_prefs();
    std::set<std::string> s(p.hidden.begin(), p.hidden.end());
    for (const std::string& k : hub::local_removed()) s.insert(k);   // removed on this PC only
    return s;
}

/* Scan time: a located file replaces a path that no longer exists. */
void apply_located_paths(std::vector<hub::GameEntry>* lib, const std::map<std::string, std::string>& preferred) {
    for (auto& e : *lib) {
        auto it = preferred.find(e.key);
        if (it == preferred.end() || it->second == e.path || !e.issue.empty()) continue;
        if (!path_present(e.path) && path_present(it->second)) e.path = it->second;
    }
}

static void set_pref_copy(const std::string& key, const std::string& path) {
    hub::prefs::Prefs p = load_prefs();
    std::map<std::string, std::string> m = p.preferred_copy;
    m[key] = path;
    if (hub::prefs::set_preferred_copy(&p, m)) store_prefs(p);
}

/* Settings > General: "Hidden games (N)" opens the list with Unhide per game. */
void hidden_games_row() {
    refresh_hidden();
    if (HL.keys.empty()) return;
    char lab[64];
    std::snprintf(lab, sizeof lab, "Hidden games (%d)", (int)HL.keys.size());
    if (button(lab, ImVec2(0, 0), false, true, ui::Icon::Close)) { HL.open = true; HL.seen = false; }
}

void hidden_games_open_shot() { HL.open = true; HL.seen = false; }

static void draw_hidden_list() {
    if (!HL.open) return;
    refresh_hidden();
    bool open = false;
    emu_modal_begin("Hidden games", &open);
    if (!open) return;
    if (ui::modal_back()) HL.open = false;
    ImGui::TextWrapped("Games you removed from the library. Unhide brings one back after a rescan.");
    ImGui::Dummy(ImVec2(0, ui::sp(1.f)));
    if (HL.keys.empty()) text_wrapped_disabled("Nothing is hidden.");
    ImGui::BeginChild("##hiddenlist", ImVec2(0, std::min(ui::sp(28.f), (float)HL.keys.size() * ui::sp(7.f) + ui::sp(1.f))), ImGuiChildFlags_NavFlattened);
    std::string unhide;
    for (size_t i = 0; i < HL.keys.size(); ++i) {
        ImGui::PushID((int)i);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(hidden_label(HL.keys[i]).c_str());
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui::sp(10.f));
        if (!HL.seen) { ImGui::SetKeyboardFocusHere(0); HL.seen = true; }   // a pad lands on the first Unhide
        if (button("Unhide", ImVec2(ui::sp(10.f), ui::sp(4.f)), false, true)) unhide = HL.keys[i];
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (!unhide.empty()) {
        if (!(g_headless && std::getenv("HUB_SHOT_HIDDEN"))) unhide_game(unhide);
        HL.keys.erase(std::remove(HL.keys.begin(), HL.keys.end(), unhide), HL.keys.end());
        HL.loaded = ImGui::GetTime();
        set_notice("Unhid " + hidden_label(unhide) + ". Rescanning...", 4.0);
        if (HL.keys.empty()) HL.open = false;
    }
    ImGui::Dummy(ImVec2(0, ui::sp(1.f)));
    if (button("Close", ImVec2(ui::sp(12.f), ui::sp(4.f)))) HL.open = false;
    if (!HL.open) { ImGui::CloseCurrentPopup(); HL.seen = false; }
    ImGui::EndPopup();
}

/* Undo after "Remove from library": a small clickable card above the toasts (the toast list itself is draw-only). */
static void draw_undo_card() {
    double now = ImGui::GetTime();
    if (UNDO.key.empty() || now > UNDO.until) { UNDO.key.clear(); return; }
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x - ui::sp(3.f), ds.y - ui::sp(12.f)), ImGuiCond_Always, ImVec2(1.f, 1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::sp(1.f), ui::sp(1.f)));
    ImGui::Begin("##undoremove", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    if (button("Undo", ImVec2(ui::sp(10.f), ui::sp(4.f)), true)) {
        std::string k = UNDO.key;
        UNDO.key.clear();
        unhide_game(k);
        set_notice("Restored " + UNDO.title + ". Rescanning...", 4.0);
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void draw_missing_rom_dialog() {
    resume_choice_draw();   // Wave 3 R1: "which save" choice before a launch
    draw_hidden_list();
    draw_undo_card();
    if (!MA.open) return;
    bool open = false;
    emu_modal_begin("Can't find game", &open);
    if (!open) return;
    if (ui::modal_back()) MA.open = false;
    ImGui::TextWrapped("Can't find %s", MA.title.empty() ? "this game" : MA.title.c_str());
    ImGui::Dummy(ImVec2(0, ui::sp(0.5f)));
    text_wrapped_disabled("The file is gone or its drive is not connected:\n" + MA.path);
    if (!MA.warning.empty()) { ImGui::Dummy(ImVec2(0, ui::sp(0.5f))); ImGui::TextColored(ImColor(ui::WARN), "%s", MA.warning.c_str()); }
    ImGui::Dummy(ImVec2(0, ui::sp(1.5f)));
    ImVec2 bs(ui::sp(20.f), ui::sp(4.f));
    if (!MA.seen) { ImGui::SetKeyboardFocusHere(0); MA.seen = true; }   // a pad lands on "Locate file"
    const bool locate = button("Locate file...", bs, true, true, ui::Icon::Folder);
    std::string ext = ext_of(MA.path);
    std::string picked = osk::file("missing_rom", locate, ext.empty() ? std::vector<std::string>{} : std::vector<std::string>{ext});
    if (!picked.empty()) {
        if (!path_present(picked)) MA.warning = "That file could not be read.";
        else {
            set_pref_copy(MA.key, picked);
            for (auto& g : A.lib) if (g.key == MA.key) g.path = picked;
            A.vis_dirty = true;
            std::string key = MA.key;
            MA.open = false;
            if (const hub::GameEntry* g = find_entry(key)) { hub::GameEntry copy = *g; play_entry(copy); }
        }
    }
    ImGui::SameLine();
    if (button("Rescan", bs, false, !A.scanning, ui::Icon::Refresh)) { start_scan(true); MA.open = false; set_notice("Scanning your games...", 4.0); }
    if (button("Remove from this PC", bs, false, true)) {   // local only: your other PCs keep the game
        remove_here(MA.key, MA.title);
        MA.open = false;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hides it here only. Hidden games (Settings) can bring it back.");
    ImGui::SameLine();
    if (button("Hide on all my PCs", bs, false, true)) {
        hide_game(MA.key, MA.title);
        MA.open = false;
    }
    if (button("Cancel", bs)) MA.open = false;
    if (!MA.open) { ImGui::CloseCurrentPopup(); MA.seen = false; }
    ImGui::EndPopup();
}

/* Settings > General folder list (library-3): "" when the folder was read in full by the last scan, else a short plain status such as
 * "Drive not connected - showing 42 games from the last scan". page_settings.cpp prints it under the folder's path. */
std::string library_folder_status_line(const std::string& folder) {
    auto norm = [](std::string s) { for (char& c : s) { if (c == '/') c = '\\'; c = (char)std::tolower((unsigned char)c); } while (!s.empty() && s.back() == '\\') s.pop_back(); return s; };
    const std::string want = norm(folder);
    for (const hub::FolderScanStatus& st : hub::scan_folder_status()) {
        if (norm(st.folder) != want || st.complete) continue;
        std::string kept = st.kept ? " Showing " + std::to_string(st.kept) + (st.kept == 1 ? " game" : " games") + " from the last scan." : std::string();
        return st.reachable ? "Could not read all of this folder." + kept : "Drive not connected." + kept;
    }
    return std::string();
}

/* Details > About: "N copies found" with their paths and a "Use this one" choice. */
void dupes_details_section(const hub::GameEntry& e) {
    auto it = A.dupes.find(e.key);
    if (it == A.dupes.end() || it->second.size() < 2) return;
    ImGui::Dummy(ImVec2(0, ui::sp(0.5f)));
    ImGui::Text("%d copies found", (int)it->second.size());
    for (size_t i = 0; i < it->second.size(); ++i) {
        const std::string& p = it->second[i];
        const bool cur = p == e.path;
        ImGui::PushID((int)i);
        ImGui::PushStyleColor(ImGuiCol_Text, cur ? ui::TEXT : ui::TEXT3);
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextWrapped("%s%s", cur ? "In use: " : "", p.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        if (!cur && button("Use this one", ImVec2(0, ui::sp(3.5f)), false, true)) {
            set_pref_copy(e.key, p);
            for (auto& g : A.lib) if (g.key == e.key) g.path = p;
            A.vis_dirty = true;
            start_scan();   // re-run the scan so everything (art, saves, ids) follows the chosen file
        }
        ImGui::PopID();
    }
}

/* ---------- Hide game / Show hidden (library-12) ---------- */

void hide_game_now(const std::string& key) {
    const hub::GameEntry* e = find_entry(key);
    if (!e || is_native(*e)) return;   // the bundled Smash entry is never hidden
    hide_game(key, e->title);
}

void unhide_game_now(const std::string& key) {
    std::string t;
    for (const hub::GameEntry& g : g_stash) if (g.key == key) t = g.title;
    unhide_game(key);
    set_notice("Unhid " + (t.empty() ? hidden_label(key) : t) + ". Rescanning...", 4.0);
    HL.loaded = -100;
}

void hidden_stash_set(std::vector<hub::GameEntry> hidden) {
    g_stash = std::move(hidden);
    std::sort(g_stash.begin(), g_stash.end(), [](const hub::GameEntry& a, const hub::GameEntry& b) { return a.title < b.title; });
}

size_t hidden_stash_count() { return g_stash.size(); }

bool show_hidden_on() {
    if (g_show_hidden < 0) {
        std::error_code ec;
        g_show_hidden = std::filesystem::exists(std::filesystem::u8path(show_flag_path()), ec) ? 1 : 0;
    }
    return g_show_hidden == 1;
}

void show_hidden_toggle() {
    const bool on = !show_hidden_on();
    g_show_hidden = on ? 1 : 0;
    std::error_code ec;
    const std::filesystem::path f = std::filesystem::u8path(show_flag_path());
    if (on) { std::ofstream o(f, std::ios::binary); o << "1"; }
    else std::filesystem::remove(f, ec);
}

/* The dimmed list under the filter chips: each hidden game with an Unhide button (a pad lands on the first one). */
void hidden_strip_draw() {
    if (!show_hidden_on()) return;
    ImGui::Dummy(ImVec2(0, ui::sp(0.5f)));
    if (g_stash.empty()) { text_wrapped_disabled("Nothing is hidden."); return; }
    text_wrapped_disabled(A.pad_mode ? "Hidden games, dimmed. Press A on Unhide to bring one back." : "Hidden games, dimmed. Unhide brings one back.");
    const float h = std::min(ui::sp(26.f), (float)g_stash.size() * ui::sp(5.f) + ui::sp(1.f));
    ImGui::BeginChild("##hiddenstrip", ImVec2(0, h), ImGuiChildFlags_NavFlattened);
    std::string unhide;
    for (size_t i = 0; i < g_stash.size(); ++i) {
        const hub::GameEntry& g = g_stash[i];
        ImGui::PushID((int)i);
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);   // dimmed
        ImGui::Text("%s  -  %s", g.title.c_str(), hub::console_name(g));
        ImGui::PopStyleColor();
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ui::sp(10.f) + ImGui::GetCursorPosX());
        if (button("Unhide", ImVec2(ui::sp(10.f), ui::sp(4.f)), false, true)) unhide = g.key;
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (!unhide.empty()) unhide_game_now(unhide);
}
