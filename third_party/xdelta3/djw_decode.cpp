/* xdelta3 DJW secondary-compression decoder. See djw_decode.h.

   Derived from xdelta3 3.2.2, xdelta3-djw.h
   Copyright 2016 Joshua MacDonald

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

   Changes made by RetroHub: the encoder is not included; the decoder is reimplemented in self-contained C++17;
   all arrays are indexed only after a range check (xdelta3 relies on assertions there), run lengths and selector
   values are validated, and the result is reported as a status string instead of an xd3_stream error. The bit order
   and all table layouts are unchanged, so the output is identical for every stream xdelta3 can produce.

   Format summary (all bits are read least-significant first within a byte; multi-bit fields are most-significant
   bit first):
     groups-1 (3 bits); if groups > 1: sector size / 5 - 1 (5 bits)
     code-length code: count-7 (4 bits), then that many 4-bit code lengths for the 22 symbols RUN_0, RUN_1, 1..20
     the code lengths of every group (256 per group), as MTF + 1/2 run-length symbols; a symbol whose length was
       zero in the previous group stays zero without being coded
     if groups > 1: groups+1 3-bit code lengths, then the group chosen for each sector (MTF + 1/2 coded)
     the data: one canonical-Huffman symbol per output byte, each sector with its group's code */

