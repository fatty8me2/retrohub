#include "console_names.h"
#include "app.h"

#include "hookshot.h"
#include "nx_runner.h"
#include "option_rows.h"
#include "option_sections.h"
#include "ui_controls.h"
#include "ui_sheet.h"

#include <algorithm>
#include <cstring>
#include <functional>

/* ---------- per-game settings UI: a game's Settings tab and the "Game defaults" area in Settings > Video & Games, drawn with the UI kit
 * (ui_controls.h sections and option rows). The rules (which sections start open, summaries, controls, pill words) are in option_sections.h. */

hub::gs::Store GSTORE;

namespace {
/* Open / closed option sections are remembered in hub.json ("option_sections", keyed by heading, option_sections.h), so "Advanced" stays
 * the way the player left it on every game and every console. */
void install_section_store() {
    ui::section_set_store({[](const std::string& k) { return hubui::section_state_get(A.st.option_sections, k); },
                           [](const std::string& k, bool open) {
                               const std::string n = hubui::section_state_put(A.st.option_sections, k, open);
                               if (n != A.st.option_sections) { A.st.option_sections = n; mark_dirty(); }
                               shot_trace("gs-section", k + (open ? " open" : " closed"));
                           }});
}
}  // namespace

void gs_load() {
    GSTORE.load(hub::saves::vault_dir());
    install_section_store();   // once at start-up (a cloud re-read installs the same store again)
}

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

hubemu::Kind nx_planned_emulator(const hub::GameEntry& e, bool online_or_room) {
    const bool installed = e.system == hub::System::Switch && !hubemu::find(hubemu::Kind::Fritchii, A.st.cemu_exe, e.path).exe.empty();
    return hubemu::nx_emulator_for(e, gs_values_for(e), online_or_room, GSTORE.source_console(hub::console_id(e), e.key, hubemu::kRunnerKey),
                                   hubemu::nx_fritchii_obtainable(hubemu::release_is_placeholder(hubemu::Kind::Fritchii), installed));
}

bool gs_set_game_value(const hub::GameEntry& e, const std::string& key, const std::string& value) {
    if (!GSTORE.set_game(e.key, e.system, key, value)) return false;
    if (!GSTORE.save(hub::saves::vault_dir())) set_notice("Couldn't save your game settings. Please try again.", 8.0);
    return true;
}

