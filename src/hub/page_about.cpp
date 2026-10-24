#include "app.h"

void draw_overlays() {
    draw_emulator_dialogs();
    draw_ps1_dialogs();
    draw_wizard();
    draw_find_games();
    draw_add_games();
    draw_details_modal();
    kt_draw_modals();
}

void page_about() {
    page_header("About", "");
    begin_panel("about");
    ImGui::PushFont(ui::F.h2);
    ImGui::TextUnformatted("RetroHub");
    ImGui::PopFont();
    ImGui::TextDisabled("Version %s", HUB_VERSION_STR);
    ImGui::Dummy(ImVec2(0, 6 * S));
    ImGui::TextWrapped("One library for N64, GameCube, Wii U, Nintendo 3DS, Game Boy Advance, Game Boy and Game Boy Color: one-click resume, "
                       "room codes, online play, and saves that follow you between PCs.");
    ImGui::Dummy(ImVec2(0, 6 * S));
    text_wrapped_disabled("Smash Classic runs as a native port with rollback netplay. N64 games run in the built-in "
                          "mupen64plus runner and Game Boy games in the built-in libretro runner, both with lockstep input netplay. "
                          "GameCube games use Dolphin, Nintendo 3DS games use Azahar and Wii U games use Cemu; those emulators are downloaded "
                          "the first time you need them (or an existing Cemu is used). You supply your own game files; none are included.");
    ImGui::Dummy(ImVec2(0, 6 * S));
    ImGui::Dummy(ImVec2(0, 10 * S));
    ImGui::PushFont(ui::F.h2);
    ImGui::TextUnformatted("Updates");
    ImGui::PopFont();
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
    text_wrapped_disabled("RetroHub checks for updates each time it starts. Updating downloads the new version, "
                          "installs it over this folder (your saves and settings are kept) and reopens RetroHub.");
    end_panel();
}

