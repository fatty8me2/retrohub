#pragma once
/* challenge_ui.h - friend challenges in the hub (Plan 4 p4-social). Owned by the CHALLENGE fork; friends_ui.cpp only calls the two
 * functions below. Pure rules/cards live in challenge_logic.h; file transfer in attach_xfer.h. */
#include "net/friends.h"

namespace hubui {

/* Called by friends_ui for every message that arrives or is loaded into a conversation with a non-empty msg.card: remembers Result
 * cards so a challenge card's leaderboard fills in. Cheap and idempotent per message id. */
void challenge_on_message(const hubnet::Message& m);

/* Draws a challenge/result card inline in the chat (title, goal, small leaderboard, Accept / Try again buttons). `width` = available
 * width. Returns false when m.card is not a challenge/result card (the caller then draws a normal text line). */
bool challenge_draw_card(const hubnet::Message& m, float width);

} // namespace hubui
