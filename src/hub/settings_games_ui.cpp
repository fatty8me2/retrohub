#include "app.h"

/* ---------- per-game settings UI (details modal section + "Game defaults" area in Settings) ---------- */

hub::gs::Store GSTORE;

void gs_load() { GSTORE.load(hub::saves::vault_dir()); }

hub::gs::Values gs_values_for(const hub::GameEntry& e) {
    return hubui::effective_values(GSTORE, e.system, e.key, A.st.fullscreen);
}

namespace {

const char* group_label(const std::string& g) {
    if (g == "Video") return "Graphics";
    if (g == "Color") return "Display";
    if (g == "Emulation") return "Gameplay";
    return g.c_str();
}

void gs_save() { GSTORE.save(hub::saves::vault_dir()); }

/* One option row. `level` 2 = per-game, 1 = system defaults. Returns true when the value changed. */
bool option_row(const hub::gs::Option& o, const std::string& cur, int src, int level, std::string* out) {
    bool changed = false;
    ImGui::PushID(o.key.c_str());
    ImGui::AlignTextToFramePadding();
    ImGui::SetNextItemWidth(sp(30.f));
    ImGui::TextUnformatted(o.label.c_str());
    ImGui::SameLine(sp(32.f));
    ImGui::SetNextItemWidth(sp(26.f));
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
        pill(level == 2 ? "overridden" : "set", ui::ACCENT);
    }
    if (o.scope != hub::gs::Scope::Visual) {
        ImGui::SameLine(0, sp(1.f));
        ImGui::TextDisabled(o.scope == hub::gs::Scope::Sync ? "host decides online" : "fixed online");
    }
    ImGui::PopID();
    return changed;
}

void options_grouped(hub::System sys, const std::string& game_key, int level) {
    auto opts = hub::gs::options_for(sys);
    const char* order[] = {"Graphics", "Display", "Gameplay", "Advanced"};
    for (const char* grp : order) {
        bool any = false;
        for (auto* o : opts) {
            if (std::string(group_label(o->group)) != grp) continue;
            if (!any) { ImGui::Dummy(ImVec2(0, sp(0.4f))); ImGui::PushFont(ui::F.bold); ImGui::TextUnformatted(grp); ImGui::PopFont(); any = true; }
            hub::gs::Values v = level == 2 ? hubui::effective_values(GSTORE, sys, game_key, A.st.fullscreen)
                                           : GSTORE.resolve(sys, "");
            if (level == 1 && o->key == "fullscreen" && GSTORE.source(sys, "", "fullscreen") == 0) v["fullscreen"] = A.st.fullscreen ? "true" : "false";
            int src = GSTORE.source(sys, game_key, o->key);
            std::string nv;
            if (option_row(*o, v[o->key], src, level, &nv)) {
                if (level == 2) GSTORE.set_game(game_key, sys, o->key, nv);
                else GSTORE.set_system(sys, o->key, nv);
                gs_save();
            }
            if (src >= level) {
                ImGui::SameLine(0, sp(1.f));
                ImGui::PushID(o->key.c_str());
                if (ImGui::SmallButton("reset")) {
                    if (level == 2) GSTORE.clear_game_key(game_key, o->key); else GSTORE.clear_system_key(sys, o->key);
                    gs_save();
                }
                ImGui::PopID();
            }
        }
    }
}

}  // namespace

void gs_draw_game_section(const hub::GameEntry& e) {
    static bool shot_open = std::getenv("HUB_SHOT_GS") != nullptr;
    if (shot_open) { ImGui::SetNextItemOpen(true, ImGuiCond_Once); shot_open = false; }
    bool over = GSTORE.has_game_overrides(e.key);
    std::string title = over ? "Settings  (this game has overrides)" : "Settings";
    if (!ImGui::CollapsingHeader(title.c_str())) return;
    if (hub::gs::options_for(e.system).empty()) { ImGui::TextDisabled("No per-game settings for this system."); return; }
    ImGui::BeginChild("##gsopts", ImVec2(0, sp(24.f)), ImGuiChildFlags_None);
    options_grouped(e.system, e.key, 2);
    ImGui::EndChild();
    if (button("Reset this game", ImVec2(0, 0), false, over)) { GSTORE.reset_game(e.key); gs_save(); }
}

void gs_draw_defaults_area() {
    begin_panel("set_gamedefs");
    section_title("Game defaults");
    text_wrapped_disabled("Defaults for every game of a system. Any game can override them under Details > Settings. Saved in your save vault, so they follow you to every PC.");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    static const hub::System sysl[] = {hub::System::N64, hub::System::GC, hub::System::WiiU, hub::System::N3DS,
                                       hub::System::NDS, hub::System::PS2, hub::System::GBA, hub::System::GB, hub::System::GBC,
                                       hub::System::NES, hub::System::SNES};
    static int cur = 0;
    if (const char* s = std::getenv("HUB_SHOT_GS_SYS")) { static bool once = true; if (once) { cur = std::atoi(s) % 11; once = false; } }
    ImGui::SetNextItemWidth(sp(30.f));
    if (ImGui::BeginCombo("##gssys", hub::system_name(sysl[cur]))) {
        for (int i = 0; i < 11; ++i)
            if (ImGui::Selectable(hub::system_name(sysl[i]), i == cur)) cur = i;
        ImGui::EndCombo();
    }
    options_grouped(sysl[cur], "", 1);
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    if (button("Reset these defaults", ImVec2(0, 0))) { GSTORE.reset_system(sysl[cur]); gs_save(); }
    end_panel();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
}
