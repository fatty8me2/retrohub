#pragma once
/* PlayStation 2 via PCSX2 (RetroHub's own portable download, Kind::Pcsx2 in emulators.h): BIOS discovery, PCSX2.ini
 * pre-writing (setup wizard off, BIOS, per-game memory card, renderer, pad bindings from the RetroHub "ps2" profile),
 * launch arguments. UI-free; see docs/PS2.md. */

#include <string>
#include <vector>

#include "controls.h"
#include "emulators.h"
#include "game_settings.h"

namespace hubemu {

/* ---- BIOS ---- */
struct Bios { std::string dir, file; };   // file = bare name, dir = folder holding it ("" = none found)
/* True for names like scph39001.bin / SCPH-70012.bin / ps2-0230a-20080220.bin, size 1..8 MB (a PS1 BIOS is 512 KB). */
bool pcsx2_bios_name_ok(const std::string& file_name, uint64_t size);
/* Looks in each folder, its BIOS / bios subfolder and (for a folder named Games*) a sibling BIOS folder. First match, scph first. */
Bios pcsx2_find_bios(const std::vector<std::string>& dirs);
/* Where to look when nothing is chosen: Desktop\Everything\BIOS, the library folders, their parents. */
std::vector<std::string> pcsx2_bios_search_dirs(const std::string& chosen_dir, const std::vector<std::string>& rom_folders);
const char* pcsx2_bios_missing_text();

/* ---- PCSX2.ini (pure text transforms; hubui::ini_set underneath) ---- */
struct Ps2Pads {
    int sdl[4] = {-1, -1, -1, -1};   // SDL index of player n's pad (-1 = none)
    int count = 0;                    // players with a pad (0 = keyboard on Pad1)
    bool multitap = false;            // 3+ pads: multitap on memory-card port 1 (Pad1, Pad3, Pad4, Pad5)
};
/* RetroHub port order -> players. by_sdl = every pad in SDL order; the index in it is PCSX2's SDL-<n>. */
Ps2Pads pcsx2_pads_with(const std::vector<PadDesc>& by_port, const std::vector<PadDesc>& by_sdl);
/* One profile term -> PCSX2 binding text, e.g. "SDL-0/A", "SDL-1/-LeftY", "SDL-0/+RightTrigger". */
std::string pcsx2_binding(const hubctl::Term& t, int sdl_index);
std::string pcsx2_ini_defaults(const std::string& ini);                                                // once
std::string pcsx2_ini_pads(const std::string& ini, const Ps2Pads& pads, const hubctl::Profile& profile);   // on profile / pad change
/* Buzz! quiz games (catalog ids "buzz_*"): USB1 = BuzzDevice, bindings BuzzDevice_<Colour><n> = SDL pad buttons for player n
 * (Red = A, Blue = B, Orange = X, Green = Y, Yellow = Start). Other games get USB1 = None. USB2 = None always. */
bool pcsx2_is_buzz_game(const std::string& catalog_id);
std::string pcsx2_ini_buzz(const std::string& ini, const Ps2Pads& pads, bool buzz);
/* Every launch: BIOS, this game's memory card, renderer / upscale / widescreen. */
std::string pcsx2_ini_launch(const std::string& ini, const Bios& bios, const std::string& card_rel, const hub::gs::Values& v);
/* Memory card path relative to <install>\memcards: "RetroHub/<game key>/Mcd001.ps2". */
std::string pcsx2_card_rel(const std::string& game_key);

/* Writes <install>\inis\PCSX2.ini (portable.ini beside the exe is written by the installer). Returns "" or an error. */
std::string prepare_pcsx2(const std::string& install_dir, const Ps2Pads& pads, const hubctl::Profile& profile, const Bios& bios,
                          const std::string& game_key, const hub::gs::Values& v, bool buzz = false);
/* -batch -nogui [-fullscreen] -- "<game>" (the game path last). */
std::string pcsx2_args(const std::string& game_path, bool fullscreen);

}  // namespace hubemu
