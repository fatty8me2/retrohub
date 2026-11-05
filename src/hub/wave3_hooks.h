#pragma once
/* Plan 4 Wave 3 seams (v1.33): one hook per lane so the shared files (launch.cpp, page_library.cpp, page_saves.cpp, logic.*)
 * are edited once, here, and every lane works in its own .cpp. Each stub is a no-op until its lane fills it in.
 *
 *   X1 Transfer Pak   transferpak.cpp   N64 Transfer Pak: a GB/GBC cartridge + its vault save plugged into the controller
 *   X2 GC <-> GBA     gba_link.cpp      Dolphin's integrated GBA per port (BIOS, ROM, vault save)
 *   X3 Pal Park       palpark.cpp       a GBA game in the DS Slot-2
 *   R1 Resume more    resume_more.cpp   resume points beyond GB/GBA/NES/SNES; "Continue on this PC"
 *   L1/L2 Smarts      progress_ui.cpp   completion status + notes (and smart shelves in shelves/logic)
 *   L3 Verify         verify_ui.cpp     known-good dump check + Library health card
 *   L4 Bring saves    save_import_ui.cpp import saves from other emulators
 *
 * Launch hooks return extra command-line flags (each starting with a space) or "" for none. They are offline-only: the
 * launch code calls them for offline play only; a hook must still never touch anything an online run reads. */
#include <string>

namespace hub { struct GameEntry; }

std::string transferpak_launch_args(const hub::GameEntry& e);                            // n64run flags (offline N64)
void transferpak_details_section(const hub::GameEntry& e);                               // details modal, Info tab
void transferpak_run_finished();                                                         // the game process ended: queue the Game Boy game's save push

void gba_link_apply_dolphin(const hub::GameEntry& e, const std::string& dolphin_dir);   // before an offline GameCube launch
void gba_link_details_section(const hub::GameEntry& e);
void gba_link_after_exit();                                                              // launch_external's after_exit: GBA saves back to the vault

std::string palpark_launch_args(const hub::GameEntry& e);                                // retrorun flags (offline DS)
void palpark_details_section(const hub::GameEntry& e);

void resume_details_section(const hub::GameEntry& e);                                    // details modal, Saves tab

void progress_details_section(const hub::GameEntry& e);                                  // details modal, Info tab

void verify_details_section(const hub::GameEntry& e);                                    // details modal, Info tab
void verify_library_card();                                                              // All Games page, above the grid

void save_import_section();                                                              // Saves page
