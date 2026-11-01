/* Decoder for the xdelta3 "DJW" secondary compression (a static, multi-table Huffman coding with
 * move-to-front + run-length coded code lengths; VCDIFF secondary compressor ID 1).
 *
 * Derived from xdelta3 3.2.2 (https://github.com/jmacd/xdelta, tag v3.2.2), xdelta3-djw.h:
 * Copyright 2016 Joshua MacDonald, Apache License 2.0 (see LICENSE in this folder). RetroHub's changes: decoder only,
 * rewritten as self-contained C++ with every table index and every read bounds-checked (the original asserts that its
 * callers validated them), no dependency on the rest of xdelta3. See VENDORED.txt. */
#pragma once

#include <cstddef>
#include <cstdint>

namespace xd3port {

/* Decodes one DJW-compressed section: `in` is the section's compressed bytes (after the uncompressed-length varint),
 * `out` must have room for exactly `out_n` bytes (out_n > 0). Returns nullptr on success, otherwise a short static
 * description of what was wrong with the data. Never reads or writes outside in[0, in_n) / out[0, out_n). */
const char* djw_decode(const uint8_t* in, size_t in_n, uint8_t* out, size_t out_n);

}  // namespace xd3port
