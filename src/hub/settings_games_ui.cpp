#include "console_names.h"
#include "app.h"

#include "hookshot.h"
#include "nx_runner.h"
#include "option_rows.h"

#include <algorithm>
#include <cstring>

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

/* The last item is under the mouse, or has the controller / keyboard focus. */
bool item_hot() {
    return ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled) || (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible);
}

/* The option's one-line tip (Option::help), through the usual tooltip. */
void option_help_tip(const hub::gs::Option& o, bool hot) {
    if (o.help.empty() || !hot) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(sp(40.f));
    ImGui::TextUnformatted(o.help.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

/* What follows a control on its row: the "overridden" / "set" / "from Switch" pill, the online note, the reset button. */
struct RowTrail { const char* pill = nullptr; const char* note = nullptr; bool reset = false; };
float trail_width(const RowTrail& t) {
    float w = 0.f;
    int n = 0;
    if (t.pill) { ImFont* f = ui::F.cap ? ui::F.cap : ImGui::GetFont(); w += f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.f, t.pill).x + 2 * sp(1.25f); ++n; }
    if (t.note) { w += ImGui::CalcTextSize(t.note).x; ++n; }
    if (t.reset) { w += ImGui::CalcTextSize("reset").x + ImGui::GetStyle().FramePadding.x * 2; ++n; }
    return n ? w + sp(1.f) * (float)(n - 1) : 0.f;
}

/* One option row, laid out by `lay` (option_rows.h) within `row_w` pixels. `level` 2 = per-game, 1 = system defaults. Returns true when the value
 * changed; *reset is set when its reset button was pressed. */
bool option_row(const hub::gs::Option& o, const std::string& cur, int src, int level, std::string* out, bool inherited, const hubui::OptRowLayout& lay,
                float row_w, bool* reset) {
    bool changed = false;
    ImGui::PushID(o.key.c_str());
    const float x0 = ImGui::GetCursorPosX();
    const float row_right = ImGui::GetCursorScreenPos().x + row_w;
    bool hot = false;
    if (lay.stacked) {   // too narrow for label and control side by side: the control goes under the label
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextUnformatted(o.label.c_str());
        ImGui::PopTextWrapPos();
        hot = item_hot();
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(o.label.c_str());
        hot = item_hot();
        ImGui::SameLine(0.f, 0.f);
        ImGui::SetCursorPosX(x0 + lay.control_x);
    }
    ImGui::SetNextItemWidth(lay.control_w);
    float ctrl_right = 0.f;
    if (o.kind == hub::gs::Kind::Bool) {
        bool b = cur == "true";
        if (ImGui::Checkbox("##v", &b)) { *out = b ? "true" : "false"; changed = true; }
        hot = hot || item_hot();
        ctrl_right = ImGui::GetItemRectMax().x;
    } else if (o.kind == hub::gs::Kind::Choice) {
        std::string preview = cur;
        for (auto& c : o.choices) if (c.value == cur) preview = c.label;
        const bool open = ImGui::BeginCombo("##v", preview.c_str());
        hot = !open && (hot || item_hot());   // (no tip over its own open list)
        ctrl_right = ImGui::GetItemRectMax().x;
        if (open) {
            for (auto& c : o.choices)
                if (ImGui::Selectable(c.label.c_str(), c.value == cur)) { *out = c.value; changed = true; }
            ImGui::EndCombo();
        }
    } else {
        char buf[260];
        std::snprintf(buf, sizeof buf, "%s", cur.c_str());
        if (osk::input("##v", nullptr, buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue)) { *out = buf; changed = true; }
        hot = hot || item_hot();
        ctrl_right = ImGui::GetItemRectMax().x;
    }
    option_help_tip(o, hot);
    RowTrail t;
    if (src >= level) t.pill = level == 2 ? "overridden" : inherited ? "from Switch" : "set";
    if (o.scope != hub::gs::Scope::Visual) t.note = o.scope == hub::gs::Scope::Sync ? "host decides online" : "fixed online";
    t.reset = src >= level && !inherited;
    const float tw = trail_width(t);
    if (tw > 0.f) {
        const bool same_line = hubui::option_trail_fits(ctrl_right, sp(1.f), tw, row_right);
        bool first = true;
        auto place = [&] {   // the first note follows the control when everything fits, else it starts the next line under the control
            if (first && !same_line) ImGui::SetCursorPosX(x0 + (lay.stacked ? 0.f : lay.control_x));
            else ImGui::SameLine(0, sp(1.f));
            first = false;
        };
        if (t.pill) { place(); pill(t.pill, ui::ACCENT); }
        if (t.note) { place(); ImGui::TextDisabled("%s", t.note); }
        if (t.reset) { place(); if (ImGui::SmallButton("reset")) *reset = true; }
    }
    ImGui::PopID();
    return changed;
}

/* The Ocarina of Time-only rows: "Plays with" (the native builds and Fritchii) and Fritchii's own options (only while Fritchii plays it). Never in the
 * per-system defaults. */
bool row_shown(const hub::gs::Option& o, bool oot_page, bool fritchii_planned) {
    if (o.key == hookshot::kRunnerSetting) return oot_page;
    if (o.key == "switch.oot_high_refresh" || o.key == "switch.oot_refresh_cap" || o.key == "switch.oot_aa") return oot_page && fritchii_planned;
    return true;
}

void options_grouped(const std::string& console, const std::string& game_key, int level) {   // console: library.h console id
    const hub::System sys = hub::console_system(console);
    const auto opts = hub::gs::options_for(sys);
    const hub::GameEntry* ge = level == 2 ? find_entry(game_key) : nullptr;
    const bool oot_page = ge && hookshot::supports(*ge);
    const bool fritchii_planned = oot_page && nx_planned_emulator(*ge, false) == hubemu::Kind::Fritchii;   // the one runner decision (also what Play starts)
    std::vector<const hub::gs::Option*> shown;
    std::vector<std::string> present;
    for (auto* o : opts)
        if (row_shown(*o, oot_page, fritchii_planned)) { shown.push_back(o); present.push_back(hubui::option_group_label(o->group)); }
    hub::gs::Values v = level == 2 ? hubui::effective_values(GSTORE, console, game_key, A.st.fullscreen, A.st.volume) : GSTORE.resolve_console(console, "");
    if (level == 1 && GSTORE.source_console(console, "", "fullscreen") == 0) v["fullscreen"] = A.st.fullscreen ? "true" : "false";
    if (level == 1 && GSTORE.source_console(console, "", "volume") == 0) v["volume"] = std::to_string(A.st.volume);
    const float row_w = ImGui::GetContentRegionAvail().x;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float combo_chrome = ImGui::GetFrameHeight() + style.FramePadding.x * 2;   // the arrow box and the text padding around a combo's value
    hubui::OptRowIn in;   // one column for the rows drawn here: as wide as their longest label, controls as wide as their longest value
    in.avail = row_w;
    in.gap = sp(2.f);
    in.min_control = sp(12.f);
    for (auto* o : shown) {
        in.label_max = std::max(in.label_max, ImGui::CalcTextSize(o->label.c_str()).x);
        if (o->kind == hub::gs::Kind::Choice)
            for (auto& c : o->choices) in.value_max = std::max(in.value_max, ImGui::CalcTextSize(c.label.c_str()).x + combo_chrome);
        else if (o->kind == hub::gs::Kind::Text) in.value_max = std::max(in.value_max, sp(26.f));   // a folder path: the old field width at least
    }
    const hubui::OptRowLayout lay = hubui::option_row_layout(in);   // side by side when it fits, else every control under its label
    for (const std::string& grp : hubui::option_group_order(present)) {
        std::vector<const hub::gs::Option*> rows;
        for (auto* o : shown) if (hubui::option_group_label(o->group) == grp) rows.push_back(o);
        if (rows.empty()) continue;
        ImGui::Dummy(ImVec2(0, sp(0.4f)));
        ImGui::PushFont(ui::F.bold); ImGui::TextUnformatted(grp.c_str()); ImGui::PopFont();
        for (auto* o : rows) {
            int src = GSTORE.source_console(console, game_key, o->key);
            const bool inherited = level == 1 && hub::console_is_switch2(console) && src == 1 && !GSTORE.has_console_value(console, o->key);
            std::string cur = v[o->key];
            if (o->key == hookshot::kRunnerSetting) cur = hubui::runner_shown(cur, fritchii_planned);   // what Play starts: Ocarina of Time with nothing chosen = Fritchii
            std::string nv;
            bool reset = false;
            if (option_row(*o, cur, src, level, &nv, inherited, lay, row_w, &reset)) {
                if (level == 2) GSTORE.set_game(game_key, sys, o->key, nv);
                else GSTORE.set_console(console, o->key, nv);
                gs_save();
            }
            if (reset) {
                if (level == 2) GSTORE.clear_game_key(game_key, o->key); else GSTORE.clear_console_key(console, o->key);
                gs_save();
            }
            if (level == 2 && o->key == hookshot::kRunnerSetting && ge) {   // chosen, but that native build is not on this PC: say it here, not only at Play
                static double at = -100.0; static std::string for_key, note;   // decide() touches the disk: every 2 s, not every frame
                const auto rv = v.find(o->key);
                const std::string want_key = game_key + "|" + (rv != v.end() ? rv->second : std::string());
                if (ImGui::GetTime() - at > 2.0 || ImGui::GetTime() < at || for_key != want_key) {
                    at = ImGui::GetTime(); for_key = want_key;
                    const hookshot::Decision d = hookshot::decide(*ge, v);
                    note = d.choice == hookshot::Choice::NativeNotBuilt ? std::string(hookshot::info(d.flavor).name) + " is not built on this PC yet. Build it on this game's About tab; until then Play asks what to do." : std::string();
                }
                if (!note.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN); ImGui::PushTextWrapPos(0.f); ImGui::TextUnformatted(note.c_str()); ImGui::PopTextWrapPos(); ImGui::PopStyleColor(); }
            }
        }
    }
    if (!opts.empty()) {   // say what is missing instead of silently leaving it out
        bool has_fx = false;
        for (auto* o : opts) if (o->key == "fx.filter") has_fx = true;
        if (!has_fx) { ImGui::Dummy(ImVec2(0, sp(0.4f))); text_wrapped_disabled(std::string("Screen filters and console frames are not available for ") + hub::full_name(sys) + "."); ImGui::Dummy(ImVec2(0, sp(1.5f))); }
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
    float sys_w = sp(30.f);   // as wide as the longest console name ("Nintendo Entertainment System" was cut off)
    for (int i = 0; i < nsys; ++i) sys_w = std::max(sys_w, ImGui::CalcTextSize(hub::console_name(std::string(sysl[i]))).x + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2);
    ImGui::SetNextItemWidth(std::min(sys_w, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##gssys", hub::console_name(std::string(sysl[cur])))) {
        for (int i = 0; i < nsys; ++i)
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
