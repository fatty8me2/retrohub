#pragma once
/* GameCube cheats for RetroHub's managed Dolphin. Dolphin reads codes from User\GameSettings\<GAMEID>.ini:
 *
 *   [Gecko]  / [Gecko_Enabled]            $Name, then "AAAAAAAA VVVVVVVV" lines  /  $Name of each enabled code
 *   [ActionReplay] / [ActionReplay_Enabled]  same, decrypted 8+8 or encrypted XXXX-XXXX-XXXXX lines
 *
 * and only applies them when Dolphin.ini [Core] has EnableCheats = True. RetroHub's own entries carry the name prefix
 * "RetroHub: " so they can be told from the player's (and Dolphin's bundled) codes: patch_game_ini() removes every
 * RetroHub entry and then adds the currently enabled ones, leaving all other text byte for byte. Dolphin cannot apply
 * changes mid-game, so the file is written right before the emulator starts (and cleaned the same way when nothing is on).
 *
 * Pure text transforms (unit tested in hubdolphincheats_test) plus one small file writer. */
#include <string>

#include "cheats.h"

namespace dolphincheats {

extern const char* const kOwnedPrefix;   // "RetroHub: "

/* `enabled`: only the cheats to switch on (their `enabled` flag is ignored). Cheats whose codes do not validate for GC
 * are skipped. *added (optional) = how many were written. */
std::string patch_game_ini(const std::string& ini, const cheats::CheatSet& enabled, int* added = nullptr);
/* How many RetroHub-owned entries the text holds (Gecko + ActionReplay sections). */
int count_owned(const std::string& ini);
/* Dolphin.ini text with [Core] EnableCheats = True. */
std::string enable_cheats(const std::string& dolphin_ini);

/* "GALE01" -> "GALE01.ini"; "" when the id is not 4..6 letters/digits. */
std::string game_ini_name(const std::string& game_id);

/* Writes User\GameSettings\<id>.ini (only when it changes or RetroHub owned something there) and, when something is on,
 * EnableCheats in User\Config\Dolphin.ini. `user_dir` = <dolphin install>\User. Returns the number of codes written,
 * -1 on an I/O error (*err set). Nothing is created when there is nothing to write and nothing to clean. */
int apply_for_game(const std::string& user_dir, const std::string& game_id, const cheats::CheatSet& enabled, std::string* err);

}  // namespace dolphincheats
