#pragma once
/* Home "Continue" hero + "Recently played" row (continue_ui.cpp; the pure rules are continue_logic.h). page_library.cpp only calls these. */
#include <vector>
#include "app.h"

/* True when `e` should get the Continue hero: played within the last 30 days (never a friend's ghost game). */
bool continue_hero_ok(const hub::GameEntry& e);
/* Same hero slot as draw_hero(): backdrop picture (newest state thumb > screenshot > cover), title, system badge, "Last played ... " and the buttons. */
void draw_continue_hero(const hub::GameEntry& e, float h);
/* The hero height the layout needs (eyebrow + title + last-played line + buttons never overlap): max(h, that). */
float continue_hero_min_h(float h);
/* The row under the hero: indices into A.lib (== the shelf's ShelfGame indices). False (row empty) when nothing was played recently. */
bool continue_row_items(std::vector<int>* out);
/* Row tile result: a second click / Enter / A continues the game straight away (plain selection first). Returns true when it launched or handled it. */
bool continue_row_result(const hub::GameEntry& e, const TileSpec& spec, const TileResult& r);
/* One launch path for the hero and the row: resume from the auto state, or a plain Play; fresh = skip the auto resume. */
void continue_launch(const hub::GameEntry& e, bool resume, bool fresh = false);
/* Free the picture textures (theme/device reset). */
void continue_textures_clear();
/* HUB_SHOT_CONTINUE=1|playing (headless): fake play history for the row / a "Playing now" hero. Call after the fake library exists. */
void continue_shot_seed();
