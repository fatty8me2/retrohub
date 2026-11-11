#include "app.h"
#include "console_names.h"
#include "convenience.h"
#include "hookshot_ui.h"

/* Plan 4 Wave 2 C: per-frame convenience work (pad glyphs + battery, Discord, firewall probe, command palette). */
void conv_frame() {
    if (g_headless) {   // screenshot aid: HUB_SHOT_SETTINGS_TAB=<0..4>
        static bool done = false;
        if (!done) { done = true; if (const char* t = std::getenv("HUB_SHOT_SETTINGS_TAB")) A.st.settings_tab = std::atoi(t); }
    }
    padinfo_tick();
    netprobe_tick();
    discord_tick();
    palette_draw();
    battery_draw();
}

void draw_overlays() {
    draw_emulator_dialogs();
    draw_ps1_dialogs();
    draw_switch_dialogs();
    draw_hookshot_run_dialogs();
    draw_wizard();
    draw_username_overlays();
    draw_welcome();
    draw_find_games();
    draw_add_games();
    draw_details_modal();
    custom_draw(0);   // customize_ui.cpp: Add to shelf / Manage shelves opened outside the details modal; polls synced prefs
    cloud_ui_overlays();
    draw_cheats_modal();
    draw_quick_menu();
    kt_draw_modals();
    party_draw();     // Plan 4 hooks (stubs until each lane lands)
    bracket_draw();
    hack_draw();
    hookshot_ui_tick();
    boot_draw_overlay();
    conv_frame();
}

void page_about() {
    page_header("Updates", "Your version, new releases and patch notes");
    begin_panel("about");
    ImGui::PushFont(ui::F.h2);
    ImGui::TextUnformatted("RetroHub");
    ImGui::PopFont();
    ImGui::TextDisabled("Version %s", HUB_VERSION_STR);
    ImGui::Dummy(ImVec2(0, 6 * S));
    if (hubupdate::downloading()) {
        float p = hubupdate::download_progress();
        ImGui::ProgressBar(p < 0 ? 0.f : p, ImVec2(sp(40.f), 0), p < 0 ? "Downloading..." : nullptr);
        text_wrapped_disabled(hubupdate::status().c_str());
    } else {
        ImGui::BeginDisabled(hubupdate::checking());
        if (ImGui::Button(hubupdate::checking() ? "Checking..." : "Check for updates")) {
            A.update_prompt_dismissed = false;
            A.update_check_requested = true;
            hubupdate::check_async(true);
        }
        ImGui::EndDisabled();
        if (hubupdate::update_available()) {
            ImGui::SameLine();
            std::string lbl = "Update to " + hubupdate::latest_version();
            if (ImGui::Button(lbl.c_str())) hubupdate::start_update();
        }
        std::string st = hubupdate::status();
        if (!st.empty()) text_wrapped_disabled(st.c_str());
        else if (A.update_check_requested && !hubupdate::checking() && !hubupdate::update_available() && !hubupdate::check_failed())
            text_wrapped_disabled("You're on the latest version.");
    }
    text_wrapped_disabled("RetroHub checks for updates each time it starts. Updating downloads the new version, installs it and reopens RetroHub. "
                          "Your saves and settings are kept.");
    ImGui::Dummy(ImVec2(0, 6 * S));
    {   // the console list comes from the one name table (console_names.h)
        static const hub::System order[] = {hub::System::NES, hub::System::SNES, hub::System::N64, hub::System::GC, hub::System::WiiU,
                                            hub::System::GB, hub::System::GBC, hub::System::GBA, hub::System::NDS, hub::System::N3DS,
                                            hub::System::PS1, hub::System::PS2, hub::System::PSP, hub::System::Switch};
        std::string list;
        const size_t n = sizeof order / sizeof order[0];
        for (size_t i = 0; i < n; ++i) {
            list += hub::full_name(order[i]);
            list += i + 2 < n ? ", " : i + 2 == n ? " and " : "";
        }
        ImGui::TextWrapped("One library for your %s games. Pick up where you left off, and your saves follow you between PCs.", list.c_str());
    }
    ImGui::Dummy(ImVec2(0, 6 * S));
    text_wrapped_disabled("You can play online with friends on every console except PlayStation Portable and Nintendo Switch. "
                          "Host a room and share its 6-character code, or join a friend's room with theirs. "
                          "On Wii U the host's game is streamed to everyone else. "
                          "You bring your own game files; none are included. Some game programs download the first time you need them.");
    end_panel();
    draw_changelog_list();
}
