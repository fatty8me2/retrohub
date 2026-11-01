/* Decoder for the xdelta3 "FGK" secondary compression (adaptive Huffman coding after Faller, Gallager and Knuth;
 * VCDIFF secondary compressor ID 16). xdelta3 only builds it when asked (-DSECONDARY_FGK=1) and nothing selects it by
 * default, so it is rare, but `xdelta3 -S fgk` files exist.
 *
 * Derived from xdelta3 3.2.2 (https://github.com/jmacd/xdelta, tag v3.2.2), xdelta3-fgk.h:
 * Copyright 2016 Joshua MacDonald, Apache License 2.0 (see LICENSE in this folder). RetroHub's changes: decoder only,
 * the pointer-linked nodes became indices into fixed arrays and every index is range-checked. See VENDORED.txt. */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace xd3port {

/* The adaptive model of one section type. xdelta3 keeps one for the data sections, one for the instruction sections
 * and one for the address sections of a patch, and the model carries over from one window to the next, so create one
 * FgkDecoder per section type and call decode() for each window in order. */
class FgkDecoder {
public:
    FgkDecoder();
    ~FgkDecoder();
    FgkDecoder(const FgkDecoder&) = delete;
    FgkDecoder& operator=(const FgkDecoder&) = delete;

    /* Decodes one section: `in` is the compressed bytes (after the uncompressed-length varint), `out` has room for
     * exactly out_n (> 0) bytes. Returns nullptr on success, otherwise a short static description of what was wrong.
     * After an error the model is unusable; stop. */
    const char* decode(const uint8_t* in, size_t in_n, uint8_t* out, size_t out_n);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace xd3port
