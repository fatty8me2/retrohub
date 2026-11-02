#include "app.h"
#include "qm_card.h"
#include "qm_logic.h"
#include "qm_party.h"

/* Quick-menu party pieces (Plan 4 P5): an emote from another room member ({kind:"emote", e:<code>}, routed by lobby.cpp) is shown as
 * "<name>: GG" on the running game's OSD. The code must be one of hubqm::emotes() (anything else is dropped), each sender is limited
 * to one emote per second, and "Chat on screen: Off" (Settings / the menu's Chat page) silences them like chat. Nothing is shown when no
 * built-in runner is running. */

void qm_emote_incoming(int from_slot, const std::string& code) {
    const hubqm::Emote* em = hubqm::find_emote(code);
    if (!em) return;
    if (from_slot == room_my_slot()) return;
    if (A.st.game_chat == hubqm::kChatOff) return;
    static hubqm::EmoteInLimiter lim;
    if (!lim.allow(from_slot, (long long)GetTickCount64())) return;
    const std::string t = hubqm::chat_osd_text(member_name(from_slot), em->label);
    if (!t.empty()) qm_send_osd(t);
}
