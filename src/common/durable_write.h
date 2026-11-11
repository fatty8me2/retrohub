#pragma once
/* Crash-safe file commit. A file written to a temp name beside its destination is flushed to disk (FlushFileBuffers) BEFORE it is
 * moved into place with one atomic replace (MoveFileExW REPLACE_EXISTING | WRITE_THROUGH, or std::filesystem::rename when the
 * destination is held open with FILE_SHARE_DELETE), so a power loss can leave the old file or the new file,
 * never a zero-length / short one and never no file. The destination is never removed first. */

#include <string>

namespace durable {

/* How long a commit keeps retrying a sharing violation (antivirus, cloud clients hold files open for a moment).
 * Short: the UI frame thread (hub settings). Long: worker threads (save copies, history). */
constexpr int kRetryShortMs = 300;
constexpr int kRetryLongMs = 1500;

/* Flush `tmp` to disk, then atomically replace `dst` with it (same volume: tmp sits beside dst).
 * Retries (~50 ms apart, up to `budget_ms`) on ERROR_SHARING_VIOLATION / ERROR_ACCESS_DENIED.
 * A read-only temp (carried over by fs::copy_file) is flushed and its read-only attribute is kept on the final file.
 * On failure `dst` is untouched, `tmp` is left in place (the caller removes it), and GetLastError() holds the reason.
 * Long paths (> 260), non-ASCII and relative paths work. */
bool commit_replace(const std::wstring& tmp, const std::wstring& dst, int budget_ms = kRetryShortMs);
/* Same for narrow paths read in the ANSI code page (how std::ofstream / std::rename read a std::string path). */
bool commit_replace_ansi(const std::string& tmp, const std::string& dst, int budget_ms = kRetryShortMs);

/* UTF-8 path entry points. Hub paths are UTF-8 std::strings and the exe has no UTF-8 manifest, so narrow file APIs would read them
 * in the ANSI code page. These always go through the wide APIs. */
std::wstring utf8_to_wide(const std::string& utf8);
/* The wide path the OLD narrow code (std::ifstream/ofstream/rename on the UTF-8 bytes) really resolved: the same bytes decoded as the
 * ANSI code page. Identical to utf8_to_wide for pure-ASCII text (and when the ANSI code page is UTF-8). Migration use only. */
std::wstring legacy_ansi_to_wide(const std::string& utf8);
bool commit_replace_utf8(const std::string& tmp, const std::string& dst, int budget_ms = kRetryShortMs);

enum class ReadResult { NotFound, Primary, Legacy };
/* Read a whole file by UTF-8 path. If it does not exist there, fall back to the legacy (ANSI-misinterpreted) location, which is where
 * older builds kept it for players with non-ASCII paths. Returns Legacy so the caller can re-save to the correct path; the legacy
 * file is never touched. */
ReadResult read_file_utf8_migrating(const std::string& path, std::string* bytes);
/* Write `bytes` to "<path>.tmp" (wide path, flushed) then commit_replace. Failure leaves the previous file untouched and removes the
 * temp file (only if this call created it). */
bool write_file_atomic_utf8(const std::string& path, const std::string& bytes, int budget_ms = kRetryShortMs);

/* Write `bytes` to "<dst>.hubtmp" (flushed), then commit_replace. The temp file is removed on failure (error code preserved). */
bool write_file_durable(const std::wstring& dst, const void* bytes, size_t len, int budget_ms = kRetryShortMs);
bool write_file_durable(const std::wstring& dst, const std::string& bytes, int budget_ms = kRetryShortMs);

}  // namespace durable
