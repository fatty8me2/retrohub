#include "app.h"

/* Brackets and the friends ladder (Plan 4 P7-P8). STUB from the Wave 1 seams commit: the results lane (L5) owns this file and
 * replaces it (pure logic in its own bracket_logic.h / elo, tests in test_bracket.cpp). Hooks are declared in app.h. */

void bracket_open() {}
void bracket_on_msg(int from_slot, const hubui::Json& j) { (void)from_slot; (void)j; }
void bracket_on_result(const hubui::results::RoundResult& r) { (void)r; }
void bracket_tick() {}
void bracket_draw() {}
void bracket_shot_init() {}
