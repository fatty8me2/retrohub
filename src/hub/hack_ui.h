#pragma once
/* ROM hacks and translations: UI entry points beyond the four hooks in app.h (hack_scan_append / hack_ensure_rom / hack_draw / hack_shot_init). */
#include "app.h"

/* Patch files (.ips .ups .bps .xdelta .vcdiff .delta) dropped on the window or chosen by hand go to hack_add_patch(). */
bool hack_is_patch_file(const std::string& path);
void hack_add_patch(const std::string& path, const hub::GameEntry* base_hint = nullptr);
/* "Add a hack or translation" from a game's page: asks for the patch file (pad file browser), then walks through the rest. e may be a hack (its base is used). */
void hack_add_open(const hub::GameEntry& e);
bool hack_can_add(const hub::GameEntry& e);   // false for consoles whose images cannot be patched here, and for the native port
/* Game page > About: the "Versions" row (Original + each hack) and, on a hack, Remove hack. */
void hack_details_section(const hub::GameEntry& e);
/* "HACK" / "TRANSLATION" for a hack entry, "" for everything else. Tiles show it as their corner tag. */
std::string hack_ribbon(const hub::GameEntry& e);