namespace {

void gs_save() { if (!GSTORE.save(hub::saves::vault_dir())) set_notice("Couldn't save your game settings. Please try again.", 8.0); }

/* The Ocarina of Time-only rows: "Plays with" (the native builds and Fritchii) and Fritchii's own options (only while Fritchii plays it). Never in the
 * per-system defaults. */
bool row_shown(const hub::gs::Option& o, bool oot_page, bool fritchii_planned) {
    if (o.key == hookshot::kRunnerSetting) return oot_page;
    if (o.key == "switch.oot_high_refresh" || o.key == "switch.oot_refresh_cap" || o.key == "switch.oot_aa") return oot_page && fritchii_planned;
    return true;
}

/* What one page shows: its rows under their headings, in drawn order. */
struct Row {
    const hub::gs::Option* o = nullptr;
    bool changed = false;     // set on this page (a game's own value, or a console default): Reset puts it back
    bool inherited = false;   // Nintendo Switch 2 defaults: the value only comes from the Nintendo Switch defaults
};
struct Group {
    std::string name;         // heading (option_group_label)
    std::vector<Row> rows;
    int changed = 0;
    bool runner = false;      // holds "Plays with"
};

/* `level` 2 = a game's own page, 1 = a console's defaults (game_key ""). */
std::vector<Group> page_groups(const std::string& console, const std::string& game_key, int level, bool oot_page, bool fritchii_planned) {
    std::vector<const hub::gs::Option*> shown;
    std::vector<std::string> present;
    for (auto* o : hub::gs::options_for(hub::console_system(console)))
        if (row_shown(*o, oot_page, fritchii_planned)) { shown.push_back(o); present.push_back(hubui::option_group_label(o->group)); }
    std::vector<Group> out;
    for (const std::string& name : hubui::option_group_order(present)) {
        Group g;
        g.name = name;
        for (auto* o : shown) {
            if (hubui::option_group_label(o->group) != name) continue;
            Row r;
            r.o = o;
            const int src = GSTORE.source_console(console, game_key, o->key);
            r.inherited = level == 1 && hub::console_is_switch2(console) && src == 1 && !GSTORE.has_console_value(console, o->key);
            r.changed = src >= level && !r.inherited;
            g.changed += r.changed ? 1 : 0;
            g.runner = g.runner || o->key == hookshot::kRunnerSetting;
            g.rows.push_back(r);
        }
        if (!g.rows.empty()) out.push_back(std::move(g));
    }
    return out;
}

/* A text option (the N64 texture pack folder) in the kit's look: label, pills and help, then the field under them (text entry is not part
 * of the kit, so it stays osk::input). A changed one gets a "Use default" button after the field. */
bool text_row(const hub::gs::Option& o, const std::string& cur, const ui::RowOpts& ro, std::string* out) {
    const float padx = sp(1.25f);
    const float x0 = ImGui::GetCursorPosX();
    const float w = std::max(sp(12.f), std::min(ImGui::GetContentRegionAvail().x, sp(112.f)) - 2.f * padx);
    bool changed = false;
    ImGui::Dummy(ImVec2(0.f, sp(0.5f)));
    ImGui::SetCursorPosX(x0 + padx);
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(o.label.c_str());
    for (const char* p : {ro.pill, ro.pill2}) {
        if (!p) continue;
        ImGui::SameLine(0.f, sp(1.f));
        if (p == ro.pill) ui::pill(p, ui::ACCENT);
        else ui::pill(p, ui::PANEL3, ui::TEXT2);
    }
    if (ro.help) {
        ImGui::PushFont(ui::F.cap);
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
        ImGui::TextUnformatted(ro.help);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    ImGui::PopTextWrapPos();
    const bool reset_btn = ro.pill_pressed != nullptr;
    const float bw = reset_btn ? ui::button_width("Use default") + sp(1.f) : 0.f;
    char buf[260];
    std::snprintf(buf, sizeof buf, "%s", cur.c_str());
    ImGui::SetNextItemWidth(std::max(sp(12.f), w - bw));
    if (osk::input("##v", nullptr, buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue)) { *out = buf; changed = true; }
    if (reset_btn) {
        ImGui::SameLine(0.f, sp(1.f));
        if (ui::button("Use default")) *ro.pill_pressed = true;
    }
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0.f, sp(0.5f)));
    return changed;
}

/* One option row with the kit control option_control() picks. Returns true when the player picked a new value (*out = the value to
 * store, exactly what the old combo / checkbox stored). The row's "Changed" pill sets *ro.pill_pressed (back to the default). */
bool option_row(const hub::gs::Option& o, const std::string& cur, const ui::RowOpts& ro, std::string* out) {
    ImGui::PushID(o.key.c_str());   // (rows of different options may share a label)
    bool changed = false;
    const char* label = o.label.c_str();
    switch (hubui::option_control(o)) {
    case hubui::OptCtl::Toggle: {
        bool on = hubui::option_toggle_on(o, cur);
        if (ui::toggle(label, &on, ro)) { *out = hubui::option_toggle_value(o, on); changed = true; }
        break;
    }
    case hubui::OptCtl::Stepper: {
        std::vector<const char*> items;
        for (const auto& c : o.choices) items.push_back(c.label.c_str());
        int idx = hubui::option_choice_index(o, cur);
        ui::RowOpts r = ro;
        std::string vt;
        if (idx < 0) { vt = hubui::option_value_text(o, cur); r.value_text = vt.c_str(); idx = std::max(0, hubui::option_choice_nearest(o, cur)); }
        if (ui::stepper(label, &idx, items.data(), (int)items.size(), r) && idx >= 0 && idx < (int)o.choices.size()) { *out = o.choices[(size_t)idx].value; changed = true; }
        break;
    }
    case hubui::OptCtl::Dropdown: {
        std::vector<std::string> items;
        for (const auto& c : o.choices) items.push_back(c.label);
        int idx = hubui::option_choice_index(o, cur);
        ui::RowOpts r = ro;
        std::string vt;
        if (idx < 0) { vt = hubui::option_value_text(o, cur); r.value_text = vt.c_str(); }
        if (ui::dropdown(label, &idx, items, r) && idx >= 0 && idx < (int)o.choices.size()) { *out = o.choices[(size_t)idx].value; changed = true; }
        break;
    }
    case hubui::OptCtl::Text:
        changed = text_row(o, cur, ro, out);
        break;
    }
    ImGui::PopID();
    return changed;
}

