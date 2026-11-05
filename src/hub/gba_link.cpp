/* Plan 4 Wave 3 X2: GameCube <-> GBA (offline) through Dolphin's integrated GBA. Logic: gba_link_logic.h (read its header for the Dolphin facts).
 *  - gba_link_details_section: the "Game Boy Advance link" row of a GameCube game's details (only for games that use a GBA).
 *  - gba_link_apply_dolphin: offline launch only; writes [GBA] + SIDevice for this launch and stages the vault saves. prepare_dolphin rewrites
 *    SIDevice0..3 from the real pads on every launch (and netplay / speed tests call it too), so the next launch is back to normal pads.
 *  - gba_link_after_exit: called by launch_external's after_exit; a changed GBA save goes back to the vault and is queued for the cloud push.
 * Screenshot aid: HUB_SHOT_GBALINK=bios|nobios (with HUB_SHOT_FAKE_LIB=8 HUB_SHOT_DETAILS=fake:2): fake Metroid Prime + Fusion/Ruby, BIOS present or missing. */
#include "app.h"
#include "emulators.h"
#include "gba_link_logic.h"
#include "saves/savestore.h"
#include "wave3_hooks.h"

#include <cctype>
#include <cstdlib>

namespace {

std::map<std::string, hubgba::Ports> g_store;
bool g_loaded = false;
std::vector<hubgba::Staged> g_staged;   // the running (or just started) launch

std::string root(const std::string& d) {
    std::string r = d;
    while (!r.empty() && (r.back() == '\\' || r.back() == '/')) r.pop_back();
    return r;
}
std::string store_path() { return root(hub::data_dir()) + "\\gba_link.json"; }

const char* shot_mode() { static const char* m = g_headless ? std::getenv("HUB_SHOT_GBALINK") : nullptr; return m; }

void load_store() {
    if (g_loaded) return;
    g_loaded = true;
    std::string t;
    if (hubgba::read_all(store_path(), &t)) hubgba::store_from_json(t, &g_store);
    if (shot_mode() && std::string(shot_mode()) == "bios") { hubgba::Ports p; p[1] = hubgba::game_choice("fake:1"); p[2] = hubgba::kControllerChoice; g_store["fake:2"] = p; }
}
void save_store() { hubgba::write_atomic(store_path(), hubgba::store_to_json(g_store)); }

/* The player's own gba_bios.bin, found the way "Real console startup" finds it (Settings BIOS folders, game folders' GamesBios / BIOS, ...). */
std::string find_bios() {
    if (shot_mode()) return std::string(shot_mode()) == "bios" ? "C:/fake/gba_bios.bin" : "";
    hub::GameEntry g;
    g.system = hub::System::GBA;
    RealBootPlan p = real_boot_plan(g);
    if (p.stage.ok && !p.stage.copies.empty()) return p.stage.copies[0].first;
    return "";
}

std::vector<hubgba::Cand> gba_games() {
    std::vector<hubgba::Cand> v;
    for (const hub::GameEntry& g : A.lib)
        if (g.system == hub::System::GBA && g.hack_patch.empty() && g.issue.empty()) v.push_back({g.key, g.title, g.serial, g.path});
    return v;
}
const hub::GameEntry* gba_entry(const std::string& key) {
    for (const hub::GameEntry& g : A.lib) if (g.system == hub::System::GBA && g.key == key) return &g;
    return nullptr;
}

std::string backup_dir_for(const std::string& key) {
    std::string k = key;
    for (char& c : k) if (!(std::isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_')) c = '_';   // keys look like "gba:axve-1234": no ':' in a folder name
    return root(hub::data_dir()) + "\\gba_link_backups\\" + k;
}

void finish_pending() {
    std::vector<hubgba::Staged> st;
    st.swap(g_staged);
    for (const hubgba::Staged& s : st) {
        std::string err;
        hubgba::Back b = hubgba::finish_save(s, &err);
        if (b == hubgba::Back::Copied) {
            hub::saves::touch_game(s.system_short, s.game_key, s.title);   // like a normal play's exit: meta, then the cloud push below
            hub::saves::history_snapshot_game(s.system_short, s.game_key);
            hub::saves::GameId gid{s.system_short, s.game_key};
            if (!cloud_bg_run(gid.gkey(), [gid]() { hub::saves::cloud_game_exited(gid); })) hub::saves::cloud_game_exited(gid);   // queues the push
            A.vault_dirty = true;
            push_toast("Saved your " + s.title + " progress from the GBA.", ui::ACCENT, 6.0);
        } else if (b == hubgba::Back::Failed) {
            push_toast("Could not copy the " + s.title + " GBA save into the vault: " + err + " (Dolphin's copy is kept).", ui::WARN, 15.0);
        }
    }
}

}  // namespace

void gba_link_after_exit() { finish_pending(); }

void gba_link_apply_dolphin(const hub::GameEntry& e, const std::string& dolphin_dir) {
    finish_pending();   // a launch that never ran its exit hook (spawn failed, hub closed): copy back first, never lose it
    const hubgba::Target* t = hubgba::target_for(e.serial);
    if (!t || e.system != hub::System::GC) return;
    load_store();
    auto it = g_store.find(e.key);
    if (it == g_store.end() || !hubgba::any_on(it->second)) return;   // default: nothing changes
    const std::string bios = find_bios();
    if (bios.empty()) { set_notice("Game Boy Advance link skipped: gba_bios.bin was not found. Put your own copy in your GamesBios folder.", 12.0); return; }
    hubgba::IniPlan plan;
    plan.bios = bios;
    plan.saves_dir = dolphin_dir + "\\User\\GBA\\Saves";
    std::vector<hubgba::Staged> staged;
    for (int i = 1; i <= t->ports && i < 4; ++i) {
        hubgba::Choice c = hubgba::parse_choice(it->second[i]);
        if (c.kind == hubgba::Kind::Off) continue;
        if (c.kind == hubgba::Kind::Controller) { plan.on[i] = true; continue; }
        const hub::GameEntry* g = gba_entry(c.key);
        if (!g || !file_exists(g->path)) { set_notice("Game Boy Advance link: the GBA game for port " + std::to_string(i + 1) + " is no longer in your library.", 10.0); continue; }
        bool dup = false;
        for (const hubgba::Staged& o : staged) dup = dup || o.game_key == g->key;
        if (dup) { set_notice("Game Boy Advance link: use a different GBA game on each port (" + g->title + " is on two).", 10.0); continue; }
        hubgba::Staged s;
        s.port = i;
        s.game_key = g->key;
        s.system_short = sys_short(*g);
        s.title = g->title;
        s.vault = hubgba::vault_file(hub::saves::battery_dir(s.system_short, g->key), g->path, g->save_stem);
        s.dolphin = hubgba::dolphin_save_file(plan.saves_dir, g->path, i);
        s.backup_dir = backup_dir_for(g->key);
        std::string err;
        if (!hubgba::stage_save(s, &err)) { set_notice("Game Boy Advance link: " + err + ".", 12.0); continue; }
        plan.on[i] = true;
        plan.rom[i] = g->path;
        staged.push_back(s);
    }
    bool any = false;
    for (int i = 1; i < 4; ++i) any = any || plan.on[i];
    if (!any) return;
    const std::string ini = dolphin_dir + "\\User\\Config\\Dolphin.ini";
    std::string text;
    hubgba::read_all(ini, &text);
    if (!hubgba::write_atomic(ini, hubgba::ini_apply(text, plan))) { set_notice("Game Boy Advance link: could not write Dolphin's settings.", 12.0); return; }
    g_staged = staged;
}

void gba_link_details_section(const hub::GameEntry& e) {
    if (e.system != hub::System::GC) return;
    if (shot_mode() && e.key == "fake:2") { const_cast<hub::GameEntry&>(e).serial = "GM8E01"; }   // screenshot aid: the fake GC game is Metroid Prime
    const hubgba::Target* t = hubgba::target_for(e.serial);
    if (!t) return;
    load_store();
    if (shot_mode()) for (hub::GameEntry& g : A.lib) {
        if (g.key == "fake:1") { g.serial = "AMTE01"; g.title = "Metroid Fusion"; }
        if (g.key == "fake:6") { g.serial = "AXVE01"; g.title = "Pokemon Ruby"; }
    }
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    ImGui::TextUnformatted("Game Boy Advance link");
    ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextWrapped("%s Offline play only.", t->help);
    const std::string bios = find_bios();
    if (bios.empty()) ImGui::TextWrapped("Needs your own GBA BIOS (gba_bios.bin) in your GamesBios folder. RetroHub never downloads it, so this stays off until it is there.");
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    const std::vector<hubgba::Cand> games = gba_games();
    size_t n_match = 0;
    const std::vector<size_t> order = hubgba::ordered(t, games, &n_match);
    if (t->need == hubgba::Need::Rom && games.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
        ImGui::TextWrapped("No Game Boy Advance games in your library yet.");
        ImGui::PopStyleColor();
    }
    hubgba::Ports& ports = g_store[e.key];
    bool changed = false;
    if (!bios.empty()) {
        for (int i = 1; i <= t->ports && i < 4; ++i) {
            ImGui::PushID(i);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Port %d", i + 1);
            ImGui::SameLine(sp(9.f));
            ImGui::SetNextItemWidth(sp(30.f));
            hubgba::Choice cur = hubgba::parse_choice(ports[i]);
            std::string preview = "Off";
            if (cur.kind == hubgba::Kind::Controller) preview = "GBA as controller";
            else if (cur.kind == hubgba::Kind::Game) { preview = "(not in your library)"; for (const auto& g : games) if (g.key == cur.key) preview = g.title; }
            if (ImGui::BeginCombo("##gba", preview.c_str())) {
                if (ImGui::Selectable("Off", cur.kind == hubgba::Kind::Off)) { ports[i].clear(); changed = true; }
                if (ImGui::Selectable("GBA as controller (no game needed)", cur.kind == hubgba::Kind::Controller)) { ports[i] = hubgba::kControllerChoice; changed = true; }
                for (size_t k = 0; k < order.size(); ++k) {
                    if (k == 0 && n_match) ImGui::TextDisabled("Best matches");
                    if (k == n_match && k > 0) { ImGui::Separator(); ImGui::TextDisabled("Other GBA games"); }
                    const hubgba::Cand& g = games[order[k]];
                    ImGui::PushID((int)k);
                    if (ImGui::Selectable(g.title.c_str(), cur.kind == hubgba::Kind::Game && cur.key == g.key)) { ports[i] = hubgba::game_choice(g.key); changed = true; }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }
    }
    if (changed) {
        ports[0].clear();
        if (!hubgba::any_on(ports)) g_store.erase(e.key);
        save_store();
    } else if (!hubgba::any_on(ports)) g_store.erase(e.key);   // never leave an all-Off entry behind
}
