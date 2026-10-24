#pragma once
/* On-screen keyboard and pad-navigable folder browser (widget; the pure logic is in osk_logic.h). */

#include "imgui.h"
#include "osk_logic.h"

#include <string>

namespace osk {

/* Drop-in for ImGui::InputText / InputTextWithHint (hint may be null). When the field is activated with a gamepad
 * (or before any physical keyboard input was seen) the on-screen keyboard opens instead. Same return value as ImGui. */
bool input(const char* id, const char* hint, char* buf, size_t size, ImGuiInputTextFlags flags = 0, Kind kind = Kind::Text);

/* Folder choice. Call every frame with whether the "Browse" button was clicked. Returns the chosen folder (""
 * otherwise): at once through the Windows dialog, or on a later frame after the pad folder browser closes. */
std::string folder(const char* tag, bool clicked);

void note_keyboard();                  // a physical keyboard key / text was seen
bool keyboard_seen();
void pre_frame(ImGuiIO& io);           // before NewFrame: suspends ImGui nav while a keyboard / browser is open
void draw_root();                      // screenshot driver only (root-level popup)
void shot_init();                      // HUB_SHOT_OSK=text|code|room|symbols[:initial]  HUB_SHOT_OSK=folder[:path]  HUB_SHOT_OSK_CMDS=right,a,...
bool is_open();

} // namespace osk
