#pragma once
/* The native-version card on Ocarina of Time (Switch 2)'s game page: "Which version plays", then a section each for Hookshot and Longshot
 * ("Build it" and its progress, hookshot_build.h). Only drawn when hookshot::supports(e). Screenshot aid (headless only), HUB_SHOT_HOOKSHOT=
 *   idle|problems (neither built; problems = the PC fails the checks) | building|paused (Hookshot building) | done (Hookshot built, Play uses Lichii) |
 *   done_on (Hookshot built and chosen) | done_fritchii (Hookshot built, Fritchii plays) | failed (Hookshot build failed) |
 *   ls_idle (neither built) | ls_building (Longshot building) | ls_done (Longshot built, Play uses Lichii) | ls_done_on (Longshot built and chosen) |
 *   both_done (both built, Fritchii plays) | ls_missing (Longshot chosen, not built)
 * fakes the state (unset, empty or 0 = off) (with HUB_SHOT_FAKE_LIB=3 HUB_SHOT_DETAILS=switch:0400486024dec000). */
#include <string>

namespace hub { struct GameEntry; }

/* Game page > About: the card. Nothing for any other game. */
void hookshot_details_section(const hub::GameEntry& e);
/* Every frame (draw_overlays): tells the build when a game is running, puts an interrupted swap right, and shows the toast when a
 * build finished or stopped. */
void hookshot_ui_tick();
