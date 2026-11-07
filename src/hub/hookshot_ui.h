#pragma once
/* The Hookshot card on Ocarina of Time (Switch 2)'s game page: "Build the native version" and its progress (hookshot_build.h).
 * Only drawn when hookshot::supports(e). Screenshot aid: HUB_SHOT_HOOKSHOT=idle|problems|building|paused|done|failed fakes the state
 * (with HUB_SHOT_FAKE_LIB=3 HUB_SHOT_DETAILS=switch:0400486024dec000). */
#include <string>

namespace hub { struct GameEntry; }

/* Game page > About: the card. Nothing for any other game. */
void hookshot_details_section(const hub::GameEntry& e);
/* Every frame (draw_overlays): tells the build when a game is running, puts an interrupted swap right, and shows the toast when a
 * build finished or stopped. */
void hookshot_ui_tick();
