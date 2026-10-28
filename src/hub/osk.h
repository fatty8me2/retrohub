#pragma once
/* On-screen keyboard and pad-navigable folder browser (widget; the pure logic is in osk_logic.h). */

#include "imgui.h"
#include "osk_logic.h"

#include <string>
#include <vector>

namespace osk {

/* Drop-in for ImGui::InputText / InputTextWithHint (hint may be null). When the field is activated with a gamepad
 * (or before any physical keyboard input was seen) the on-screen keyboard opens instead. Same return value as ImGui. */
bool input(const char* id, const char* hint, char* buf, size_t size, ImGuiInputTextFlags flags = 0, Kind kind = Kind::Text);

/* Folder choice. Call every frame with whether the "Browse" button was clicked. Returns the chosen folder (""
 * otherwise): at once through the Windows dialog, or on a later frame after the pad folder browser closes. */
std::string folder(const char* tag, bool clicked);

/* Single-file choice (account file, Cemu.exe, zip ...). filters = extensions with the dot, e.g. {".json"}. Call every frame with the
 * button result. Pad (A.pad_mode): file browser over drives / shortcuts / folders / matching files; else the Windows dialog. */
std::string file(const char* tag, bool clicked, const std::vector<std::string>& filters);

bool hub_pad_blocked();                // like hub_input_blocked but false while the View+Y quick menu is open (pad only; chat/OSK use hub_input_blocked)
bool hub_input_blocked();             // a game runs or the hub window is unfocused: UI ignores keys / text / pad (headless never blocks)
void note_keyboard();                  // a physical keyboard key / text was seen
bool keyboard_seen();
void pre_frame(ImGuiIO& io);           // before NewFrame: suspends ImGui nav while a keyboard / browser is open
void draw_root();                      // screenshot driver only (root-level popup)
void shot_init();                      // HUB_SHOT_OSK=text|code|room|symbols[:initial]  HUB_SHOT_OSK=folder[:path]  HUB_SHOT_OSK=file[:path]  HUB_SHOT_OSK_CMDS=right,a,...
bool is_open();
bool inject(const char* tok);          // headless QA: feed a pad token (up,down,left,right,a,b,x,y,start,back/view,lb,rb) to the open keyboard; false = not open
std::string shot_text();               // current text of the open keyboard ("" when closed)

} // namespace osk