/* "Plays with" chose a native build that is not on this PC: say it under the row, not only at Play. */
void runner_note(const hub::GameEntry& ge, const std::string& game_key, const hub::gs::Values& v) {
    static double at = -100.0; static std::string for_key, note;   // decide() touches the disk: every 2 s, not every frame
    const auto rv = v.find(hookshot::kRunnerSetting);
    const std::string want_key = game_key + "|" + (rv != v.end() ? rv->second : std::string());
    if (ImGui::GetTime() - at > 2.0 || ImGui::GetTime() < at || for_key != want_key) {
        at = ImGui::GetTime(); for_key = want_key;
        const hookshot::Decision d = hookshot::decide(ge, v);
        note = d.choice == hookshot::Choice::NativeNotBuilt ? std::string(hookshot::info(d.flavor).name) + " is not built on this PC yet. Build it on this game's About tab; until then Play asks what to do." : std::string();
    }
    if (note.empty()) return;
    const float padx = sp(1.25f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padx);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(sp(12.f), std::min(ImGui::GetContentRegionAvail().x, sp(112.f)) - 2.f * padx));
    ImGui::TextUnformatted(note.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

/* One page of option rows: a game's Settings tab (level 2) or one console's Game defaults (level 1, game_key ""). Every heading is a kit
 * section (open / closed remembered by heading) and all rows share one option group, so the label and control columns line up across the
 * sections. `lead(changed rows)` draws first, inside the group. Draws into whatever region hosts it (a modal body, the game page sheet, the
 * Settings page): it never begins its own scrolling child. */
void options_grouped(const std::string& console, const std::string& game_key, int level, const std::function<void(int)>& lead = nullptr) {
    const hub::System sys = hub::console_system(console);
    const hub::GameEntry* ge = level == 2 ? find_entry(game_key) : nullptr;
    const bool oot_page = ge && hookshot::supports(*ge);
    const bool fritchii_planned = oot_page && nx_planned_emulator(*ge, false) == hubemu::Kind::Fritchii;   // the one runner decision (also what Play starts)
    const std::vector<Group> groups = page_groups(console, game_key, level, oot_page, fritchii_planned);
    hub::gs::Values v = level == 2 ? hubui::effective_values(GSTORE, console, game_key, A.st.fullscreen, A.st.volume) : GSTORE.resolve_console(console, "");
    if (level == 1 && GSTORE.source_console(console, "", "fullscreen") == 0) v["fullscreen"] = A.st.fullscreen ? "true" : "false";
    if (level == 1 && GSTORE.source_console(console, "", "volume") == 0) v["volume"] = std::to_string(A.st.volume);
    int rows = 0, changed = 0;
    for (const Group& g : groups) { rows += (int)g.rows.size(); changed += g.changed; }
    /* The help shows under the label (at most three lines). Where it will not fit there, it is also the hover text. */
    const float label_col = std::max(sp(20.f), std::min(ImGui::GetContentRegionAvail().x, sp(112.f)) - sp(2.5f) - sp(30.f) - sp(2.f));   // next to a typical control
    auto help_cut = [&](const std::string& h) { return ui::F.cap->CalcTextSizeA(ui::F.cap->FontSize, FLT_MAX, 0.f, h.c_str()).x > label_col * 2.8f; };
    const std::string gid = "gs_rows|" + console + "|" + game_key;
    ui::option_group_begin(gid.c_str());
    if (lead) lead(changed);
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        const Group& g = groups[gi];
        ui::SectionOpts so;
        so.default_open = hubui::option_section_default_open(g.name, (int)gi, g.runner, rows);
        so.persist_key = g.name.c_str();
        const int known = hubui::section_state_get(A.st.option_sections, g.name);   // the section follows the same store (ui_controls.h)
        const bool open_now = known >= 0 ? known == 1 : so.default_open;
        const std::string summary = open_now ? std::string() : hubui::option_section_summary((int)g.rows.size(), g.changed);
        if (!summary.empty()) so.summary = summary.c_str();
        if (!ui::section(g.name.c_str(), so)) continue;
        for (const Row& r : g.rows) {
            const hub::gs::Option& o = *r.o;
            std::string cur = v[o.key];
            if (o.key == hookshot::kRunnerSetting) cur = hubui::runner_shown(cur, fritchii_planned);   // what Play starts: Ocarina of Time with nothing chosen = Fritchii
            bool reset = false;
            ui::RowOpts ro;
            if (!o.help.empty()) ro.help = o.help.c_str();
            if (!o.help.empty() && help_cut(o.help)) ro.tooltip = o.help.c_str();
            ro.pill = hubui::option_changed_pill(r.changed, false);
            if (r.changed) ro.pill_pressed = &reset;
            ro.pill2 = r.inherited ? hubui::option_changed_pill(false, true) : hubui::option_online_pill(o.scope);
            std::string nv;
            if (option_row(o, cur, ro, &nv)) {
                if (level == 2) GSTORE.set_game(game_key, sys, o.key, nv);
                else GSTORE.set_console(console, o.key, nv);
                gs_save();
                shot_trace("gs-set", (level == 2 ? game_key : console) + " " + o.key + "=" + nv);
            }
            if (reset) {
                if (level == 2) GSTORE.clear_game_key(game_key, o.key); else GSTORE.clear_console_key(console, o.key);
                gs_save();
                shot_trace("gs-clear", (level == 2 ? game_key : console) + " " + o.key);
            }
            if (level == 2 && o.key == hookshot::kRunnerSetting && ge) runner_note(*ge, game_key, v);
        }
    }
    ui::option_group_end();
    if (!groups.empty()) {   // say what is missing instead of silently leaving it out
        bool has_fx = false;
        for (const Group& g : groups) for (const Row& r : g.rows) if (r.o->key == "fx.filter") has_fx = true;
        if (!has_fx) { ImGui::Dummy(ImVec2(0, sp(1.f))); text_wrapped_disabled(std::string("Screen filters and console frames are not available for ") + hub::full_name(sys) + "."); }
    }
}

