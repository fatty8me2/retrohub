/* VCDIFF (RFC 3284) decoder with the xdelta3 extensions real files use: Adler-32 per window (VCD_ADLER32), the
 * application header, and secondary compression (DJW and LZMA/xz). Decoder only.
 *
 * Streaming: the patch is read through bounds-checked cursors, one window at a time; the game is read on demand for
 * COPY instructions that point into the source segment; the result goes out through the Emitter, which writes
 * it to the sink in 1 MB pieces. Memory is about three times one window (the three sections plus the target
 * window), not the size of the game. No Windows headers here (the LZMA SDK defines True / False). */

#include "patcher_internal.h"

#include "djw_decode.h"
#include "fgk_decode.h"
#include "Lzma2Dec.h"

#include <cstdio>
#include <cstdlib>

namespace hub::patch::detail {
namespace {

constexpr uint64_t kMaxWindow = 256ull << 20;      // largest target window we will build (xdelta3 itself stops at 64 MB)
constexpr uint64_t kMaxOffset = 1ull << 62;        // keeps every sum of two offsets inside 64 bits
constexpr unsigned kNear = 4, kSame = 3;           // the default address cache of RFC 3284 section 5.1

constexpr unsigned kIdDjw = 1, kIdLzma = 2, kIdFgk = 16;   // secondary compressor IDs used by xdelta3

constexpr const char* kMsgCustomTable = "This patch uses a custom compression table, which the hub can't read yet.";
constexpr const char* kMsgWindowTooBig = "This patch uses windows that are too large for the hub.";

uint32_t adler32(const uint8_t* p, size_t n) {
    uint32_t s1 = 1, s2 = 0;
    while (n) {
        size_t k = std::min<size_t>(n, 5552);
        n -= k;
        do { s1 += *p++; s2 += s1; } while (--k);
        s1 %= 65521u;
        s2 %= 65521u;
    }
    return (s2 << 16) | s1;
}

/* ---- the default code table (RFC 3284 section 5.6) ---- */
struct DefaultTable {
    VcdCode e[256];
    DefaultTable() {
        int i = 0;
        auto add = [&](int t1, int s1, int m1, int t2, int s2, int m2) {
            e[i++] = VcdCode{(uint8_t)t1, (uint8_t)s1, (uint8_t)m1, (uint8_t)t2, (uint8_t)s2, (uint8_t)m2};
        };
        add(2, 0, 0, 0, 0, 0);                                                         // 0: RUN
        add(1, 0, 0, 0, 0, 0);                                                         // 1: ADD, size follows
        for (int s = 1; s <= 17; ++s) add(1, s, 0, 0, 0, 0);                           // 2..18: ADD 1..17
        for (int m = 0; m <= 8; ++m) {                                                 // 19..162: COPY, every mode
            add(3, 0, m, 0, 0, 0);
            for (int s = 4; s <= 18; ++s) add(3, s, m, 0, 0, 0);
        }
        for (int m = 0; m <= 5; ++m)                                                   // 163..234: ADD 1..4 + COPY 4..6
            for (int a = 1; a <= 4; ++a)
                for (int cs = 4; cs <= 6; ++cs) add(1, a, 0, 3, cs, m);
        for (int m = 6; m <= 8; ++m)                                                   // 235..246: ADD 1..4 + COPY 4 (same cache)
            for (int a = 1; a <= 4; ++a) add(1, a, 0, 3, 4, m);
        for (int m = 0; m <= 8; ++m) add(3, 4, m, 1, 1, 0);                            // 247..255: COPY 4 + ADD 1
        if (i != 256) std::abort();   // a coding mistake in this table, caught the first time any VCDIFF patch is read
    }
};

/* ---- the address cache of one window (RFC 3284 section 5.1) ---- */
struct AddrCache {
    uint64_t near_[kNear];
    uint64_t same_[kSame * 256];
    unsigned next_;
    void reset() {
        std::memset(near_, 0, sizeof near_);
        std::memset(same_, 0, sizeof same_);
        next_ = 0;
    }
    void update(uint64_t a) {
        near_[next_] = a;
        next_ = (next_ + 1) % kNear;
        same_[a % (kSame * 256)] = a;
    }
};

/* Address of a COPY: `here` is the current position in the combined (source segment + window so far) address space. */
uint64_t decode_address(unsigned mode, uint64_t here, Cursor& addr_sec, AddrCache& cache) {
    uint64_t addr;
    if (mode < 2 + kNear) {
        const uint64_t v = addr_sec.varint();
        if (mode == 0) {                   // VCD_SELF
            addr = v;
        } else if (mode == 1) {            // VCD_HERE
            if (v > here) damaged("VCDIFF: address before the start");
            addr = here - v;
        } else {                           // a "near" slot
            addr = cache.near_[mode - 2];
            if (v > UINT64_MAX - addr) damaged("VCDIFF: address overflow");
            addr += v;
        }
    } else {
        const unsigned m = mode - (2 + kNear);
        if (m >= kSame) damaged("VCDIFF: bad address mode");
        addr = cache.same_[m * 256 + addr_sec.u8()];
    }
    cache.update(addr);
    return addr;
}

/* ---- secondary compression with LZMA: an xz stream per section type that lasts for the whole patch ----
 * xdelta3 starts one xz stream (stream header, block header, LZMA2 chunks) per section type, then every window adds
 * LZMA2 chunks to it, each window's piece ending on a sync-flush so its bytes can be decoded on their own. */
void* lz_alloc(ISzAllocPtr, size_t size) { return std::malloc(size); }
void lz_free(ISzAllocPtr, void* p) { std::free(p); }

class XzStream {
public:
    XzStream() {
        alloc_.Alloc = lz_alloc;
        alloc_.Free = lz_free;
        Lzma2Dec_Construct(&dec_);
    }
    ~XzStream() {
        if (inited_) Lzma2Dec_Free(&dec_, &alloc_);
    }
    XzStream(const XzStream&) = delete;
    XzStream& operator=(const XzStream&) = delete;

