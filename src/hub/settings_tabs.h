#pragma once
/* Settings page tabs: pure logic (no ImGui) so the console test can check the layout.
 * kControls lists EVERY setting control the Settings page offers and the tab it lives on. When a control is added or moved,
 * update this table; test_logic.cpp compares it with the pre-split checklist so nothing silently disappears. */
#include <cstring>

namespace hubui {

enum SettingsTab { ST_General = 0, ST_Online, ST_Saves, ST_Controllers, ST_Video, ST_Account, ST_Count };

inline const char* settings_tab_name(int t) {
    static const char* n[] = {"General", "Online", "Saves & Cloud", "Controllers", "Video & Games", "Account"};
    return t >= 0 && t < ST_Count ? n[t] : n[0];
}
inline int settings_tab_clamp(int t) { return t < 0 || t >= ST_Count ? ST_General : t; }

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
        {"rom_folders_list", ST_General}, {"rom_folder_remove", ST_General}, {"add_folder", ST_General}, {"find_games", ST_General},
        {"run_setup_again", ST_General}, {"rescan", ST_General}, {"download_art", ST_General}, {"smash_exe_browse", ST_General},
        {"smash_exe_default", ST_General}, {"sidebar_mode", ST_General}, {"show_friends", ST_General}, {"notify_in_game", ST_General},
        {"notif_sound", ST_General}, {"rom_cache_clear", ST_General}, {"rom_cache_limit", ST_General}, {"skipped_archives", ST_General},
        {"delay_auto", ST_Online}, {"input_delay", ST_Online}, {"auto_friend", ST_Online}, {"share_playtime", ST_Online}, {"share_games", ST_Online}, {"show_friend_games", ST_Online}, {"friend_code_copy", ST_Online}, {"server_url", ST_Online},
        {"cloud_summary", ST_Saves}, {"saves_page_link", ST_Saves}, {"pause_cloud", ST_Saves}, {"backup_shots", ST_Saves},
        {"cloud_states", ST_Saves}, {"sync_folder", ST_Saves}, {"sync_folder_stop", ST_Saves}, {"sync_auto", ST_Saves}, {"sync_copy_now", ST_Saves},
        {"controllers_page_link", ST_Controllers}, {"cemu_manage_controls", ST_Controllers}, {"cemu_restore_controls", ST_Controllers},
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
