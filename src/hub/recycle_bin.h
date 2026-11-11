#pragma once
/* Recycle Bin delete for the hub (screenshot viewer). Hub-only: the runners do not link this, so they do not import shell32 for it.
 *
 * SHFileOperation(FO_DELETE | FOF_ALLOWUNDO) silently deletes PERMANENTLY, and still reports success, on network and removable drives
 * (and when the bin is turned off), so the UI asks recycle_expected() first and words its question to match. It can also pump window
 * messages: call delete_user_file() outside any ImGui Begin/End (the viewer queues the path and runs it at the start of the next frame). */
#include <functional>
#include <string>

namespace hubrb {

/* SHFileOperation wants "path\0\0": the path plus two NULs (size() == path.size() + 2). */
std::wstring shfile_path_buffer(const std::wstring& path);

/* Pure: does a drive of this GetDriveType() value keep a Recycle Bin? Only fixed disks do (DRIVE_FIXED == 3). */
bool drive_type_has_bin(unsigned drive_type);

/* Will a delete of this file go to the Recycle Bin (drive type + SHQueryRecycleBinW on the drive)? False when unsure. */
bool recycle_expected(const std::string& utf8_path);

/* Delete to the bin (no dialog, no confirmation). True when the file is gone into the bin. */
bool move_to_recycle_bin(const std::string& utf8_path);

using DeleteFn = std::function<bool(const std::string&)>;
/* Delete a user file: the bin first (`recycle`, default move_to_recycle_bin; tests inject a fake), and only if that fails the plain
 * permanent delete. A file that is already gone counts as deleted. True when the file no longer exists. */
bool delete_user_file(const std::string& utf8_path, const DeleteFn& recycle = nullptr);

}  // namespace hubrb
