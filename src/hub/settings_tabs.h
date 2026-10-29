#pragma once
/* Settings page tabs: pure logic (no ImGui) so the console test can check the layout.
 * kControls lists EVERY setting control the Settings page offers and the tab it lives on. When a control is added or moved,
 * update this table; test_logic.cpp compares it with the pre-split checklist so nothing silently disappears. */
#include <cstring>

namespace hubui {

/* Four tabs. "Saves & Cloud" and "Controllers" were removed: their controls live on the sidebar Saves / Controllers pages (see kPageSaves / kPageControllers). */
enum SettingsTab { ST_General = 0, ST_Online, ST_Video, ST_Account, ST_Count };
/* Pseudo-tabs for controls that now live on a sidebar page instead of a Settings tab. */
enum { kPageSaves = 100, kPageControllers = 101 };

inline const char* settings_tab_name(int t) {
    static const char* n[] = {"General", "Online", "Video & Games", "Account"};
    return t >= 0 && t < ST_Count ? n[t] : n[0];
}
inline int settings_tab_clamp(int t) { return t < 0 || t >= ST_Count ? ST_General : t; }

/* settings.json written before the tab removal stored the old 6-tab index (General, Online, Saves & Cloud, Controllers, Video & Games, Account).
 * Saves & Cloud and Controllers have no tab any more, so they open General (the Saves / Controllers pages are one click away in the sidebar). */
inline int settings_tab_from_legacy(int t) {
    switch (t) { case 0: return ST_General; case 1: return ST_Online; case 4: return ST_Video; case 5: return ST_Account; default: return ST_General; }
}

/* LB / RB: moves one tab; past either end it asks the caller to move to the neighbouring page instead (returns the page step -1/0/+1). */
inline int settings_tab_step(int cur, int dir, int* next) {
    int n = cur + dir;
    if (n < 0) { *next = cur; return -1; }
    if (n >= ST_Count) { *next = cur; return 1; }
    *next = n;
    return 0;
}

struct SettingsCtl { const char* id; int tab; };
inline const SettingsCtl* settings_controls(int* n) {
    static const SettingsCtl c[] = {
        {"rom_folders_list", ST_General}, {"pack_links", ST_General}, {"rom_folder_remove", ST_General}, {"add_folder", ST_General}, {"find_games", ST_General},
        {"run_setup_again", ST_General}, {"rescan", ST_General}, {"download_art", ST_General}, {"smash_exe_browse", ST_General},
        {"smash_exe_default", ST_General}, {"sidebar_mode", ST_General}, {"theme_picker", ST_General}, {"show_friends", ST_General}, {"notify_in_game", ST_General}, {"game_chat", ST_General},
        {"tv_mode", ST_General}, {"tv_start", ST_General}, {"tv_windows", ST_General},
        {"notif_sound", ST_General}, {"menu_sounds", ST_General}, {"menu_music", ST_General}, {"attract_mode", ST_General}, {"rom_cache_clear", ST_General}, {"rom_cache_limit", ST_General}, {"skipped_archives", ST_General},
        {"delay_auto", ST_Online}, {"input_delay", ST_Online}, {"auto_friend", ST_Online}, {"share_playtime", ST_Online}, {"share_games", ST_Online}, {"show_friend_games", ST_Online}, {"friend_code_copy", ST_Online}, {"server_url", ST_Online},
        {"cloud_summary", kPageSaves}, {"saves_page_link", kPageSaves}, {"pause_cloud", kPageSaves}, {"backup_shots", kPageSaves},
        {"cloud_states", kPageSaves}, {"sync_folder", kPageSaves}, {"sync_folder_stop", kPageSaves}, {"sync_auto", kPageSaves}, {"sync_copy_now", kPageSaves},
        {"controllers_page_link", kPageControllers}, {"cemu_manage_controls", kPageControllers}, {"cemu_restore_controls", kPageControllers},
        {"gb_core", ST_Video}, {"fullscreen", ST_Video}, {"start_hint", ST_Video},
        {"boot_anim", ST_Video}, {"boot_sounds", ST_Video}, {"boot_volume", ST_Video}, {"boot_short", ST_Video}, {"boot_skip_online", ST_Video}, {"dolphin_status", ST_Video}, {"pcsx2_status", ST_Video},
        {"ps2_bios_dir", ST_Video}, {"ps2_bios_auto", ST_Video}, {"ps1_bios", ST_Video}, {"ps1_openbios", ST_Video},
        {"azahar_status", ST_Video}, {"cemu_exe", ST_Video}, {"cemu_exe_auto", ST_Video}, {"game_defaults", ST_Video},
        {"username", ST_Account}, {"change_username", ST_Account}, {"friend_code", ST_Account}, {"account_code", ST_Account},
        {"redeem_code", ST_Account}, {"export_account", ST_Account}, {"import_account", ST_Account},
    };
    *n = (int)(sizeof c / sizeof c[0]);
    return c;
}
inline int settings_control_tab(const char* id) {
    int n; const SettingsCtl* c = settings_controls(&n);
    for (int i = 0; i < n; ++i) if (!std::strcmp(c[i].id, id)) return c[i].tab;
    return -1;
}

}  // namespace hubui