    /* Decodes exactly out_n bytes from the next piece of the stream. */
    void decode(const uint8_t* in, size_t in_n, uint8_t* out, size_t out_n) {
        if (!started_) start(in, in_n);
        while (out_n) {
            SizeT slen = in_n, dlen = out_n;
            ELzmaStatus st = LZMA_STATUS_NOT_SPECIFIED;
            const SRes r = Lzma2Dec_DecodeToBuf(&dec_, out, &dlen, in, &slen, LZMA_FINISH_ANY, &st);
            in += slen; in_n -= slen; out += dlen; out_n -= dlen;
            if (r != SZ_OK) damaged("VCDIFF: LZMA data error");
            if (out_n && (st == LZMA_STATUS_FINISHED_WITH_MARK || (slen == 0 && dlen == 0)))
                damaged("VCDIFF: LZMA section ends early");
        }
        if (in_n) damaged("VCDIFF: LZMA section has extra data");
    }

private:
    static uint32_t le(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

    /* Parses the xz stream header and the block header (only the LZMA2 filter is allowed), starts the decoder. */
    void start(const uint8_t*& in, size_t& in_n) {
        static const uint8_t kMagic[6] = {0xFD, '7', 'z', 'X', 'Z', 0x00};
        if (in_n < 12 + 12) damaged("VCDIFF: xz header truncated");
        if (std::memcmp(in, kMagic, 6) != 0) damaged("VCDIFF: not an xz stream");
        if (in[6] != 0 || (in[7] & 0xF0) != 0) fail(Error::Unsupported, kMsgUnsupported, "xz stream flags");
        if (crc32(in + 6, 2) != le(in + 8)) damaged("VCDIFF: xz header checksum");
        const uint8_t* bh = in + 12;
        if (bh[0] == 0) damaged("VCDIFF: xz block header");
        const size_t bh_size = ((size_t)bh[0] + 1) * 4;
        if (bh_size < 12 || bh_size > in_n - 12) damaged("VCDIFF: xz block header truncated");
        if (crc32(bh, bh_size - 4) != le(bh + bh_size - 4)) damaged("VCDIFF: xz block header checksum");
        const unsigned flags = bh[1];
        if (flags & 0x3C) damaged("VCDIFF: xz block flags");
        size_t p = 2;
        const size_t lim = bh_size - 4;
        auto vli = [&]() -> uint64_t {                        // xz variable-length integer: 7 bits per byte, low first
            uint64_t v = 0;
            for (unsigned i = 0; i < 9; ++i) {
                if (p >= lim) damaged("VCDIFF: xz block header");
                const uint8_t b = bh[p++];
                v |= (uint64_t)(b & 0x7F) << (7 * i);
                if (!(b & 0x80)) return v;
            }
            damaged("VCDIFF: xz block header");
        };
        if (flags & 0x40) vli();                              // compressed size (not used)
        if (flags & 0x80) vli();                              // uncompressed size (not used)
        if ((flags & 3) != 0) fail(Error::Unsupported, kMsgUnsupported, "xz filter chain");
        const uint64_t filter_id = vli();
        const uint64_t prop_size = vli();
        if (filter_id != 0x21 || prop_size != 1 || p >= lim) fail(Error::Unsupported, kMsgUnsupported, "xz filter");
        const uint8_t prop = bh[p++];
        while (p < lim) {
            if (bh[p++] != 0) damaged("VCDIFF: xz block header padding");
        }
        if (prop > 40) damaged("VCDIFF: LZMA2 dictionary size");
        const uint64_t dict = (uint64_t)(2u | (prop & 1u)) << (prop / 2 + 11);
        if (dict > (256ull << 20)) fail(Error::Unsupported, kMsgUnsupported, "LZMA2 dictionary larger than 256 MB");
        inited_ = true;
        if (Lzma2Dec_Allocate(&dec_, prop, &alloc_) != SZ_OK) fail(Error::TooBig, kMsgNoMemory);
        Lzma2Dec_Init(&dec_);
        started_ = true;
        in += 12 + bh_size;
        in_n -= 12 + bh_size;
    }

    ISzAlloc alloc_;
    CLzma2Dec dec_;
    bool inited_ = false;
    bool started_ = false;
};

/* A readable header text for Info::metadata: the application header when it is plain text, else nothing. */
std::string printable(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const uint8_t b = p[i];
        if (b < 0x20 && b != '\t' && b != '\r' && b != '\n') return std::string();
        if (b == 0x7F) return std::string();
    }
    return std::string(reinterpret_cast<const char*>(p), n);
}

/* ---- the decoder proper ---- */
class VcdDecoder {
public:
    explicit VcdDecoder(Job& j) : j_(j), game_(*j.game), psz_(j.patch->size()), c_(*j.patch, 0, j.patch->size()), e_(j) {}

