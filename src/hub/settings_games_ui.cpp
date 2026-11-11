#include "app.h"

#include "hookshot.h"
#include "nx_runner.h"

/* ---------- per-game settings UI (details modal section + "Game defaults" area in Settings) ---------- */

hub::gs::Store GSTORE;

void gs_load() { GSTORE.load(hub::saves::vault_dir()); }

void gs_cloud_sync_tick() {   // saves-F11: the engine merged settings / cheats from another PC into the files: re-read them (saving merges again, so nothing is overwritten either way)
    static unsigned seen = hub::saves::cloud_settings_rev();
    const unsigned now = hub::saves::cloud_settings_rev();
    if (now == seen) return;
    seen = now;
    gs_load();
    cheats_reload_from_vault();
}

hub::gs::Values gs_values_for(const hub::GameEntry& e) {
    return hubui::effective_values(GSTORE, e, A.st.fullscreen, A.st.volume);   // the game's own console layer (Switch 2 on top of Switch)
}

bool gs_set_game_value(const hub::GameEntry& e, const std::string& key, const std::string& value) {
    if (!GSTORE.set_game(e.key, e.system, key, value)) return false;
    if (!GSTORE.save(hub::saves::vault_dir())) set_notice("Couldn't save your game settings. Please try again.", 8.0);
    return true;
}

namespace {

const char* group_label(const std::string& g) {
    if (g == "Video") return "Graphics";
    if (g == "Color") return "Display";
    if (g == "Emulation") return "Gameplay";
    return g.c_str();
}

void gs_save() { if (!GSTORE.save(hub::saves::vault_dir())) set_notice("Couldn't save your game settings. Please try again.", 8.0); }

/* One option row. `level` 2 = per-game, 1 = system defaults. Returns true when the value changed. */
bool option_row(const hub::gs::Option& o, const std::string& cur, int src, int level, std::string* out, bool inherited = false) {
    bool changed = false;
    ImGui::PushID(o.key.c_str());
    ImGui::AlignTextToFramePadding();
    ImGui::SetNextItemWidth(sp(30.f));
    ImGui::TextUnformatted(o.label.c_str());
    ImGui::SameLine(sp(32.f));
    ImGui::SetNextItemWidth(sp(o.key == "switch.runner" ? 36.f : 26.f));   // (the runner labels are the longest)
    if (o.kind == hub::gs::Kind::Bool) {
        bool b = cur == "true";
        if (ImGui::Checkbox("##v", &b)) { *out = b ? "true" : "false"; changed = true; }
    } else if (o.kind == hub::gs::Kind::Choice) {
        std::string preview = cur;
        for (auto& c : o.choices) if (c.value == cur) preview = c.label;
        if (ImGui::BeginCombo("##v", preview.c_str())) {
            for (auto& c : o.choices)
                if (ImGui::Selectable(c.label.c_str(), c.value == cur)) { *out = c.value; changed = true; }
            ImGui::EndCombo();
        }
    } else {
        char buf[260];
        std::snprintf(buf, sizeof buf, "%s", cur.c_str());
        if (osk::input("##v", nullptr, buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue)) { *out = buf; changed = true; }
    }
    if (src >= level) {
        ImGui::SameLine(0, sp(1.f));
        pill(level == 2 ? "overridden" : inherited ? "from Switch" : "set", ui::ACCENT);
    }
    if (o.scope != hub::gs::Scope::Visual) {
        ImGui::SameLine(0, sp(1.f));
        ImGui::TextDisabled(o.scope == hub::gs::Scope::Sync ? "host decides online" : "fixed online");
    }
    ImGui::PopID();
    return changed;
}

void options_grouped(const std::string& console, const std::string& game_key, int level) {   // console: library.h console id
    const hub::System sys = hub::console_system(console);
    auto opts = hub::gs::options_for(sys);
    const char* order[] = {"Graphics", "Display", "Audio", "Gameplay", "Advanced"};
    for (const char* grp : order) {
        bool any = false;
        for (auto* o : opts) {
            if (std::string(group_label(o->group)) != grp) continue;
            if (o->key == hookshot::kRunnerSetting) {   // only for the one game Hookshot runs, never in the per-system defaults
                const hub::GameEntry* ge = level == 2 ? find_entry(game_key) : nullptr;
                if (!ge || !hookshot::supports(*ge)) continue;
            }
            if (o->key == "switch.oot_high_refresh" || o->key == "switch.oot_refresh_cap" || o->key == "switch.oot_aa") {   // Fritchii's Ocarina of Time options: that game, played with Fritchii, never in the per-system defaults
                const hub::GameEntry* ge = level == 2 ? find_entry(game_key) : nullptr;
                if (!ge || !hookshot::supports(*ge) ||
                    hubemu::nx_emulator_for(*ge, gs_values_for(*ge), false, GSTORE.source_console(console, game_key, hubemu::kRunnerKey), true) != hubemu::Kind::Fritchii) continue;
            }
            if (!any) { ImGui::Dummy(ImVec2(0, sp(0.4f))); ImGui::PushFont(ui::F.bold); ImGui::TextUnformatted(grp); ImGui::PopFont(); any = true; }
            hub::gs::Values v = level == 2 ? hubui::effective_values(GSTORE, console, game_key, A.st.fullscreen, A.st.volume)
                                           : GSTORE.resolve_console(console, "");
            if (level == 1 && o->key == "fullscreen" && GSTORE.source_console(console, "", "fullscreen") == 0) v["fullscreen"] = A.st.fullscreen ? "true" : "false";
            if (level == 1 && o->key == "volume" && GSTORE.source_console(console, "", "volume") == 0) v["volume"] = std::to_string(A.st.volume);
            int src = GSTORE.source_console(console, game_key, o->key);
            const bool inherited = level == 1 && hub::console_is_switch2(console) && src == 1 && !GSTORE.has_console_value(console, o->key);
            std::string nv;
            if (option_row(*o, v[o->key], src, level, &nv, inherited)) {
                if (level == 2) GSTORE.set_game(game_key, sys, o->key, nv);
                else GSTORE.set_console(console, o->key, nv);
                gs_save();
            }
            if (src >= level && !inherited) {
                ImGui::SameLine(0, sp(1.f));
                ImGui::PushID(o->key.c_str());
                if (ImGui::SmallButton("reset")) {
                    if (level == 2) GSTORE.clear_game_key(game_key, o->key); else GSTORE.clear_console_key(console, o->key);
                    gs_save();
                }
                ImGui::PopID();
            }
        }
    }
}

}  // namespace

