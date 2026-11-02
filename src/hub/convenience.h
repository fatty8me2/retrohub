#pragma once
/* Plan 4 Wave 2 C (convenience): hooks between the small feature files and the pages that show them.
 * conv_frame() is the one per-frame entry point (called from draw_overlays in page_about.cpp). */
#include "app.h"

void conv_frame();                       // every frame: pad kind + battery, Discord presence, palette, type-to-search bookkeeping

/* glyph_ui.cpp */
void padinfo_tick();                     // which pad is in use -> ui::pad_kind (HUB_SHOT_FAKE_PADTYPE=xbox|ps|switch overrides, HUB_SHOT_FAKE_BATTERY=empty|low|medium|full)
void battery_draw();                     // small badge in the hub window corner; never while a game is active or the hub is not in front
const char* pad_kind_label();            // "Xbox" / "PlayStation" / "Switch"

/* discord_ipc.cpp / discord_ui.cpp */
void discord_ipc_set(const std::string& activity_json);   // "" = clear presence. Fails silently when Discord is not running
std::string discord_ipc_status();                         // "" | "Connected to Discord" | "Discord is not running"
void discord_tick();
void discord_settings_ui();              // Settings > Online

/* nettest_ui.cpp */
void nettest_settings_ui();              // Settings > Online: connection test
void netprobe_start();                   // run n64run --net-probe / retrorun --net-probe (wizard step; RetroHub is in front)
bool netprobe_running();
void netprobe_tick();                    // marks A.st.net_probe_done when both finished

/* diag_ui.cpp */
void diag_settings_ui();                 // Settings > Help: Copy diagnostics

/* palette_ui.cpp */
void palette_open();
bool palette_active();
void palette_draw();
bool type_to_search_pending();           // page_library: a typed character should start a search this frame
void palette_set_hotkey_hint(bool on);

/* help_ui.cpp */
void help_tab_ui();                      // Settings > Help: FAQ
void finish_setup_card();                // Home: "Finish setup" while steps are open