    void run() {
        header();
        while (c_.remaining() > 0) {
            window();
            j_.report((double)c_.pos() / (double)psz_);
        }
        e_.finish();
    }

private:
    void header() {
        uint8_t magic[4];
        c_.read(magic, 4);
        if (magic[0] != 0xD6 || magic[1] != 0xC3 || magic[2] != 0xC4) damaged("not a VCDIFF patch");
        if (magic[3] != 0) fail(Error::Unsupported, kMsgUnsupported, "VCDIFF version " + std::to_string(magic[3]));
        hdr_ind_ = c_.u8();
        if (hdr_ind_ & ~7u) damaged("VCDIFF: unknown header bits");
        if (hdr_ind_ & 1) sec_id_ = c_.u8();
        if (hdr_ind_ & 2) fail(Error::Unsupported, kMsgCustomTable, "VCD_CODETABLE");
        if (hdr_ind_ & 4) {                       // application header (xdelta3: the file names), not needed to decode
            const uint64_t len = c_.varint();
            if (len > c_.remaining()) damaged("VCDIFF: application header past the end");
            c_.skip(len);
        }
    }

    /* Reads one section of a window into sec_[which], undoing the secondary compression when the window says so.
     * `cap` is the most a sane section of this kind can hold for the window's length. */
    void load_section(int which, uint64_t len, bool compressed, uint64_t cap) {
        std::vector<uint8_t>& raw = sec_[which];
        raw.resize((size_t)len);
        c_.read(raw.data(), raw.size());
        if (!compressed) return;
        Cursor sc(raw.data(), raw.size());
        const uint64_t dec = sc.varint();
        if (dec == 0) damaged("VCDIFF: empty compressed section");
        if (dec > cap) damaged("VCDIFF: compressed section too large");
        std::vector<uint8_t> plain((size_t)dec);
        const uint8_t* body = raw.data() + sc.pos();
        const size_t body_n = raw.size() - (size_t)sc.pos();
        switch (sec_id_) {
            case kIdDjw: {
                const char* err = xd3port::djw_decode(body, body_n, plain.data(), plain.size());
                if (err) damaged(std::string("VCDIFF: DJW: ") + err);
                break;
            }
            case kIdLzma:
                xz_[which].decode(body, body_n, plain.data(), plain.size());
                break;
            case kIdFgk: {
                if (!fgk_[which]) fgk_[which].reset(new xd3port::FgkDecoder);
                const char* err = fgk_[which]->decode(body, body_n, plain.data(), plain.size());
                if (err) damaged(std::string("VCDIFF: FGK: ") + err);
                break;
            }
            default:
                fail(Error::Unsupported, kMsgUnsupported, "secondary compressor " + std::to_string(sec_id_));
        }
        raw.swap(plain);
    }

