#pragma once
/* Extras for boot_art.cpp beyond the boot_anim.h contract (no textures, no SDL: pure ImDrawList). */
#include "boot_anim.h"

namespace bootanim {

/* Hook for later: cover textures the shelf may show on some of its boxes (copied; pass n = 0 to clear). */
void set_shelf_covers(const ImTextureID* tex, int n);

/* Drawn inside a console between its well/tray and its lid (GC/PS1/PS2) so a disc can sit in the well with the lid closing over it.
 * Pass an empty function to clear. Set it only around one draw_console call. */
void set_console_inner(std::function<void(ImDrawList*)> fn);

/* Short lowercase name for a system's art ("n64", "gb", "gbc", "gba", "ds", "3ds", "gc", "ps1", "ps2", "wiiu", "nes", "snes"); never null. */
const char* art_name(hub::System);
/* True when the system has dedicated console + media art (Unknown falls back to N64 art and reports false). */
bool art_dedicated(hub::System);
/* Parse an art_name (case-insensitive); returns System::Unknown when unrecognised. */
hub::System art_system_from_name(const char*);

/* Gallery (HUB_SHOT_BOOTART): "all" = every console in a grid, "shelf" = shelf only, or a single art_name = that console at
 * several lid/power states plus its media. `cover` may be null. */
void draw_gallery(ImDrawList*, ImVec2 min, ImVec2 max, const char* which, ImTextureID cover);

} // namespace bootanim
