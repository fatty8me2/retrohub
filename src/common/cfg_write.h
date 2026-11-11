#pragma once
/* Crash-safe writer for the config files RetroHub generates for external emulators (Dolphin.ini, PCSX2.ini, qt-config.ini, ppsspp.ini,
 * controls.ini, Cemu settings.xml, controls.json ...). The bytes written are exactly the bytes given (no BOM, no newline translation).
 * The file is committed with durable::write_file_durable (temp beside it, flushed, one atomic replace), so a crash or power cut leaves
 * the old file or the new file, never an empty one. Header only; link hubbase (every hub target already does). */

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#include <windows.h>

#include "durable_write.h"

namespace hubcfg {

/* True when `p` exists and holds exactly `bytes`. An unreadable or missing file is "different". */
inline bool same_on_disk(const std::filesystem::path& p, const std::string& bytes) {
    std::error_code ec;
    auto sz = std::filesystem::file_size(p, ec);
    if (ec || sz != bytes.size()) return false;
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::string cur((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return cur == bytes;
}

/* Create the parent folder, skip the write when the file already holds `bytes` (no timestamp churn), else commit durably.
 * Returns true when the file holds `bytes` afterwards. */
inline bool write_file(const std::filesystem::path& p, const std::string& bytes, bool skip_if_same = true) {
    std::error_code ec;
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    /* A player may mark a config read-only so the hub leaves it alone: the old truncate-and-write failed there, so this fails too
     * (durable::commit_replace would otherwise clear the attribute and replace the file). */
    DWORD attr = GetFileAttributesW(p.wstring().c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY)) return false;
    if (skip_if_same && same_on_disk(p, bytes)) return true;
    return durable::write_file_durable(p.wstring(), bytes);
}

inline bool write_utf8(const std::string& utf8_path, const std::string& bytes, bool skip_if_same = true) {
    return write_file(std::filesystem::u8path(utf8_path), bytes, skip_if_same);
}

}  // namespace hubcfg