    void window() {
        const unsigned win_ind = c_.u8();
        if (win_ind & ~7u) damaged("VCDIFF: unknown window bits");
        const bool use_src = (win_ind & 1) != 0, use_tgt = (win_ind & 2) != 0, has_sum = (win_ind & 4) != 0;
        if (use_src && use_tgt) damaged("VCDIFF: window copies from both the source and the target");
        uint64_t cpylen = 0, cpyoff = 0;
        if (use_src || use_tgt) {
            cpylen = c_.varint();
            cpyoff = c_.varint();
            if (cpylen > kMaxOffset || cpyoff > kMaxOffset) damaged("VCDIFF: copy window out of range");
        }
        const uint64_t enclen = c_.varint();
        const uint64_t delta_start = c_.pos();
        const uint64_t tgtlen = c_.varint();
        const unsigned del_ind = c_.u8();
        if (del_ind & ~7u) damaged("VCDIFF: unknown delta bits");
        if (del_ind != 0 && !(hdr_ind_ & 1)) damaged("VCDIFF: compressed section without a compressor");
        const uint64_t dlen = c_.varint(), ilen = c_.varint(), alen = c_.varint();
        uint32_t want_sum = 0;
        if (has_sum) want_sum = c_.be32();
        const uint64_t head_bytes = c_.pos() - delta_start;
        if (dlen > psz_ || ilen > psz_ || alen > psz_) damaged("VCDIFF: section longer than the patch");
        if (enclen != head_bytes + dlen + ilen + alen) damaged("VCDIFF: window length does not add up");
        if (dlen + ilen + alen > c_.remaining()) damaged("VCDIFF: window past the end of the patch");

        if (tgtlen > kMaxWindow) fail(Error::Unsupported, kMsgWindowTooBig, "window of " + std::to_string(tgtlen) + " bytes");
        if (tgtlen > j_.max_out || e_.pos() > j_.max_out - tgtlen) fail(Error::TooBig, kMsgTooBig);
        if (use_tgt && (cpyoff > e_.pos() || cpylen > e_.pos() - cpyoff)) damaged("VCDIFF: target copy window past what was written");

        /* ADD / RUN bytes cannot outnumber the bytes of the window; instructions and addresses take a few bytes each. */
        load_section(0, dlen, (del_ind & 1) != 0, tgtlen);
        load_section(1, ilen, (del_ind & 2) != 0, tgtlen * 8 + 4096);
        load_section(2, alen, (del_ind & 4) != 0, tgtlen * 8 + 4096);
        Cursor data(sec_[0].data(), sec_[0].size());
        Cursor inst(sec_[1].data(), sec_[1].size());
        Cursor addr(sec_[2].data(), sec_[2].size());

        /* The window buffer grows with what the instructions really produce, so a window that claims 200 MB and
         * delivers nothing costs nothing. */
        uint8_t* w = win_.data();
        auto grow = [&](uint64_t need) {
            if (need <= win_.size()) return;
            const uint64_t step = std::min<uint64_t>(tgtlen, (uint64_t)win_.size() * 2 + 65536);
            win_.resize((size_t)std::max(need, step));
            w = win_.data();
        };
        cache_.reset();
        bool used_src = false;
        uint64_t wpos = 0;                                    // bytes of this window built so far

        const VcdCode* table = vcd_default_table();
        while (inst.remaining() > 0) {
            const VcdCode& code = table[inst.u8()];
            for (int half = 0; half < 2; ++half) {
                const unsigned type = half ? code.type2 : code.type1;
                if (type == 0) continue;
                uint64_t size = half ? code.size2 : code.size1;
                if (size == 0) size = inst.varint();
                uint64_t a = 0;
                if (type == 3) {
                    a = decode_address(half ? code.mode2 : code.mode1, cpylen + wpos, addr, cache_);
                    if (a >= cpylen + wpos) damaged("VCDIFF: copy from an address that does not exist yet");
                }
                if (size > tgtlen - wpos) damaged("VCDIFF: instruction runs past the end of its window");
                grow(wpos + size);
                if (type == 1) {                              // ADD
                    data.read(w + wpos, (size_t)size);
                    wpos += size;
                } else if (type == 2) {                       // RUN
                    std::memset(w + wpos, data.u8(), (size_t)size);
                    wpos += size;
                } else {                                      // COPY
                    uint64_t left = size;
                    while (left) {
                        if (a < cpylen) {                     // inside the source segment
                            const uint64_t take = std::min(left, cpylen - a);
                            if (use_src) {
                                const uint64_t abs = cpyoff + a;
                                if (abs > game_.size() || take > game_.size() - abs)
                                    fail(Error::WrongSource, kMsgWrongGame, "the patch copies from past the end of the game");
                                if (!game_.read(abs, w + wpos, (size_t)take)) fail(Error::Io, game_.io_message());
                                used_src = true;
                            } else {
                                e_.read_back(cpyoff + a, w + wpos, (size_t)take);
                            }
                            wpos += take; a += take; left -= take;
                        } else {                              // inside the window itself; may overlap what it writes
                            const uint8_t* src = w + (a - cpylen);
                            uint8_t* dst = w + wpos;
                            const uint64_t dist = wpos - (a - cpylen);       // >= 1 because a < cpylen + wpos
                            uint64_t todo = left;
                            while (todo) {
                                const uint64_t k = std::min(todo, dist);
                                std::memcpy(dst, src, (size_t)k);
                                dst += k; src += k; todo -= k;
                            }
                            wpos += left; a += left; left = 0;
                        }
                    }
                }
            }
        }
        if (wpos != tgtlen) damaged("VCDIFF: window is shorter than it says");
        if (data.remaining() != 0 || addr.remaining() != 0) damaged("VCDIFF: unused data left in a window");

        if (has_sum) {
            const uint32_t got = adler32(w, (size_t)tgtlen);
            if (got != want_sum) {
                char buf[96];
                std::snprintf(buf, sizeof buf, "window at output offset %llu: Adler-32 expected %08X, got %08X",
                              (unsigned long long)e_.pos(), want_sum, got);
                if (used_src) fail(Error::WrongSource, kMsgWrongGame, buf);   // by far the likeliest cause: another version of the game
                fail(Error::BadOutput, kMsgBadOutput, buf);
            }
        }
        e_.put(w, (size_t)tgtlen);
    }

