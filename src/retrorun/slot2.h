#pragma once
/* DS Slot-2 (Wave 3 X3, "Pal Park"): pure helpers for retrorun --slot2-rom / --slot2-save. No SDL, no core; unit-tested (test_slot2.cpp).
 *
 * The melonDS DS core takes a GBA cartridge through its libretro subsystem "Slot 1 & 2 Boot" (3 contents: NDS ROM, GBA ROM, GBA save data) or
 * "Slot 1 & 2 Boot (No GBA Save Data)" (2 contents). The save is given as a plain, unarchived file; the core flushes it back to that path.
 * retrorun hands the core a scratch COPY and only copies it back over the vault file after a clean exit (see Frontend::run), so a crash can never
 * leave a half-written save in the vault. Offline only: every online / link / rollback mode refuses the flags. */
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace retrorun {
namespace slot2 {

struct RunMode {
    bool net = false, ds_link = false, link = false, rollback = false, spectate = false, dropin = false, synctest = false, share_host_save = false;
    bool is_ds = true;
};

/* "" when the flags may be used; else the reason they are refused (logged by retrorun, which then exits with code 2). */
inline std::string refuse_reason(const RunMode& m, bool have_rom, bool have_save) {
    if (!have_rom && have_save) return "--slot2-save needs --slot2-rom";
    if (!have_rom) return "";
    if (!m.net && !m.ds_link && !m.link && !m.rollback && !m.spectate && !m.dropin && !m.synctest && !m.share_host_save) {
        if (!m.is_ds) return "--slot2-rom is only for Nintendo DS games";
        return "";
    }
    if (m.net || m.rollback || m.spectate || m.dropin || m.share_host_save) return "the GBA cartridge (Slot-2) is offline only: not allowed with --net";
    if (m.ds_link) return "the GBA cartridge (Slot-2) is not allowed with DS wireless (--ds-link)";
    return "the GBA cartridge (Slot-2) is not allowed in this mode";
}

struct Sub { std::string ident, desc; unsigned id = 0, num_roms = 0; };

/* Game type of the Slot-2 subsystem: with a save 3 contents, without 2. false when the core does not offer it. */
inline bool pick_game_type(const std::vector<Sub>& subs, bool with_save, unsigned* id) {
    for (const Sub& s : subs) {
        const bool slots = s.desc.find("Slot 1 & 2") != std::string::npos || s.ident.find("slot_1_2") != std::string::npos;
        if (!slots) continue;
        const bool no_sav = s.desc.find("No GBA Save") != std::string::npos || s.ident.find("no_sram") != std::string::npos;
        if (with_save == no_sav || s.num_roms != (with_save ? 3u : 2u)) continue;
        *id = s.id;
        return true;
    }
    return false;
}

/* Copy the scratch save back over the vault file? Only when the core changed it, it is not empty, and its size is the original's
 * (a GBA battery keeps its size; anything else is a failed or foreign write and the vault file stays untouched). */
inline bool should_copy_back(const std::vector<uint8_t>& original, const std::vector<uint8_t>& work) {
    if (work.empty() || work == original) return false;
    return original.empty() || work.size() == original.size();
}

/* Backup file name: "<file name>.<yyyymmdd-hhmmss>.bak". */
inline std::string backup_name(const std::string& file_name, const std::string& stamp) { return file_name + "." + stamp + ".bak"; }

/* The name for a new backup that never takes the place of an earlier one: "<file name>.<stamp>-NN.bak", NN = 01, 02, ... the first not in
 * `existing` (two runs inside one second, or a clock that went back). The counter is zero padded, so names still sort oldest first. "" = 99 taken. */
inline std::string unique_backup_name(const std::string& file_name, const std::string& stamp, const std::vector<std::string>& existing) {
    for (int n = 1; n < 100; ++n) {
        const std::string name = backup_name(file_name, stamp + (n < 10 ? "-0" : "-") + std::to_string(n));
        if (std::find(existing.begin(), existing.end(), name) == existing.end()) return name;
    }
    return std::string();
}

/* Of the existing backup names of one save (stamped names sort oldest first), those to delete so only `keep` newest remain. */
inline std::vector<std::string> backups_to_prune(std::vector<std::string> names, size_t keep) {
    std::sort(names.begin(), names.end());
    std::vector<std::string> out;
    if (names.size() > keep) out.assign(names.begin(), names.end() - (std::ptrdiff_t)keep);
    return out;
}

/* A name is one of this save's backups: "<file name>.<stamp>.bak". */
inline bool is_backup_of(const std::string& name, const std::string& file_name) {
    return name.size() > file_name.size() + 5 && name.compare(0, file_name.size() + 1, file_name + ".") == 0 &&
           name.compare(name.size() - 4, 4, ".bak") == 0;
}

}  // namespace slot2
}  // namespace retrorun
