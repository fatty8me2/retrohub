#include "app.h"
#include "pcsx2.h"
#include "gamepack.h"

/* ---------- Settings ---------- */

void settings_label(const char* title, const char* hint) {
    ImGui::PushFont(ui::F.bold);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (hint) text_wrapped_disabled(hint);
}

namespace {

/* Emulator lookups touch the disk: refresh every 2 s, not every frame (Video & Games tab). */
struct EmuLook { hubemu::Install az, cm, dl, ps; hubemu::Bios bi; };
const EmuLook& emu_look() {
    static EmuLook e;
    static double looked_at = -100;
    static std::string looked_cemu;
    if (ImGui::GetTime() - looked_at > 2.0 || looked_cemu != A.st.cemu_exe + A.st.ps2_bios_dir) {
        std::string wiiu_game;
        for (auto& g : A.lib) if (g.system == hub::System::WiiU) { wiiu_game = g.path; break; }
        e.az = hubemu::find(hubemu::Kind::Azahar, "", "");
        e.dl = hubemu::find(hubemu::Kind::Dolphin, "", "");
        e.ps = hubemu::find(hubemu::Kind::Pcsx2, "", "");
        e.bi = hubemu::pcsx2_find_bios(hubemu::pcsx2_bios_search_dirs(A.st.ps2_bios_dir, A.st.rom_folders));
        e.cm = hubemu::find(hubemu::Kind::Cemu, A.st.cemu_exe, wiiu_game);
        looked_at = ImGui::GetTime();
        looked_cemu = A.st.cemu_exe + A.st.ps2_bios_dir;
    }
    return e;
}

void gap() { ImGui::Dummy(ImVec2(0, sp(0.5f))); }

/* "Unpacked games" cache section. Disk usage is measured on a background thread (on open and after clearing), never per frame. */
struct CacheUi {
    std::atomic<uint64_t> bytes{0};
    std::atomic<bool> measuring{false}, clearing{false}, clear_done{false};
    bool measured = false, confirm = false;
    int last_frame = -10;
    std::vector<hub::SkippedArchive> skipped;
};
CacheUi CU;

void cache_measure() {
    if (CU.measuring.exchange(true)) return;
    std::thread([] { CU.bytes = hub::rom_cache_bytes(); CU.measuring = false; }).detach();
}

void cache_section() {
    int fr = ImGui::GetFrameCount();
    if (!CU.measured || fr - CU.last_frame > 2) {   // tab just opened
        CU.measured = true;
        cache_measure();
        CU.skipped = hub::scan_skipped_archives();
    }
    CU.last_frame = fr;
    if (CU.clear_done.exchange(false)) { CU.clearing = false; cache_measure(); start_scan(); }

    ImGui::Dummy(ImVec2(0, sp(1.f)));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    settings_label("Unpacked games");
    char line[96];
    std::snprintf(line, sizeof line, "Unpacked games use %.1f GB", (double)CU.bytes.load() / (double)(1ull << 30));
    ImGui::TextUnformatted(line);
    if (CU.clearing) {
        text_wrapped_disabled("Clearing...");
    } else if (!CU.confirm) {
        if (button("Clear cache", ImVec2(0, 0), false, !A.scanning)) CU.confirm = true;
    } else {
        text_wrapped_disabled("Delete all unpacked games? Your archives are not touched; cartridges unpack again on the next scan.");
        if (button("Yes, clear", ImVec2(0, 0))) {
            CU.confirm = false;
            CU.clearing = true;
            std::thread([] { hub::rom_cache_clear(); CU.clear_done = true; }).detach();
        }
        ImGui::SameLine();
        if (button("Cancel", ImVec2(0, 0))) CU.confirm = false;
    }
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    static const int gbs[] = {10, 20, 40, 80, 0};
    static const char* labels[] = {"10 GB", "20 GB", "40 GB", "80 GB", "No limit"};
    int cur = 2;
    for (int i = 0; i < 5; ++i) if (gbs[i] == A.st.rom_cache_limit_gb) cur = i;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Limit");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(sp(20.f));
    if (ImGui::BeginCombo("##romcachelimit", labels[cur])) {
        for (int i = 0; i < 5; ++i)
            if (ImGui::Selectable(labels[i], i == cur)) {
                A.st.rom_cache_limit_gb = gbs[i];
                hub::rom_cache_set_limit((uint64_t)gbs[i] << 30);
                mark_dirty();
                std::thread([] { hub::rom_cache_trim(hub::rom_cache_limit()); }).detach();
            }
        ImGui::EndCombo();
    }
    text_wrapped_disabled("Oldest unpacked games are removed first; your archives are never touched.");
    if (!CU.skipped.empty()) {
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        std::string title = std::to_string(CU.skipped.size()) + " archives skipped";
        if (ImGui::CollapsingHeader(title.c_str())) {
            for (auto& s : CU.skipped) {
                std::string name = std::filesystem::path(s.path).filename().string();
                ImGui::PushTextWrapPos(0.f);
                ImGui::TextUnformatted(name.c_str());
                ImGui::PopTextWrapPos();
                text_wrapped_disabled(s.reason);
            }
        }
    }
}

void tab_general() {
    begin_panel("set_lib");
    section_title("Library");
    gap();
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
    add_rom_folder(button("Add folder...", ImVec2(0, 0), false, true, ui::Icon::Plus));
    ImGui::SameLine();
    if (button("Find my games...", ImVec2(0, 0), false, !A.scanning, ui::Icon::Search)) find_games_open();
    ImGui::SameLine();
    if (button("Run setup again", ImVec2(0, 0), false, true, ui::Icon::Gamepad)) wizard_open();
    ImGui::SameLine();
    if (button("Rescan", ImVec2(0, 0), false, !A.scanning, ui::Icon::Refresh)) start_scan();
    { ImGui::SameLine(); hidden_games_row(); }
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    settings_label("Shared game packs");
    text_wrapped_disabled("Greyed-out games can be installed from the shared packs - everyone gets the community packs automatically. Have a game nobody else has? "
                          "Put it in your own public Google Drive folder and link the folder that directly contains the console subfolders "
                          "(n64, gc, wiiu, 3ds, nds, gba, gbc, gb, ps1), each game in its own folder. Send the link so it can be added to the shared list. "
                          "Links below apply to this PC only.");
    {
        const std::vector<std::string> ids = hubgp::links_split(A.st.pack_links);
        if (ids.empty()) text_wrapped_disabled("No links yet.");
        std::string to_remove;
        for (size_t i = 0; i < ids.size(); ++i) {
            ImGui::PushID((int)i);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(hubgp::link_label(ids[i]).c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("https://drive.google.com/drive/folders/%s", ids[i].c_str());
            ImGui::SameLine();
            if (button("Remove", ImVec2(0, 0), false, true, ui::Icon::Close)) to_remove = ids[i];
            ImGui::PopID();
        }
        if (!to_remove.empty() && hubgp::links_remove(&A.st.pack_links, to_remove)) { mark_dirty(); set_toast("Link removed.", true, 3.0); }
    }
    static char pl_new[1024] = "";
    ImGui::SetNextItemWidth(-sp(14.f));
    osk::input("##pack_link_new", "Paste a Google Drive folder link", pl_new, sizeof pl_new);
    ImGui::SameLine();
    if (button("Add", ImVec2(0, 0), false, pl_new[0] != 0, ui::Icon::Plus)) {
        int found = 0;
        int added = hubgp::links_add(&A.st.pack_links, pl_new, &found);
        if (found == 0) set_toast("That doesn't look like a Google Drive folder link.", false, 4.0);
        else if (added == 0) set_toast("Already added.", false, 3.0);
        else { mark_dirty(); set_toast(added == 1 ? "Link added." : std::to_string(added) + " links added.", true, 3.0); pl_new[0] = 0; }
    }
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
    locate_smash(button("Browse for BattleShip.exe...", ImVec2(0, 0), false, true, ui::Icon::Folder));
    if (!A.st.smash_exe.empty()) {
        ImGui::SameLine();
        if (button("Use default", ImVec2(0, 0))) { A.st.smash_exe.clear(); mark_dirty(); A.hello_dirty = true; start_scan(); }
    }
    cache_section();
    end_panel();
    gap();
    begin_panel("set_app");
    section_title("Startup, sidebar and sounds");
    gap();
    theme_settings_row();
    gap();
    settings_label("Sidebar");
    int sm = A.st.sidebar_mode;
    if (ImGui::RadioButton("Automatic", sm == 0)) { A.st.sidebar_mode = 0; mark_dirty(); }
    ImGui::SameLine();
    if (ImGui::RadioButton("Always expanded", sm == 1)) { A.st.sidebar_mode = 1; mark_dirty(); }
    ImGui::SameLine();
    if (ImGui::RadioButton("Icons only", sm == 2)) { A.st.sidebar_mode = 2; mark_dirty(); }
    text_wrapped_disabled("Automatic shrinks the sidebar to icons when the window is narrow.");
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    if (ImGui::Checkbox("Show friends panel", &A.st.show_friends)) mark_dirty();
    if (ImGui::Checkbox("Notify me about invites while I'm playing", &A.st.notify_in_game)) mark_dirty();
    text_wrapped_disabled("Shows a Windows notification that never takes focus from your game. Click Join to enter the room when your game ends.");
    {
        ImGui::TextUnformatted("Chat on screen while playing");
        if (ImGui::RadioButton("Off##gchat", A.st.game_chat == 0)) { A.st.game_chat = 0; mark_dirty(); }
        ImGui::SameLine();
        if (ImGui::RadioButton("Room only##gchat", A.st.game_chat == 1)) { A.st.game_chat = 1; mark_dirty(); }
        ImGui::SameLine();
        if (ImGui::RadioButton("Room and friends##gchat", A.st.game_chat == 2)) { A.st.game_chat = 2; mark_dirty(); }
        text_wrapped_disabled("Shows messages as small text in the corner of the game, nothing pops up. Reply from the game menu (View + Y).");
    }
    if (ImGui::Checkbox("Notification sound", &A.st.notif_sound)) mark_dirty();
    if (ImGui::Checkbox("Menu sounds", &A.st.menu_sounds)) mark_dirty();
    if (A.st.menu_sounds) {
        ImGui::SameLine(); ImGui::SetNextItemWidth(sp(28.f));
        if (ImGui::SliderInt("##menu_vol", &A.st.menu_volume, 0, 100, "%d%%")) mark_dirty();
    }
    if (ImGui::Checkbox("Menu music", &A.st.menu_music)) mark_dirty();
    text_wrapped_disabled("Soft sounds and an optional quiet ambient loop in RetroHub. Always silent while a game is running or RetroHub is in the background.");
    tvmode::settings_ui();
    if (ImGui::Checkbox("Show my screenshots when RetroHub is idle", &A.st.attract_mode)) mark_dirty();
    text_wrapped_disabled("After about a minute with no input on Home, the top card slowly pans through your own screenshots and save-state pictures. Any button brings it back. Nothing is downloaded.");
    end_panel();
}

void tab_online() {
    begin_panel("set_online");
    section_title("Online play");
    gap();
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
    gap();
    begin_panel("set_friends");
    section_title("Friends");
    gap();
    if (ImGui::Checkbox("Automatically add people I play with as friends", &A.st.auto_friend)) { mark_dirty(); fr_set_auto_friend(A.st.auto_friend); }
    text_wrapped_disabled("Joining a room, or playing a game together, makes you friends. Both people need this on.");
    if (ImGui::Checkbox("Share my play time with friends", &A.st.share_playtime)) { mark_dirty(); fr_set_share_playtime(A.st.share_playtime); }
    text_wrapped_disabled("Friends see roughly how long you have played each game (whole hours). Turn it off to stop sharing and remove it from the server.");
    if (ImGui::Checkbox("Show my games to friends", &A.st.share_games)) mark_dirty();
    text_wrapped_disabled("Friends see the names of your games greyed out in their library, so they can ask you to host. Only titles are shared, never game files. Turn off to hide your list.");
    if (ImGui::Checkbox("Show friends' games in my library", &A.st.show_friend_games)) { mark_dirty(); A.vis_dirty = true; }
    ImGui::TextDisabled("%s", hubui::fg::lists_held_text(fr_library_lists_held(), fr_friends().size()).c_str());
    std::string mycode = fr_my_code();
    if (!mycode.empty()) {
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        settings_label("Your friend code", "Share it so friends can add you.");
        ImGui::TextUnformatted(mycode.c_str());
        ImGui::SameLine();
        if (button("Copy", ImVec2(0, 0))) { SDL_SetClipboardText(mycode.c_str()); set_toast("Friend code copied.", true, 3.0); }
    }
    end_panel();
    gap();
    begin_panel("set_adv");
    if (ImGui::CollapsingHeader("Advanced")) {
        char sb[256];
        std::snprintf(sb, sizeof sb, "%s", A.st.server.c_str());
        ImGui::TextUnformatted("Lobby server URL");
        ImGui::SetNextItemWidth(-1);
        if (osk::input("##srv", hubnet::default_server(), sb, sizeof sb)) { A.st.server = sb; mark_dirty(); }
        ImGui::TextDisabled("Leave empty for the default server.");
    }
    end_panel();
}

void tab_video() {
    begin_panel("set_play");
    section_title("Per-system defaults");
    gap();
    settings_label("Game Boy / Game Boy Color emulator");
    int core = A.st.gb_core == "gambatte" ? 1 : 0;
    if (ImGui::RadioButton("SameBoy (most accurate)", core == 0)) { A.st.gb_core = "sameboy"; mark_dirty(); }
    ImGui::SameLine();
    if (ImGui::RadioButton("Gambatte (lighter)", core == 1)) { A.st.gb_core = "gambatte"; mark_dirty(); }
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    if (ImGui::Checkbox("Start games fullscreen", &A.st.fullscreen)) mark_dirty();
    text_wrapped_disabled("Default for every system; each system or game can override it under Game defaults / Details.");
    ImGui::SetNextItemWidth(sp(30.f));
    if (ImGui::SliderInt("Game volume", &A.st.volume, 0, 100, "%d%%")) mark_dirty();
    text_wrapped_disabled("Default for every game, set at launch. Each system or game can override it (Game defaults / Details), and the game menu (View + Y) changes it live.");
    if (ImGui::Checkbox("Show controller tips when a game starts", &A.st.start_hint)) mark_dirty();
    text_wrapped_disabled("A small card for about 5 seconds: hold View + Y for the game menu, View + Menu to return to RetroHub.");
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    boot_settings_ui();
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    {
        const EmuLook& el = emu_look();
        const hubemu::Install &az = el.az, &cm = el.cm, &dl = el.dl, &ps = el.ps;
        const hubemu::Bios& bi = el.bi;
        text_wrapped_disabled("Dolphin (GameCube): " + (dl.exe.empty() ? std::string("not installed - downloads the first time you play a GameCube game (about 18 MB)") : dl.exe));
        text_wrapped_disabled("PCSX2 (PlayStation 2): " + (ps.exe.empty() ? std::string("not installed - downloads the first time you play a PS2 game (about 25 MB)") : ps.exe));
        {
            text_wrapped_disabled(bi.file.empty() ? std::string("PS2 BIOS: not found - put your own scph*.bin in a BIOS folder next to your games, or choose the folder below")
                                                   : "PS2 BIOS: " + bi.dir + "\\" + bi.file);
            {
                std::string p = osk::folder("ps2bios_set", button("Choose PS2 BIOS folder...", ImVec2(0, 0), false, true, ui::Icon::Folder));
                if (!p.empty()) { A.st.ps2_bios_dir = p; mark_dirty(); }
            }
            if (!A.st.ps2_bios_dir.empty()) {
                ImGui::SameLine();
                if (button("Use automatic##ps2bios", ImVec2(0, 0))) { A.st.ps2_bios_dir.clear(); mark_dirty(); }
            }
        }
        ps1_settings_ui();
        text_wrapped_disabled("Azahar (3DS): " + (az.exe.empty() ? std::string("not installed - downloads the first time you play a 3DS game (about 42 MB)") : az.exe));
        text_wrapped_disabled("Cemu (Wii U): " + (cm.exe.empty() ? std::string("not found - downloads the first time you play a Wii U game (about 25 MB)") : cm.exe + "  (" + cm.how + ")"));
        {
            std::string p = osk::file("cemu_exe", button("Choose Cemu.exe...", ImVec2(0, 0), false, true, ui::Icon::Folder), {".exe"});
            if (!p.empty()) { A.st.cemu_exe = p; mark_dirty(); }
        }
        if (!A.st.cemu_exe.empty()) {
            ImGui::SameLine();
            if (button("Use automatic", ImVec2(0, 0))) { A.st.cemu_exe.clear(); mark_dirty(); }
        }
        text_wrapped_disabled("A Cemu you already have keeps its own settings and saves; RetroHub only copies the game's save folder into the "
                              "save vault after you play (and restores it into an empty one).");
    }
    end_panel();
    gap();
    gs_draw_defaults_area();
}

}  // namespace

void settings_open(int tab) {
    A.st.settings_tab = hubui::settings_tab_clamp(tab);
    A.page = Page::Settings;
    mark_dirty();
}

void page_settings() {
    page_header("Settings", "Make RetroHub yours.");
    A.st.settings_tab = hubui::settings_tab_clamp(A.st.settings_tab);
    static const char* labels[hubui::ST_Count] = {"General", "Online", "Video & Games", "Account"};
    int nt = ui::segmented("settabs", labels, hubui::ST_Count, A.st.settings_tab);
    if (nt != A.st.settings_tab) { A.st.settings_tab = nt; mark_dirty(); }
    ImGui::SameLine(0, sp(2.f));
    ImGui::TextDisabled("LB / RB switch tabs");
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    switch (A.st.settings_tab) {
        case hubui::ST_General: tab_general(); break;
        case hubui::ST_Online: tab_online(); break;
        case hubui::ST_Video: tab_video(); break;
        default: draw_account_section(); break;
    }
    ImGui::Dummy(ImVec2(0, sp(2.f)));
}
