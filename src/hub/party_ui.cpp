#include "app.h"
#include "party_logic.h"

/* Party mode UI and flow (Plan 4 P1-P3). STUB from the Wave 1 seams commit: the party lane (L1) owns this file and replaces it.
 * Contract: party_logic.h (state, messages, reducer) and results.h (finished matches). Hooks are declared in app.h. */

void party_on_msg(int from_slot, const hubui::Json& j) { (void)from_slot; (void)j; }
void party_tick() {}
void party_on_session_end() {}
void party_room_panel(float width) { (void)width; }
void party_draw() {}
void party_shot_init() {}
