#pragma once
/* PlayStation 2 online rooms (retrorun + the LRPS2 libretro core, docs/PS2_ONLINE.md). Pure helpers, unit tested in test_ps2_online.cpp. */
#include <string>

namespace hubps2 {

/* What the room needs to know to say whether this PC can start a PS2 game online. */
struct OnlineState {
    bool core_present = false;      // pcsx2_libretro.dll is installed (managed download or the cores folder)
    bool core_downloading = false;  // the on-demand download is running
    int percent = 0;                // 0..100 while downloading
    std::string download_error;     // last failed download ("" = none)
    bool bios_found = false;        // a PS2 BIOS (scph*.bin) is available to stage
    bool needs_unpack = false;      // the game is an archive that must be unpacked first
    std::string title;
};

/* "" when fine, else the reason for the READY / LAUNCH button (shown for host and guests alike). */
inline std::string online_block_reason(const OnlineState& s) {
    if (!s.core_present) {
        if (s.core_downloading) return "Downloading the PlayStation 2 online core (" + std::to_string(s.percent) + "%)...";
        if (!s.download_error.empty()) return "The PlayStation 2 online core could not be downloaded: " + s.download_error + ". Check your connection; RetroHub retries shortly.";
        return "Getting the PlayStation 2 online core (a one-time download of about 3.6 MB)...";
    }
    if (!s.bios_found) return "Your PC has no PlayStation 2 BIOS (scph*.bin) - add it in Settings > Playing. Everyone in the room needs the same BIOS file.";
    if (s.needs_unpack) return "Unpacking " + s.title + " (needed for PlayStation 2 online)...";
    return "";
}

/* retrorun options that describe the setup every PC must share: the BIOS file (by name, staged in <system-dir>/pcsx2/bios) and the disc serial,
 * which is mixed into the setup fingerprint (retrorun compares core build, BIOS hash, disc and core options before frame 0). */
inline std::string retrorun_setup_args(const std::string& bios_file, const std::string& serial, bool share_card) {
    std::string a = " --system ps2 --opt pcsx2_bios=" + bios_file;
    if (!serial.empty()) a += " --setup-tag " + serial;
    if (share_card) a += " --share-host-save";
    return a;
}

/* The host's PCSX2 memory card inside a vault battery dir (emulators.cpp pcsx2_card_dir_rel + PCSX2's per-game card name), and where the staged copy goes. */
inline std::string vault_card_rel(const std::string& safe_key) { return "memcards\\RetroHub\\" + safe_key + "\\Mcd001.ps2"; }
inline const char* staged_card_rel() { return "pcsx2\\memcards\\Mcd001.ps2"; }
inline const char* staged_bios_dir_rel() { return "pcsx2\\bios"; }

/* Exit codes of retrorun's PS2 setup check (src/retrorun/main.cpp) -> text for the player. */
inline const char* setup_mismatch_text(unsigned long code) {
    switch (code) {
        case 9: return "Your PlayStation 2 BIOS file differs from another player's. Everyone in the room needs the same BIOS file (the same scph*.bin) - compare them in Settings > Playing.";
        case 10: return "Your game disc differs from another player's. Everyone needs the same disc image of the same game (same region and version).";
        case 11: return "Your PlayStation 2 emulator core or its options differ from another player's. Update RetroHub on every PC and try again.";
        default: return "";
    }
}

}  // namespace hubps2
