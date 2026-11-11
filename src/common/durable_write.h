#pragma once
/* Crash-safe file commit. A file written to a temp name beside its destination is flushed to disk (FlushFileBuffers) BEFORE it is
 * moved into place with one MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH), so a power loss can leave the old file or the new file,
 * never a zero-length / short one and never no file. The destination is never removed first. */

#include <string>

namespace durable {

/* Flush `tmp` to disk, then atomically replace `dst` with it (same volume: tmp sits beside dst).
 * Retries the move a few times (~50 ms apart) on ERROR_SHARING_VIOLATION / ERROR_ACCESS_DENIED (antivirus, cloud clients).
 * On failure `dst` is untouched, `tmp` is left in place (the caller removes it), and GetLastError() holds the reason.
 * Long paths (> 260) and non-ASCII paths work. */
bool commit_replace(const std::wstring& tmp, const std::wstring& dst);
/* Same for narrow paths read in the ANSI code page (how std::ofstream / std::rename read a std::string path). */
bool commit_replace_ansi(const std::string& tmp, const std::string& dst);

/* Write `bytes` to "<dst>.hubtmp" (flushed), then commit_replace. The temp file is removed on failure (error code preserved). */
bool write_file_durable(const std::wstring& dst, const void* bytes, size_t len);
bool write_file_durable(const std::wstring& dst, const std::string& bytes);

}  // namespace durable
