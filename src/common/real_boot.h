#pragma once
/* "Real console startup": boot a game through the player's OWN console boot files (IPL / BIOS / boot ROM / firmware) so the genuine
 * startup sequence plays. RetroHub ships none of those files and none of the consoles' logos or sounds; this module only finds the files
 * the player already has, decides per system whether the real startup can be used, and says what the launch must stage and set.
 * Pure (std::filesystem only); unit-tested in test_real_boot.cpp. */

#include <string>
#include <utility>
#include <vector>
#include "library.h"

namespace hub::realboot {

/* Systems with a startup RetroHub can run for real: GC (Dolphin IPL), PS1 (Beetle PSX BIOS, skip off), PS2 (PCSX2 fast boot off),
 * GBA (mGBA + gba_bios.bin), GB / GBC (SameBoy / Gambatte boot ROM), NDS (melonDS firmware + BIOS). */
bool supported(System s);
const char* console_label(System s);      // "GameCube"
const char* file_hint(System s);          // what to put where, shown in Settings when the file is missing

/* Files the launch must copy into the runner's system folder: (source path, file name in the system folder). */
struct Stage {
    bool ok = false;                                  // every needed file was found
    std::vector<std::pair<std::string, std::string>> copies;
    std::string region;                               // GC: "USA" / "EUR" / "JAP"
    std::string detail;                               // what was found (or what is missing), for Settings
};

/* GC disc ID (6 chars, "GALE01") -> Dolphin's IPL region folder: E -> USA, P -> EUR, J / K -> JAP (anything else: USA). */
std::string gc_region_for_serial(const std::string& serial);

/* Looks for the player's files in `dirs` (and vendor subfolders up to 4 deep when the folder name says "bios").
 * GC: an IPL.bin / "IPL (USA).bin" / <USA|EUR|JAP>\IPL.bin of 2 MB; `serial` picks the region (an IPL of another region is not used).
 * GBA: gba_bios.bin (16 KB).  GB: dmg_boot.bin / gb_bios.bin (256 B).  GBC: cgb_boot.bin / gbc_bios.bin (2304 B / 4 KB).
 * NDS: bios7.bin + bios9.bin + firmware.bin. PS1 and PS2 are found by their own finders (ps1.cpp, pcsx2.cpp): not handled here. */
Stage find(System s, const std::vector<std::string>& dirs, const std::string& serial = std::string());

/* Settings: the systems the player turned the real startup OFF for (comma separated short names). Default = on where the file exists. */
bool enabled_for(const std::string& off_list, System s);
std::string set_enabled(const std::string& off_list, System s, bool on);

/* Retrorun options for one system. `real`: boot through the console's own startup; the option set differs for off (skip it, as before). */
std::vector<std::string> retro_opts(System s, bool real, bool user_skip_bios, bool gambatte);
/* The launch resumes the last auto-state (GB/GBA --resume): a state resume would skip the startup, so real boot drops it. */
inline bool drops_resume(System s) { return s == System::GBA || s == System::GB || s == System::GBC; }

/* Dolphin.ini text with Core/SkipIPL set (the launcher rewrites it on every launch: real boot only for that launch). */
std::string dolphin_skip_ipl(const std::string& ini, bool skip);
/* PCSX2.ini text with EmuCore/EnableFastBoot set. */
std::string pcsx2_fast_boot(const std::string& ini, bool fast);

/* Online sessions never use it: peers must boot identically (lockstep / rollback). */
inline bool allowed(bool online_session) { return !online_session; }

/* ------------------------------------------------------------------ custom startup sounds (userdata\boot_sounds\<system>.wav|ogg|mp3) */
constexpr double kMaxSoundSeconds = 15.0;
/* Path of the custom sound for a system ("gc" -> <dir>\gc.wav|.ogg|.mp3, first that exists), or "". */
std::string custom_sound_path(const std::string& dir, System s);
/* Decodes wav / ogg / mp3 to interleaved stereo float at `rate`, at most kMaxSoundSeconds (longer files are cut with a short fade).
 * false when the file is unreadable or unsupported. */
bool decode_sound(const std::string& path, int rate, std::vector<float>* stereo);
/* Same from memory with the format given by the extension (".wav", ".ogg", ".mp3"). */
bool decode_sound_mem(const unsigned char* data, size_t n, const std::string& ext, int rate, std::vector<float>* stereo);
/* Which systems have a custom sound in `dir` (short names). */
std::vector<System> custom_sounds_present(const std::string& dir);

}  // namespace hub::realboot