#include "djw_decode.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace xd3port {
namespace {

constexpr unsigned kAlphabet = 256;                  // ALPHABET_SIZE
constexpr unsigned kMaxCodeLen = 20;                 // DJW_MAX_CODELEN
constexpr unsigned kTotalCodes = kMaxCodeLen + 2;    // DJW_TOTAL_CODES: RUN_0, RUN_1, code lengths 1..20
constexpr unsigned kRun1 = 1;                        // RUN_1 (RUN_0 is 0)
constexpr unsigned kExtra12Offset = 7;               // DJW_BASIC_CODES + DJW_RUN_CODES
constexpr unsigned kExtraCodeBits = 4;               // DJW_EXTRA_CODE_BITS
constexpr unsigned kMaxGroups = 8;                   // DJW_MAX_GROUPS
constexpr unsigned kGroupBits = 3;                   // DJW_GROUP_BITS
constexpr unsigned kSectorMult = 5;                  // DJW_SECTORSZ_MULT
constexpr unsigned kSectorBits = 5;                  // DJW_SECTORSZ_BITS
constexpr unsigned kMaxClclen = 15;                  // DJW_MAX_CLCLEN
constexpr unsigned kClclenBits = 4;                  // DJW_CLCLEN_BITS
constexpr unsigned kMaxGbclen = 7;                   // DJW_MAX_GBCLEN
constexpr unsigned kGbclenBits = 3;                  // DJW_GBCLEN_BITS

/* The initial MTF order of code-length values: 0, then djw_encode_12basic, then djw_encode_12extra. */
constexpr uint8_t kInitialClenMtf[21] = {0, 4, 5, 6, 7, 8, 9, 10, 3, 11, 2, 12, 13, 1, 14, 15, 16, 17, 18, 19, 20};
constexpr unsigned kClenMtfSize = 21;

struct Bits {
    const uint8_t* p;
    const uint8_t* end;
    unsigned byte = 0;
    unsigned mask = 0x100;    // BIT_STATE_DECODE_INIT: the first read loads a byte
};

inline bool next_bit(Bits& b, unsigned* bit) {
    if (b.mask == 0x100) {
        if (b.p == b.end) return false;
        b.byte = *b.p++;
        b.mask = 1;
    }
    *bit = (b.byte & b.mask) ? 1u : 0u;
    b.mask <<= 1;
    return true;
}

/* xd3_decode_bits: n bits, the first one read is the most significant. */
bool get_bits(Bits& b, unsigned n, unsigned* v) {
    unsigned value = 0;
    for (unsigned i = 0; i < n; ++i) {
        unsigned bit;
        if (!next_bit(b, &bit)) return false;
        value = (value << 1) | bit;
    }
    *v = value;
    return true;
}

/* Canonical Huffman decoding tables (djw_build_decoder). */
struct Huff {
    unsigned min_len = 0, max_len = 0;    // max_len == 0: no symbol has a code
    unsigned nsyms = 0;                   // symbols that have a code
    unsigned base[kMaxCodeLen + 2] = {};
    unsigned limit[kMaxCodeLen + 2] = {};
    uint8_t inorder[kAlphabet] = {};
};

bool build(Huff& h, const uint8_t* clen, unsigned asize, unsigned abs_max) {
    if (asize > kAlphabet || abs_max > kMaxCodeLen) return false;
    unsigned nr[kMaxCodeLen + 2] = {};
    for (unsigned i = 0; i < asize; ++i) {
        if (clen[i] > abs_max) return false;
        nr[clen[i]]++;
    }
    unsigned min_len = abs_max + 1, max_len = 0;
    for (unsigned l = 1; l <= abs_max; ++l) {
        if (nr[l]) { min_len = l; break; }
    }
    for (unsigned l = abs_max; l >= 1; --l) {
        if (nr[l]) { max_len = l; break; }
    }
    h.min_len = min_len;
    h.max_len = max_len;
    h.nsyms = 0;
    if (max_len == 0) return true;

    unsigned tmp_base[kMaxCodeLen + 2] = {};
    h.base[min_len] = 0;
    h.limit[min_len] = nr[min_len] - 1;
    for (unsigned l = min_len + 1; l <= max_len; ++l) {
        unsigned last_limit = (h.limit[l - 1] + 1) << 1;
        tmp_base[l] = tmp_base[l - 1] + nr[l - 1];
        h.limit[l] = last_limit + nr[l] - 1;
        h.base[l] = last_limit - tmp_base[l];
    }
    for (unsigned i = 0; i < asize; ++i) {
        unsigned l = clen[i];
        if (l == 0) continue;
        h.inorder[tmp_base[l]++] = (uint8_t)i;      // tmp_base[l] < total symbols <= asize <= 256
        h.nsyms++;
    }
    return true;
}

/* djw_decode_symbol: one canonical code, bit by bit. */
bool decode_symbol(Bits& b, const Huff& h, unsigned* sym) {
    if (h.max_len == 0) return false;
    unsigned code = 0, bits = 0;
    for (;;) {
        if (bits == h.max_len) return false;
        unsigned bit;
        if (!next_bit(b, &bit)) return false;
        ++bits;
        code = (code << 1) | bit;
        if (bits >= h.min_len && code <= h.limit[bits]) {
            if (h.base[bits] <= code) {
                unsigned offset = code - h.base[bits];
                if (offset < h.nsyms) {
                    *sym = h.inorder[offset];
                    return true;
                }
            }
            return false;
        }
    }
}

/* djw_decode_1_2: `elts` values coded as MTF indices with RUN_0 / RUN_1 run lengths (bijective base 2).
 * With skip_offset != 0 (code lengths of group 2 and up) a value whose counterpart one group earlier was zero is
 * zero again and costs nothing. */
bool decode_1_2(Bits& b, const Huff& h, uint8_t* mtfvals, unsigned mtf_size, unsigned elts, unsigned skip_offset,
                uint8_t* values) {
    unsigned n = 0, rep = 0, mtf = 0, s = 0;
    while (n < elts) {
        if (skip_offset != 0 && n >= skip_offset && values[n - skip_offset] == 0) {
            values[n++] = 0;
            continue;
        }
        if (rep != 0) {
            values[n++] = mtfvals[0];
            --rep;
            continue;
        }
        if (mtf != 0) {
            if (mtf >= mtf_size) return false;
            uint8_t sym = mtfvals[mtf];
            for (unsigned k = mtf; k != 0; --k) mtfvals[k] = mtfvals[k - 1];
            mtfvals[0] = sym;
            values[n++] = sym;
            mtf = 0;
            continue;
        }
        unsigned sym;
        if (!decode_symbol(b, h, &sym)) return false;
        if (sym <= kRun1) {
            if (s >= 24) return false;
            rep = (sym + 1u) << s;
            ++s;
            if (rep > elts - n) return false;       // more repeats than values left
        } else {
            mtf = sym - 1;
            s = 0;
        }
    }
    return rep == 0;
}

}  // namespace

