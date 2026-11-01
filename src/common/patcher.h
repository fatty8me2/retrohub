#pragma once
/* ROM patch engine: IPS, UPS, BPS and VCDIFF (xdelta3 output).
 *
 *  - Pure C++17 apart from apply_file / inspect_file / crc32_file, which use Win32 files. All paths are UTF-8.
 *  - Nothing throws out of this API and nothing writes outside the output file: every read of a patch is
 *    bounds-checked, a damaged or truncated patch returns Error::BadPatch, and the output size is capped.
 *  - Result::message is written for the player ("This patch is for a different version of the game.");
 *    Result::detail is the technical version (expected / actual CRCs and so on) for logs.
 *  - apply_file never loads the game or the result into memory: the game is read on demand, the result is
 *    streamed to "<out_path>.tmp" and renamed over out_path only when every check passed. Memory use is a
 *    few MB plus (for VCDIFF) one window of the patch, whatever the size of the game (GameCube ISOs are fine).
 *  - Formats and what they check:
 *      IPS    records, RLE records, "EOF" marker, optional truncation size. No checksums.
 *      UPS    source / target / patch CRC32 and sizes.
 *      BPS    source / target / patch CRC32 and sizes, metadata. The footer's source CRC names the base game.
 *      VCDIFF RFC 3284 plus the xdelta3 extensions: Adler-32 per window, application header, secondary
 *             compression (DJW and LZMA/xz). No whole-file checksum; the caller verifies the output.
 *  - PPF patches are recognised and refused with Error::Unsupported.
 *  See docs/PATCHES.md.
 */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hub::patch {

enum class Format { Unknown, Ips, Ups, Bps, Vcdiff };

/* Format from the first bytes: "PATCH", "UPS1", "BPS1" or the VCDIFF magic D6 C3 C4 00. Unknown otherwise. */
Format detect(const uint8_t* data, size_t n);
/* Format from a file name's extension (case-insensitive): .ips .ups .bps .xdelta .vcdiff .delta. Unknown otherwise. */
Format format_from_ext(const std::string& path);
/* "IPS", "UPS", "BPS", "xdelta" or "unknown", for UI text. */
const char* format_name(Format f);

struct Info {
    Format format = Format::Unknown;
    bool has_source_crc = false; uint32_t source_crc = 0; uint64_t source_size = 0;   // size 0 = unknown
    bool has_target_crc = false; uint32_t target_crc = 0; uint64_t target_size = 0;
    std::string metadata;   // BPS metadata text; for VCDIFF the application header (file names) when it is text
};

/* Reads the headers and footers only (O(1) in the size of the patch), so it is cheap to call on every patch in a folder.
 * It does NOT verify the patch's own checksum; apply() does. IPS has no CRCs, so only `format` is filled in.
 * Returns false (and fills *err in plain words) for something that is not a readable patch. out / err may be null. */
bool inspect(const uint8_t* patch, size_t n, Info* out, std::string* err);
/* Same, from a file: reads at most the first 64 KB and the last 16 bytes. */
bool inspect_file(const std::string& patch_path, Info* out, std::string* err);

enum class Error { None, BadPatch, WrongSource, BadOutput, TooBig, Unsupported, Io, Cancelled };
struct Result {
    Error error = Error::None;
    std::string message;   // plain words a player understands, e.g. "This patch is for a different version of the game."
    bool ok() const { return error == Error::None; }
    std::string detail;    // technical details for the log (CRCs, offsets); may be empty
};

/* Applies `patch` (n bytes) to `source` and puts the result in *out (cleared on failure; may be null to only check).
 *   max_out  largest result allowed in bytes (Error::TooBig beyond it). 0 means no limit.
 * Memory-to-memory: meant for small images (cartridges). Use apply_file for anything big. */
Result apply(const std::vector<uint8_t>& source, const uint8_t* patch, size_t n,
             std::vector<uint8_t>* out, uint64_t max_out);

/* Applies the patch file to the source file and writes the result to out_path.
 *   - Writes "<out_path>.tmp" next to it, then replaces out_path in one step; out_path is never left partial and
 *     an existing out_path is untouched on any failure. The folder of out_path is created if missing.
 *   - out_path may not be the source or the patch file (Error::Io).
 *   - progress(0..1) is called a few hundred times at most; return false to cancel (Error::Cancelled, nothing written).
 *     It runs on the calling thread. May be empty.
 *   - Paths are UTF-8 and may contain spaces and non-ASCII characters. */
Result apply_file(const std::string& source_path, const std::string& patch_path, const std::string& out_path,
                  uint64_t max_out, const std::function<bool(double)>& progress /* return false = cancel */);

/* Standard CRC-32 (zlib / PNG / the one UPS and BPS use). Chain calls by passing the previous result as `prev`:
 * crc32(b, nb, crc32(a, na)) == crc32 of a followed by b. */
uint32_t crc32(const uint8_t* d, size_t n, uint32_t prev = 0);
/* CRC-32 of a whole file, streamed. progress as in apply_file. Returns false on I/O error or cancel (*err says which). */
bool crc32_file(const std::string& path, uint32_t* out, std::string* err,
                const std::function<bool(double)>& progress = std::function<bool(double)>());

}  // namespace hub::patch