/* "Reset to defaults" and its confirm: the dialog is drawn from the page, so it stacks on top of whatever hosts the page. */
bool reset_button(bool enabled) {
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    return ui::button("Reset to defaults", ImVec2(0, 0), false, enabled, ui::Icon::Refresh);
}
bool reset_confirm(const char* id, const char* title, const char* subtitle, bool* ask, const std::string& what, const std::string& kept) {
    ui::DialogOpts o;
    o.subtitle = subtitle;
    bool yes = false;
    if (ui::dialog_begin(id, title, ask, ui::DialogSize::Small, o)) {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextUnformatted(what.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
        ImGui::TextUnformatted(kept.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ui::dialog_buttons();
        if (ui::button("Reset", ImVec2(0, 0), true)) { yes = true; *ask = false; }
        ImGui::SameLine();
        if (ui::button("Cancel")) *ask = false;
        ImGui::SetItemDefaultFocus();
        ui::dialog_end();
    }
    return yes;
}

std::string settings_count(int n) { return std::to_string(n) + (n == 1 ? " setting" : " settings"); }

}  // namespace

bool gs_set_system_value(hub::System sys, const std::string& key, const std::string& value) {
    std::string n;
    const hub::gs::Option* o = hub::gs::find_option(key);
    if (o && !hub::gs::normalize(*o, value, &n)) return false;
    GSTORE.set_system(sys, key, o ? n : value);
    shot_trace("gs-set", std::string(hub::system_short(sys)) + " " + key + "=" + (o ? n : value));
    const bool saved = GSTORE.save(hub::saves::vault_dir());
    if (!saved) set_notice("Couldn't save your game settings. Please try again.", 8.0);
    return saved;
}

bool gs_has_game_section(const hub::GameEntry& e) { return !hub::gs::options_for(e.system).empty(); }

void gs_draw_game_section(const hub::GameEntry& e) {   // body of the game page's "Settings" tab
    if (!gs_has_game_section(e)) return;
    const bool over = GSTORE.has_game_overrides(e.key);
    const std::string console = hub::console_id(e);
    ImGui::PushID("gs_game");
    options_grouped(console, e.key, 2, [&](int changed) {
        if (changed > 0) text_wrapped_disabled(settings_count(changed) + " changed for this game.");
        else if (over) text_wrapped_disabled("This game has its own settings.");
        else text_wrapped_disabled(std::string("Changes here are for this game only. The rest use the ") + hub::console_name(console) + " defaults in Settings.");
    });
    static bool ask = false;
    static std::string ask_key;
    if (reset_button(over)) { ask = true; ask_key = e.key; }
    if (ask && ask_key != e.key) ask = false;   // (the page now shows another game)
    if (reset_confirm("gs_reset_game", "Reset this game's settings?", e.title.c_str(), &ask,
                      std::string("Every setting on this page goes back to the default for ") + hub::console_name(console) + " games.",
                      "Your saves, save states and screenshots stay as they are.")) {
        GSTORE.reset_game(e.key);
        gs_save();
        shot_trace("gs-reset", e.key);
    }
    ImGui::PopID();
}

void gs_draw_defaults_area() {
    begin_panel("set_gamedefs");
    section_title("Game defaults");
    text_wrapped_disabled("Defaults for every game of a system. Any game can override them under Details > Settings. Saved in your save vault, so they follow you to every PC.");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    static const char* const sysl[] = {"n64", "gc", "wiiu", "3ds", "nds", "ps1", "ps2", "psp", "gba", "gb", "gbc", "nes", "snes", "switch", "switch2"};   // console ids: Switch 2 right after Switch
    const int nsys = (int)(sizeof(sysl) / sizeof(sysl[0]));
    static int cur = 0;
    if (const char* s = g_headless ? std::getenv("HUB_SHOT_GS_SYS") : nullptr) {   // screenshot aid: a console id of sysl ("ps1", "switch2") or an index into it
        static bool once = true;
        if (once) {
            once = false;
            cur = ((std::atoi(s) % nsys) + nsys) % nsys;
            for (int i = 0; i < nsys; ++i) if (std::strcmp(s, sysl[i]) == 0) cur = i;
        }
    }
    static std::vector<std::string> names;
    if (names.empty()) for (int i = 0; i < nsys; ++i) names.push_back(hub::console_name(std::string(sysl[i])));
    const std::string console = sysl[cur];
    ImGui::PushID("gs_defaults");
    options_grouped(console, "", 1, [&](int) { ui::dropdown("System", &cur, names); });
    if (hub::console_is_switch2(console)) text_wrapped_disabled("Nintendo Switch 2 uses the Nintendo Switch defaults until you change a setting here.");
    bool any = false;   // something is set for this console (also what the rows do not show, such as the Switch-wide "play with" choice)
    for (const hub::gs::Option* o : hub::gs::options_for(hub::console_system(console))) any = any || GSTORE.has_console_value(console, o->key);
    static bool ask = false;
    static std::string ask_console;
    if (reset_button(any)) { ask = true; ask_console = console; }
    const std::string name = hub::console_name(ask_console.empty() ? console : ask_console);
    const std::string title = "Reset the " + name + " defaults?";
    if (reset_confirm("gs_reset_defaults", title.c_str(), nullptr, &ask, "Every setting here goes back to RetroHub's own default for " + name + " games.",
                      hub::console_is_switch2(ask_console) ? std::string("Nintendo Switch 2 then uses the Nintendo Switch defaults again. Games with their own settings keep them.")
                                                           : std::string("Games with their own settings keep them."))) {
        GSTORE.reset_console(ask_console);
        gs_save();
        shot_trace("gs-reset", ask_console);
    }
    ImGui::PopID();
    end_panel();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
}