const char* djw_decode(const uint8_t* in, size_t in_n, uint8_t* out, size_t out_n) {
    if (out_n == 0) return "empty section";
    Bits b{in, in + in_n};

    unsigned groups;
    if (!get_bits(b, kGroupBits, &groups)) return "truncated header";
    groups += 1;

    size_t sector_size;
    if (groups > 1) {
        unsigned v;
        if (!get_bits(b, kSectorBits, &v)) return "truncated header";
        sector_size = (size_t)(v + 1) * kSectorMult;
    } else {
        sector_size = out_n;
    }
    const size_t sectors = 1 + (out_n - 1) / sector_size;

    /* The code that the code lengths are written in. */
    Huff cl;
    uint8_t cl_mtf[kClenMtfSize];
    {
        unsigned count;
        if (!get_bits(b, kExtraCodeBits, &count)) return "truncated header";
        count += kExtra12Offset;                         // 7 .. 22
        uint8_t cl_clen[kTotalCodes] = {};
        for (unsigned i = 0; i < count; ++i) {
            unsigned v;
            if (!get_bits(b, kClclenBits, &v)) return "truncated header";
            cl_clen[i] = (uint8_t)v;
        }
        if (!build(cl, cl_clen, kTotalCodes, kMaxClclen)) return "bad code-length code";
        std::memcpy(cl_mtf, kInitialClenMtf, sizeof cl_mtf);
    }

    /* The code lengths of every group, then the decoding tables. */
    uint8_t clen[kMaxGroups * kAlphabet];
    if (!decode_1_2(b, cl, cl_mtf, kClenMtfSize, kAlphabet * groups, kAlphabet, clen)) return "bad code lengths";
    std::vector<Huff> tables(groups);
    for (unsigned g = 0; g < groups; ++g) {
        if (!build(tables[g], clen + g * kAlphabet, kAlphabet, kMaxCodeLen)) return "bad code lengths";
    }

    /* Which group codes each sector. */
    std::vector<uint8_t> selector;
    if (groups > 1) {
        uint8_t sel_clen[kMaxGroups + 1];
        uint8_t sel_mtf[kMaxGroups + 2];
        for (unsigned g = 0; g < groups + 1; ++g) {
            unsigned v;
            if (!get_bits(b, kGbclenBits, &v)) return "truncated header";
            sel_clen[g] = (uint8_t)v;
            sel_mtf[g] = (uint8_t)g;
        }
        Huff sel;
        if (!build(sel, sel_clen, groups + 1, kMaxGbclen)) return "bad selector code";
        selector.resize(sectors);
        if (sectors > 0xFFFFFFFFu || !decode_1_2(b, sel, sel_mtf, groups + 1, (unsigned)sectors, 0, selector.data()))
            return "bad selectors";
    }

    /* The data. */
    size_t o = 0;
    for (size_t c = 0; c < sectors; ++c) {
        const Huff* h = &tables[0];
        if (groups > 1) {
            if (selector[c] >= groups) return "bad selector";
            h = &tables[selector[c]];
        }
        size_t n = std::min(sector_size, out_n - o);
        do {
            unsigned sym;
            if (!decode_symbol(b, *h, &sym)) return "bad data";
            out[o++] = (uint8_t)sym;
        } while (--n);
    }
    return nullptr;
}

}  // namespace xd3port