bool gs_set_system_value(hub::System sys, const std::string& key, const std::string& value) {
    std::string n;
    const hub::gs::Option* o = hub::gs::find_option(key);
    if (o && !hub::gs::normalize(*o, value, &n)) return false;
    GSTORE.set_system(sys, key, o ? n : value);
    const bool saved = GSTORE.save(hub::saves::vault_dir());
    if (!saved) set_notice("Couldn't save your game settings. Please try again.", 8.0);
    return saved;
}

bool gs_has_game_section(const hub::GameEntry& e) { return !hub::gs::options_for(e.system).empty(); }

void gs_draw_game_section(const hub::GameEntry& e) {   // body of the details "Settings" tab
    bool over = GSTORE.has_game_overrides(e.key);
    if (over) ImGui::TextDisabled("This game has its own overrides.");
    if (!gs_has_game_section(e)) return;
    options_grouped(hub::console_id(e), e.key, 2);
    if (button("Reset this game", ImVec2(0, 0), false, over)) { GSTORE.reset_game(e.key); gs_save(); }
}

void gs_draw_defaults_area() {
    begin_panel("set_gamedefs");
    section_title("Game defaults");
    text_wrapped_disabled("Defaults for every game of a system. Any game can override them under Details > Settings. Saved in your save vault, so they follow you to every PC.");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    static const char* const sysl[] = {"n64", "gc", "wiiu", "3ds", "nds", "ps2", "psp", "gba", "gb", "gbc", "nes", "snes", "switch", "switch2"};   // console ids: Switch 2 right after Switch
    static int cur = 0;
    if (const char* s = std::getenv("HUB_SHOT_GS_SYS")) { static bool once = true; if (once) { cur = std::atoi(s) % (int)(sizeof(sysl) / sizeof(sysl[0])); once = false; } }
    ImGui::SetNextItemWidth(sp(30.f));
    if (ImGui::BeginCombo("##gssys", hub::console_name(std::string(sysl[cur])))) {
        for (int i = 0; i < (int)(sizeof(sysl) / sizeof(sysl[0])); ++i)
            if (ImGui::Selectable(hub::console_name(std::string(sysl[i])), i == cur)) cur = i;
        ImGui::EndCombo();
    }
    options_grouped(sysl[cur], "", 1);
    if (hub::console_is_switch2(sysl[cur])) text_wrapped_disabled("Nintendo Switch 2 uses the Nintendo Switch defaults until you change a setting here.");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    if (button("Reset these defaults", ImVec2(0, 0))) { GSTORE.reset_console(sysl[cur]); gs_save(); }
    end_panel();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
}
