#include "app.h"
#include "pcsx2.h"
namespace hs = hub::saves;

/* ---------- Settings ---------- */

void settings_label(const char* title, const char* hint) {
    ImGui::PushFont(ui::F.bold);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (hint) text_wrapped_disabled(hint);
}

namespace {

/* Emulator lookups touch the disk: refresh every 2 s, not every frame (shared by the Video & Games and Controllers tabs). */
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
    if (ImGui::Checkbox("Notification sound", &A.st.notif_sound)) mark_dirty();
    tvmode::settings_ui();
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

void tab_saves() {
    begin_panel("set_cloud");
    section_title("Saves and cloud");
    gap();
    {
        const bool on = A.st.cloud_enabled;
        ImGui::PushFont(ui::F.h2);
        ImGui::TextUnformatted(on ? "Cloud saves are on" : "Cloud saves are paused");
        ImGui::PopFont();
        text_wrapped_disabled("Space used, per-game history and Sync now live on the Saves page.");
        if (button("Open the Saves page", ImVec2(0, 0), false, true, ui::Icon::Refresh)) A.page = Page::Saves;
        ImGui::Dummy(ImVec2(0, sp(1.f)));
        bool paused = !on;
        if (ImGui::Checkbox("Pause cloud sync", &paused)) { A.st.cloud_enabled = !paused; hs::cloud_set_paused(paused); mark_dirty(); }
        text_wrapped_disabled("Saves stay on this PC only while paused.");
        if (ImGui::Checkbox("Back up screenshots", &A.st.backup_shots)) { hs::set_backup_screenshots(A.st.backup_shots); mark_dirty(); }
        text_wrapped_disabled("They share the space used by save states.");
        bool st = A.st.cloud_states;
        if (ImGui::Checkbox("Back up save states to the cloud", &st)) { A.st.cloud_states = st; hs::cloud_set_sync_states(st); mark_dirty(); }
        text_wrapped_disabled("Resume points follow you between PCs. Uses the shared 50 MB space; turn off to keep states on this PC.");
    }
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    if (ImGui::CollapsingHeader("Advanced: also keep a copy in a folder")) {
        text_wrapped_disabled("You don't need this: cloud saves already follow your account to every PC. Only use it if you also want a plain copy in a folder of your own (a USB stick, a NAS...).");
        text_wrapped_disabled(A.st.sync_folder.empty() ? std::string("Sync folder: (not set)") : "Sync folder: " + A.st.sync_folder);
        std::string p = osk::folder("set_sync", button("Browse...", ImVec2(0, 0), A.st.sync_folder.empty()));
        if (!p.empty()) { A.st.sync_folder = p; mark_dirty(); }
        if (!A.st.sync_folder.empty()) {
            ImGui::SameLine();
            if (button("Stop syncing", ImVec2(0, 0))) { A.st.sync_folder.clear(); mark_dirty(); A.sync_result.clear(); }
        }
        if (ImGui::Checkbox("Sync automatically after playing", &A.st.sync_auto)) mark_dirty();
        if (button("Copy to folder now", ImVec2(0, 0), false, !A.st.sync_folder.empty() && !A.job.running && !A.proc)) start_sync(true);
    }
    end_panel();
}

void tab_controllers() {
    begin_panel("set_ctl");
    section_title("Controllers");
    gap();
    text_wrapped_disabled("Pick a pad per player, remap buttons and set per-system layouts on the Controllers page.");
    if (button("Open the Controllers page", ImVec2(0, 0), false, true, ui::Icon::Gamepad)) A.page = Page::Controllers;
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    {
        const hubemu::Install& cm = emu_look().cm;
        settings_label("Wii U (Cemu) controls");
        if (cm.exe.empty() || cm.managed) text_wrapped_disabled("These options apply to a Cemu you already have installed; RetroHub's own Cemu always uses the Controllers page.");
        if (!cm.exe.empty() && !cm.managed) {
            if (ImGui::Checkbox("Let RetroHub manage Wii U controls", &A.st.cemu_manage_controls)) mark_dirty();
            text_wrapped_disabled("On: before each game RetroHub writes controller profiles from its Controllers page into your Cemu (Xbox and other "
                                  "SDL pads). Your original controllerProfiles folder is first copied once to controllerProfiles.retrohub-backup. "
                                  "Off: your Cemu controls are never touched.");
            if (button("Restore my Cemu controls", ImVec2(0, 0))) {
                const char* ad = std::getenv("APPDATA");
                bool ok = hubemu::cemu_restore_controls(hubemu::cemu_user_data_dir(cm.dir, ad ? ad : ""));
                set_toast(ok ? "Your Cemu controls were restored from the backup." : "No RetroHub backup of your Cemu controls exists yet.", ok, 4.0);
            }
        }
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
    static const char* labels[hubui::ST_Count] = {"General", "Online", "Saves & Cloud", "Controllers", "Video & Games", "Account"};
    int nt = ui::segmented("settabs", labels, hubui::ST_Count, A.st.settings_tab);
    if (nt != A.st.settings_tab) { A.st.settings_tab = nt; mark_dirty(); }
    ImGui::SameLine(0, sp(2.f));
    ImGui::TextDisabled("LB / RB switch tabs");
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    switch (A.st.settings_tab) {
        case hubui::ST_General: tab_general(); break;
        case hubui::ST_Online: tab_online(); break;
        case hubui::ST_Saves: tab_saves(); break;
        case hubui::ST_Controllers: tab_controllers(); break;
        case hubui::ST_Video: tab_video(); break;
        default: draw_account_section(); break;
    }
    ImGui::Dummy(ImVec2(0, sp(2.f)));
}
