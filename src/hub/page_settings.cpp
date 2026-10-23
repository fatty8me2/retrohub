#include "app.h"

/* ---------- Settings ---------- */

void settings_label(const char* title, const char* hint) {
    ImGui::PushFont(ui::F.bold);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (hint) text_wrapped_disabled(hint);
}

void page_settings() {
    page_header("Settings", "Make RetroHub yours.");

    begin_panel("set_player");
    section_title("Player");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    settings_label("Player name", "Shown to friends in online rooms.");
    char nb[40];
    std::snprintf(nb, sizeof nb, "%s", A.st.name.c_str());
    ImGui::SetNextItemWidth(sp(40.f));
    if (ImGui::InputText("##name", nb, 25)) { A.st.name = nb; mark_dirty(); }
    end_panel();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));

    begin_panel("set_lib");
    section_title("Library");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    settings_label("ROM folders", "RetroHub scans these folders (and everything below them) for your game files.");
    if (A.st.rom_folders.empty()) text_wrapped_disabled("(none yet)");
    int remove_at = -1;
    for (size_t i = 0; i < A.st.rom_folders.size(); ++i) {
        ImGui::PushID((int)i);
        if (ui::icon_button("rm", ui::Icon::Close, sp(4.f), false, "Remove this folder")) remove_at = (int)i;
        ImGui::SameLine(0, sp(1.5f));
        ImGui::AlignTextToFramePadding();
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("%s", A.st.rom_folders[i].c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopID();
    }
    if (remove_at >= 0) {
        A.st.rom_folders.erase(A.st.rom_folders.begin() + remove_at);
        mark_dirty();
        start_scan();
    }
    if (button("Add folder...", ImVec2(0, 0), false, true, ui::Icon::Plus)) add_rom_folder();
    ImGui::SameLine();
    if (button("Find my games...", ImVec2(0, 0), false, !A.scanning, ui::Icon::Search)) find_games_open();
    ImGui::SameLine();
    if (button("Rescan", ImVec2(0, 0), false, !A.scanning, ui::Icon::Refresh)) start_scan();
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    if (ImGui::Checkbox("Download cover art", &A.st.download_art)) {
        hubart::set_enabled(A.st.download_art);
        mark_dirty();
    }
    text_wrapped_disabled("Fetches box art for your games (and the games you could add) from the libretro-thumbnails project, "
                          "then keeps it on this PC. Turn this off to stay fully offline; the colourful tiles are used instead.");
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    settings_label("Smash Classic location");
    std::string se = smash_exe_path();
    text_wrapped_disabled(se.empty() ? "BattleShip.exe not found" : se);
    if (button("Browse for BattleShip.exe...", ImVec2(0, 0), false, true, ui::Icon::Folder)) locate_smash();
    if (!A.st.smash_exe.empty()) {
        ImGui::SameLine();
        if (button("Use default", ImVec2(0, 0))) { A.st.smash_exe.clear(); mark_dirty(); A.hello_dirty = true; start_scan(); }
    }
    end_panel();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));

    begin_panel("set_play");
    section_title("Playing");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    settings_label("Game Boy / Game Boy Color emulator");
    int core = A.st.gb_core == "gambatte" ? 1 : 0;
    if (ImGui::RadioButton("SameBoy (most accurate)", core == 0)) { A.st.gb_core = "sameboy"; mark_dirty(); }
    ImGui::SameLine();
    if (ImGui::RadioButton("Gambatte (lighter)", core == 1)) { A.st.gb_core = "gambatte"; mark_dirty(); }
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    if (ImGui::Checkbox("Start games fullscreen", &A.st.fullscreen)) mark_dirty();
    text_wrapped_disabled("Default for every system; each system or game can override it under Game defaults / Details.");
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    settings_label("3DS and Wii U emulators");
    {
        std::string wiiu_game;
        for (auto& g : A.lib) if (g.system == hub::System::WiiU) { wiiu_game = g.path; break; }
        static hubemu::Install az, cm, dl;   // the lookups touch the disk: refresh every 2 s, not every frame
        static double looked_at = -100;
        static std::string looked_cemu;
        if (ImGui::GetTime() - looked_at > 2.0 || looked_cemu != A.st.cemu_exe) {
            az = hubemu::find(hubemu::Kind::Azahar, "", "");
            dl = hubemu::find(hubemu::Kind::Dolphin, "", "");
            cm = hubemu::find(hubemu::Kind::Cemu, A.st.cemu_exe, wiiu_game);
            looked_at = ImGui::GetTime();
            looked_cemu = A.st.cemu_exe;
        }
        text_wrapped_disabled("Dolphin (GameCube): " + (dl.exe.empty() ? std::string("not installed - downloads the first time you play a GameCube game (about 18 MB)") : dl.exe));
        text_wrapped_disabled("Azahar (3DS): " + (az.exe.empty() ? std::string("not installed - downloads the first time you play a 3DS game (about 42 MB)") : az.exe));
        text_wrapped_disabled("Cemu (Wii U): " + (cm.exe.empty() ? std::string("not found - downloads the first time you play a Wii U game (about 25 MB)") : cm.exe + "  (" + cm.how + ")"));
        if (button("Choose Cemu.exe...", ImVec2(0, 0), false, true, ui::Icon::Folder)) {
            std::string p = pick_path(A.hwnd, false, L"Cemu.exe", L"*.exe");
            if (!p.empty()) { A.st.cemu_exe = p; mark_dirty(); }
        }
        if (!A.st.cemu_exe.empty()) {
            ImGui::SameLine();
            if (button("Use automatic", ImVec2(0, 0))) { A.st.cemu_exe.clear(); mark_dirty(); }
        }
        text_wrapped_disabled("A Cemu you already have is used as it is: RetroHub never changes its settings or moves its saves; it only copies "
                              "the game's save folder into the save vault after you play (and restores it into an empty one).");
    }
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    settings_label("Online input delay");
    ImGui::SetNextItemWidth(sp(40.f));
    if (ImGui::RadioButton("Auto (recommended)", A.st.delay_auto)) { A.st.delay_auto = true; mark_dirty(); }
    ImGui::SameLine();
    if (ImGui::RadioButton("Manual", !A.st.delay_auto)) { A.st.delay_auto = false; mark_dirty(); }
    if (A.st.delay_auto) {
        text_wrapped_disabled("The game measures your connection and picks the delay for everyone. The host's choice applies to the whole room.");
    } else {
        if (ImGui::SliderInt("##delay", &A.st.input_delay, 1, 8, "%d frames")) mark_dirty();
        text_wrapped_disabled("About " + std::to_string(A.st.input_delay * 1000 / 60) + " ms at 60 fps. Higher is smoother on slow connections but feels laggier.");
    }
    end_panel();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    gs_draw_defaults_area();

    begin_panel("set_friends");
    section_title("Friends");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    if (ImGui::Checkbox("Show friends panel", &A.st.show_friends)) mark_dirty();
    if (ImGui::Checkbox("Appear offline", &A.st.appear_offline)) mark_dirty();
    text_wrapped_disabled("Friends see you as offline, but you can still use the friends list.");
    if (ImGui::Checkbox("Notify me about invites while I'm playing", &A.st.notify_in_game)) mark_dirty();
    text_wrapped_disabled("Shows a Windows notification that never takes focus from your game. Click Join to enter the room when your game ends.");
    if (ImGui::Checkbox("Notification sound", &A.st.notif_sound)) mark_dirty();
    std::string mycode = fr_my_code();
    if (!mycode.empty()) {
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        settings_label("Your friend code", "Share it so friends can add you.");
        ImGui::TextUnformatted(mycode.c_str());
        ImGui::SameLine();
        if (button("Copy", ImVec2(0, 0))) { SDL_SetClipboardText(mycode.c_str()); set_toast("Friend code copied.", true, 3.0); }
    }
    end_panel();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));

    draw_account_section();

    begin_panel("set_app");
    section_title("Appearance");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    settings_label("Sidebar");
    int sm = A.st.sidebar_mode;
    if (ImGui::RadioButton("Automatic", sm == 0)) { A.st.sidebar_mode = 0; mark_dirty(); }
    ImGui::SameLine();
    if (ImGui::RadioButton("Always expanded", sm == 1)) { A.st.sidebar_mode = 1; mark_dirty(); }
    ImGui::SameLine();
    if (ImGui::RadioButton("Icons only", sm == 2)) { A.st.sidebar_mode = 2; mark_dirty(); }
    text_wrapped_disabled("Automatic shrinks the sidebar to icons when the window is narrow.");
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    if (ImGui::CollapsingHeader("Advanced")) {
        char sb[256];
        std::snprintf(sb, sizeof sb, "%s", A.st.server.c_str());
        ImGui::TextUnformatted("Lobby server URL");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##srv", hubnet::default_server(), sb, sizeof sb)) { A.st.server = sb; mark_dirty(); }
        ImGui::TextDisabled("Leave empty for the default server.");
    }
    end_panel();
    ImGui::Dummy(ImVec2(0, sp(2.f)));
}

