#pragma once
/* retrorun's file writes (battery saves, clocks, save states): a temp file beside the target, then one atomic replace, so a crash never leaves
 * a torn file. Two flavours (unit tested: test_save_write.cpp):
 *   in game (at_exit = false): one attempt, never waits. A battery save that fails because another program holds the old file stays pending
 *       and is written again at the next 30 s check, so the emulation thread never stalls on the disk.
 *   at exit (at_exit = true): the last write of the session has no second chance, so it goes through durable::write_file_durable: flushed to
 *       disk before the replace (a power cut leaves the old file or the new one, never a zero-filled one) and retried for up to
 *       durable::kRetryLongMs while a sync client, backup tool or virus scanner still holds the old file. Before, one failed rename there
 *       lost everything saved in the game since the last periodic write (only a log line said so). */
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#include "durable_write.h"

namespace retrorun {

inline bool write_atomic(const std::string& path, const void* data, size_t size, bool at_exit = false) {
    const std::filesystem::path p = std::filesystem::u8path(path);
    if (at_exit) return durable::write_file_durable(p.wstring(), data, size, durable::kRetryLongMs);
    std::filesystem::path tmp = p;
    tmp += L".tmp";
    {
        FILE* f = _wfopen(tmp.c_str(), L"wb");
        if (!f) return false;
        bool ok = size == 0 || fwrite(data, 1, size, f) == size;
        ok = (fflush(f) == 0) && ok;
        fclose(f);
        if (!ok) { std::error_code ec; std::filesystem::remove(tmp, ec); return false; }
    }
    if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

}  // namespace retrorun
