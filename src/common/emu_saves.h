#pragma once
/* Save-folder mapping for the external emulators (Azahar for 3DS, Cemu for Wii U): where a game's
 * save data lives inside the emulator's user dir, and a newest-wins tree copy used to move it
 * between that folder and the RetroHub save vault. Pure logic: no UI, no process handling. */

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hub {

/* ---- 3DS (Azahar): <user>\sdmc\Nintendo 3DS\<id0>\<id1>\... ---- */

/* Paths relative to the "<id0>\<id1>" folder, forward slashes. Title id 0004000000055D00 ->
 *   n3ds_save_rel    "title/00040000/00055d00/data"         (cartridge + digital save data)
 *   n3ds_extdata_rel "extdata/00000000/00055d00"            (extra save data, many titles use the unique id) */
std::string n3ds_save_rel(uint64_t title_id);
std::string n3ds_extdata_rel(uint64_t title_id);
/* "<user>\sdmc\Nintendo 3DS\<id0>\<id1>" -- the first existing pair (sorted), else the all-zero ids Azahar creates. */
std::string n3ds_sd_root(const std::string& azahar_user_dir);

/* ---- Wii U (Cemu): <mlc01>\usr\save\<hi>\<lo>\... ---- */

std::string wiiu_save_rel(uint64_t title_id);   // "usr/save/00050000/1019e500"

/* Where an existing Cemu install keeps its data. Follows Cemu's own rules (src/gui/wxgui/CemuApp.cpp):
 *   <exe dir>\portable\ exists        -> user data = that folder (portable)
 *   else <exe dir>\settings.xml       -> user data = exe dir (legacy portable)
 *   else                              -> %APPDATA%\Cemu
 * and <mlc_path> from settings.xml when non-empty, else <user data>\mlc01. */
struct CemuDirs {
    std::string user_data;
    std::string mlc;
    bool portable = false;
};
struct FsProbe {
    std::function<bool(const std::string&)> dir_exists;
    std::function<bool(const std::string&)> file_exists;
    std::function<std::string(const std::string&)> read_text;
};
CemuDirs cemu_dirs_for(const std::string& cemu_exe_dir, const std::string& appdata_roaming, const FsProbe& fs);
/* <mlc_path>...</mlc_path> from a settings.xml body ("" when absent/empty). */
std::string cemu_settings_mlc_path(const std::string& settings_xml);

/* ---- newest-wins tree copy ---- */

struct TreeSyncReport {
    int copied = 0;       // files written to dst
    int identical = 0;    // already equal
    int kept = 0;         // dst was newer than src and left alone
    int backed_up = 0;    // overwritten dst files saved first into backup_dir
    bool ok = true;
    std::string error;
};

/* Copy every file under src into dst. A file missing in dst is copied; equal content is skipped; when
 * the content differs the file with the newer modification time wins. Before dst is overwritten the old
 * file is copied to <backup_dir>/<relative path>.<backup_tag> (skipped when backup_dir is empty).
 * Never deletes anything. Keeps modification times. A missing src is not an error (nothing to do). */
TreeSyncReport sync_tree_newest(const std::string& src, const std::string& dst, const std::string& backup_dir,
                                const std::string& backup_tag);

/* True when `dir` exists and holds at least one regular file. */
bool tree_has_files(const std::string& dir);

/* Names of the immediate subfolders of `dir` whose newest file is at or after `since_unix` (seconds). */
std::vector<std::string> subdirs_modified_since(const std::string& dir, int64_t since_unix);

} // namespace hub