    Job& j_;
    Source& game_;
    const uint64_t psz_;
    Cursor c_;
    Emitter e_;
    unsigned hdr_ind_ = 0, sec_id_ = 0;
    std::vector<uint8_t> sec_[3];       // data, instructions, addresses of the window being decoded
    XzStream xz_[3];                    // one LZMA stream per section type, kept across windows
    std::unique_ptr<xd3port::FgkDecoder> fgk_[3];   // likewise one adaptive FGK model per section type
    std::vector<uint8_t> win_;
    AddrCache cache_ = {};
};

}  // namespace

const VcdCode* vcd_default_table() {
    static const DefaultTable t;
    return t.e;
}

void run_vcdiff(Job& j) {
    VcdDecoder d(j);
    d.run();
}

void inspect_vcdiff(const uint8_t* patch, size_t n, Info* out) {
    Cursor c(patch, n);
    uint8_t magic[4];
    c.read(magic, 4);
    if (magic[0] != 0xD6 || magic[1] != 0xC3 || magic[2] != 0xC4 || magic[3] != 0) damaged("not a VCDIFF patch");
    const unsigned hind = c.u8();
    if (hind & ~7u) damaged("VCDIFF: unknown header bits");
    if (hind & 1) c.u8();
    if (hind & 2) return;                    // custom code table: its length would be needed to find the rest
    if (hind & 4) {
        const uint64_t len = c.varint();
        const uint64_t avail = c.remaining();
        const size_t take = (size_t)std::min(len, avail);
        std::vector<uint8_t> hdr(take);
        c.read(hdr.data(), take);
        if (out) out->metadata = printable(hdr.data(), hdr.size());
    }
}

}  // namespace hub::patch::detail
