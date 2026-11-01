/* ROM patch engine: CRC-32, format detection, IPS / UPS / BPS, the Win32 file layer behind apply_file.
 * VCDIFF lives in patcher_vcdiff.cpp. See patcher.h and docs/PATCHES.md. */

#include "patcher_internal.h"

#include "winutil.h"

#include <windows.h>

#include <cstdio>
#include <cwctype>
#include <new>
#include <stdexcept>

namespace hub::patch {

/* ======================================================================================== CRC-32 */

namespace {
struct CrcTables {
    uint32_t t[8][256];
    CrcTables() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = t[0][i];
            for (int k = 1; k < 8; ++k) {
                c = t[0][c & 0xFF] ^ (c >> 8);
                t[k][i] = c;
            }
        }
    }
};
const CrcTables& crc_tables() {
    static const CrcTables t;
    return t;
}
}  // namespace

uint32_t crc32(const uint8_t* d, size_t n, uint32_t prev) {
    const CrcTables& T = crc_tables();
    uint32_t c = ~prev;
    while (n >= 8) {                       // slice-by-8; the host is little-endian (x64 / arm64)
        uint32_t a, b;
        std::memcpy(&a, d, 4);
        std::memcpy(&b, d + 4, 4);
        a ^= c;
        c = T.t[7][a & 0xFF] ^ T.t[6][(a >> 8) & 0xFF] ^ T.t[5][(a >> 16) & 0xFF] ^ T.t[4][a >> 24] ^
            T.t[3][b & 0xFF] ^ T.t[2][(b >> 8) & 0xFF] ^ T.t[1][(b >> 16) & 0xFF] ^ T.t[0][b >> 24];
        d += 8;
        n -= 8;
    }
    while (n--) c = T.t[0][(c ^ *d++) & 0xFF] ^ (c >> 8);
    return ~c;
}

/* ======================================================================================== formats */

static bool is_ppf(const uint8_t* d, size_t n) {
    return d && n >= 5 && std::memcmp(d, "PPF", 3) == 0 && d[3] >= '1' && d[3] <= '3' && d[4] == '0';
}

Format detect(const uint8_t* d, size_t n) {
    if (!d) return Format::Unknown;
    if (n >= 5 && std::memcmp(d, "PATCH", 5) == 0) return Format::Ips;
    if (n >= 4 && std::memcmp(d, "UPS1", 4) == 0) return Format::Ups;
    if (n >= 4 && std::memcmp(d, "BPS1", 4) == 0) return Format::Bps;
    if (n >= 4 && d[0] == 0xD6 && d[1] == 0xC3 && d[2] == 0xC4 && d[3] == 0x00) return Format::Vcdiff;
    return Format::Unknown;
}

Format format_from_ext(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return Format::Unknown;
    std::string ext = path.substr(dot + 1);
    for (char& ch : ext) {
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
    }
    if (ext == "ips") return Format::Ips;
    if (ext == "ups") return Format::Ups;
    if (ext == "bps") return Format::Bps;
    if (ext == "xdelta" || ext == "vcdiff" || ext == "delta") return Format::Vcdiff;
    return Format::Unknown;
}

const char* format_name(Format f) {
    switch (f) {
        case Format::Ips: return "IPS";
        case Format::Ups: return "UPS";
        case Format::Bps: return "BPS";
        case Format::Vcdiff: return "xdelta";
        default: return "unknown";
    }
}

/* ======================================================================================== plumbing */

namespace detail {

void Job::report(double f) {
    if (!progress) return;
    if (f < 0.0) f = 0.0;
    if (f > 1.0) f = 1.0;
    const double v = base_ + span_ * f;
    if (v - last_ < 0.002 && f < 1.0) return;
    last_ = v;
    if (!progress(v)) fail(Error::Cancelled, kMsgCancelled);
}

void Cursor::refill() {
    if (!src_ || next_ >= end_) damaged("read past the end of the patch");
    constexpr uint64_t kBuf = 65536;
    const uint64_t want = std::min<uint64_t>(end_ - next_, kBuf);
    if (buf_.size() < kBuf) buf_.resize((size_t)kBuf);
    if (!src_->read(next_, buf_.data(), (size_t)want)) fail(Error::Io, src_->io_message());
    p_ = buf_.data();
    e_ = p_ + want;
    next_ += want;
}

void Cursor::read(void* dst, size_t n) {
    uint8_t* d = static_cast<uint8_t*>(dst);
    while (n) {
        if (p_ == e_) {
            if (src_ && n >= 65536) {                       // big read: straight into the destination
                if (next_ > end_ || n > end_ - next_) damaged("read past the end of the patch");
                if (!src_->read(next_, d, n)) fail(Error::Io, src_->io_message());
                next_ += n;
                return;
            }
            refill();
        }
        const size_t take = std::min<size_t>(n, (size_t)(e_ - p_));
        std::memcpy(d, p_, take);
        p_ += take; d += take; n -= take;
    }
}

void Cursor::skip(uint64_t n) {
    const uint64_t avail = (uint64_t)(e_ - p_);
    if (n <= avail) { p_ += n; return; }
    n -= avail;
    p_ = e_;
    if (!src_ || next_ > end_ || n > end_ - next_) damaged("skip past the end of the patch");
    next_ += n;
}

uint32_t Cursor::le32() {
    const uint32_t a = u8(), b = u8(), c = u8(), d = u8();
    return a | (b << 8) | (c << 16) | (d << 24);
}

uint32_t Cursor::be32() {
    const uint32_t a = u8(), b = u8(), c = u8(), d = u8();
    return (a << 24) | (b << 16) | (c << 8) | d;
}

uint64_t Cursor::vlq() {
    uint64_t data = 0, shift = 1;
    for (;;) {
        const uint8_t x = u8();
        const uint64_t part = x & 0x7F;
        if (part && shift > UINT64_MAX / part) damaged("number too large");
        const uint64_t add = part * shift;
        if (data > UINT64_MAX - add) damaged("number too large");
        data += add;
        if (x & 0x80) return data;
        if (shift > (UINT64_MAX >> 7)) damaged("number too large");
        shift <<= 7;
        if (data > UINT64_MAX - shift) damaged("number too large");
        data += shift;
    }
}

uint64_t Cursor::varint() {
    uint64_t v = 0;
    for (int i = 0; i < 10; ++i) {
        const uint8_t b = u8();
        if (v > (UINT64_MAX >> 7)) damaged("number too large");
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) return v;
    }
    damaged("number too long");
}

constexpr size_t kEmitBuf = 1u << 20;

Emitter::Emitter(Job& j) : j_(j) { buf_.reserve(kEmitBuf); }

void Emitter::room(uint64_t n) {
    if (n > j_.max_out || total_ > j_.max_out - n) fail(Error::TooBig, kMsgTooBig, "limit " + std::to_string(j_.max_out) + " bytes");
}

void Emitter::flush() {
    if (buf_.empty()) return;
    if (!j_.out->write(flushed_, buf_.data(), buf_.size())) fail(Error::Io, j_.out->io_message());
    flushed_ += buf_.size();
    buf_.clear();
    if (expect_) j_.report((double)flushed_ / (double)expect_);
}

void Emitter::put(const void* p, size_t n) {
    if (!n) return;
    room(n);
    crc_ = crc32(static_cast<const uint8_t*>(p), n, crc_);
    if (n >= kEmitBuf) {
        flush();
        if (!j_.out->write(flushed_, p, n)) fail(Error::Io, j_.out->io_message());
        flushed_ += n;
        total_ += n;
        if (expect_) j_.report((double)flushed_ / (double)expect_);
        return;
    }
    if (buf_.size() + n > kEmitBuf) flush();
    const uint8_t* b = static_cast<const uint8_t*>(p);
    buf_.insert(buf_.end(), b, b + n);
    total_ += n;
}

void Emitter::read_back(uint64_t off, void* dst, size_t n) {
    if (off > total_ || n > total_ - off) damaged("copy from output that does not exist yet");
    uint8_t* d = static_cast<uint8_t*>(dst);
    if (off < flushed_) {
        const size_t k = (size_t)std::min<uint64_t>(n, flushed_ - off);
        if (!j_.out->read(off, d, k)) fail(Error::Io, j_.out->io_message());
        d += k; off += k; n -= k;
    }
    if (n) std::memcpy(d, buf_.data() + (off - flushed_), n);
}

void Emitter::finish() { flush(); }

static uint32_t crc32_range(Job& j, Source& s, uint64_t n) {
    std::vector<uint8_t> buf((size_t)std::min<uint64_t>(n, 1u << 20));
    uint32_t c = 0;
    uint64_t done = 0;
    while (done < n) {
        const size_t k = (size_t)std::min<uint64_t>(buf.size(), n - done);
        if (!s.read(done, buf.data(), k)) fail(Error::Io, s.io_message());
        c = crc32(buf.data(), k, c);
        done += k;
        j.report((double)done / (double)n);
    }
    return c;
}

uint32_t crc32_source(Job& j, Source& s) { return crc32_range(j, s, s.size()); }
uint32_t crc32_patch_prefix(Job& j, uint64_t n) { return crc32_range(j, *j.patch, n); }

/* The game as a stream that can be copied into the output; bytes past its end read as zero (UPS wants that). */
class GameCopier {
public:
    explicit GameCopier(Source& g) : g_(g), size_(g.size()) {}
    void emit(Emitter& e, uint64_t from, uint64_t n) {
        const size_t want = (size_t)std::min<uint64_t>(n, kChunk);
        if (buf_.size() < want) buf_.resize(want);
        while (n) {
            const size_t k = (size_t)std::min<uint64_t>(buf_.size(), n);
            size_t real = 0;
            if (from < size_) real = (size_t)std::min<uint64_t>(k, size_ - from);
            if (real && !g_.read(from, buf_.data(), real)) fail(Error::Io, g_.io_message());
            if (real < k) std::memset(buf_.data() + real, 0, k - real);
            e.put(buf_.data(), k);
            from += k;
            n -= k;
        }
    }
    uint8_t at(uint64_t pos) {          // one byte of the game (zero past its end), through a small window
        if (pos >= size_) return 0;
        if (pos < win_off_ || pos >= win_off_ + win_len_) {
            if (win_.size() < kWindow) win_.resize(kWindow);
            win_off_ = pos;
            win_len_ = (size_t)std::min<uint64_t>(win_.size(), size_ - pos);
            if (!g_.read(win_off_, win_.data(), win_len_)) { win_len_ = 0; fail(Error::Io, g_.io_message()); }
        }
        return win_[(size_t)(pos - win_off_)];
    }
private:
    static constexpr uint64_t kChunk = 1u << 20;
    static constexpr size_t kWindow = 1u << 16;
    Source& g_;
    uint64_t size_;
    std::vector<uint8_t> buf_, win_;
    uint64_t win_off_ = 0;
    size_t win_len_ = 0;
};

/* The checks before the first byte is written, shared by UPS and BPS: the patch's own CRC, then the game's size
 * and CRC. The 12 bytes at the end are source CRC, target CRC, patch CRC (little endian). */
struct Footer { uint32_t src_crc, tgt_crc, pat_crc; };

static Footer read_footer(Source& p) {
    uint8_t f[12];
    if (!p.read(p.size() - 12, f, 12)) fail(Error::Io, p.io_message());
    auto le = [](const uint8_t* q) { return (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24); };
    return Footer{le(f), le(f + 4), le(f + 8)};
}

static std::string hex8(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%08X", v);
    return b;
}

static void check_patch_crc(Job& j, const Footer& f) {
    j.phase(0.0, 0.05);
    const uint32_t got = crc32_patch_prefix(j, j.patch->size() - 4);
    if (got != f.pat_crc) damaged("patch CRC " + hex8(got) + ", footer says " + hex8(f.pat_crc));
}

static void check_game(Job& j, const Footer& f, uint64_t want_size, uint64_t result_size) {
    Source& g = *j.game;
    const bool maybe_done = g.size() == result_size;
    if (g.size() != want_size && !maybe_done)
        fail(Error::WrongSource, kMsgWrongGame, "game is " + std::to_string(g.size()) + " bytes, patch wants " + std::to_string(want_size));
    j.phase(0.05, 0.35);
    const uint32_t got = crc32_source(j, g);
    if (g.size() == want_size && got == f.src_crc) return;
    if (maybe_done && got == f.tgt_crc) fail(Error::WrongSource, kMsgAlreadyPatched, "the game already matches the patch's result");
    fail(Error::WrongSource, kMsgWrongGame, "game CRC " + hex8(got) + ", patch wants " + hex8(f.src_crc));
}

static void check_output(Emitter& e, const Footer& f) {
    if (e.crc() != f.tgt_crc) fail(Error::BadOutput, kMsgBadOutput, "result CRC " + hex8(e.crc()) + ", patch says " + hex8(f.tgt_crc));
}

/* ======================================================================================== IPS */

static void run_ips(Job& j) {
    Source& P = *j.patch;
    Sink& O = *j.out;
    Cursor c(P, 0, P.size());
    uint8_t magic[5];
    c.read(magic, 5);
    if (std::memcmp(magic, "PATCH", 5) != 0) damaged("not an IPS patch");

    /* The result starts as a copy of the game; the records are written over it. */
    const uint64_t gsz = j.game->size();
    if (gsz > j.max_out) fail(Error::TooBig, kMsgTooBig);
    j.phase(0.0, 0.85);
    {
        std::vector<uint8_t> buf((size_t)std::min<uint64_t>(gsz, 1u << 20));
        uint64_t done = 0;
        while (done < gsz) {
            const size_t k = (size_t)std::min<uint64_t>(buf.size(), gsz - done);
            if (!j.game->read(done, buf.data(), k)) fail(Error::Io, j.game->io_message());
            if (!O.write(done, buf.data(), k)) fail(Error::Io, O.io_message());
            done += k;
            j.report((double)done / (double)gsz);
        }
    }

    j.phase(0.85, 0.15);
    const uint64_t psz = P.size();
    auto be24 = [&]() { const uint32_t a = c.u8(), b = c.u8(), d = c.u8(); return (a << 16) | (b << 8) | d; };
    auto be16 = [&]() { const uint32_t a = c.u8(), b = c.u8(); return (a << 8) | b; };
    std::vector<uint8_t> tmp;
    for (;;) {
        if (c.remaining() < 3) damaged("IPS: no EOF marker");
        const uint32_t off = be24();
        if (off == 0x454F46) {                              // "EOF"
            const uint64_t rem = c.remaining();
            if (rem == 0) break;
            if (rem == 3) {                                 // the optional truncation size
                const uint32_t size = be24();
                if (size > j.max_out) fail(Error::TooBig, kMsgTooBig);
                if (!O.resize(size)) fail(Error::Io, O.io_message());
                break;
            }
            if (rem < 4) damaged("IPS: bad ending");
            /* otherwise a real record that happens to sit at offset 0x454F46 */
        }
        const uint32_t size = be16();
        if (size == 0) {                                    // RLE record
            const uint32_t run = be16();
            const uint8_t value = c.u8();
            if (run == 0) continue;
            if ((uint64_t)off + run > j.max_out) fail(Error::TooBig, kMsgTooBig);
            tmp.assign(run, value);
            if (!O.write(off, tmp.data(), run)) fail(Error::Io, O.io_message());
        } else {
            if ((uint64_t)off + size > j.max_out) fail(Error::TooBig, kMsgTooBig);
            tmp.resize(size);
            c.read(tmp.data(), size);
            if (!O.write(off, tmp.data(), size)) fail(Error::Io, O.io_message());
        }
        j.report((double)c.pos() / (double)psz);
    }
}

/* ======================================================================================== UPS */

static void run_ups(Job& j) {
    Source& P = *j.patch;
    const uint64_t psz = P.size();
    if (psz < 4 + 2 + 12) damaged("UPS: too short");
    const Footer foot = read_footer(P);
    check_patch_crc(j, foot);

    Cursor c(P, 0, psz - 12);
    uint8_t magic[4];
    c.read(magic, 4);
    if (std::memcmp(magic, "UPS1", 4) != 0) damaged("not a UPS patch");
    const uint64_t src_size = c.vlq();
    const uint64_t tgt_size = c.vlq();

    check_game(j, foot, src_size, tgt_size);
    if (tgt_size > j.max_out) fail(Error::TooBig, kMsgTooBig);

    j.phase(0.40, 0.60);
    Emitter e(j);
    e.expect(tgt_size);
    GameCopier game(*j.game);
    uint64_t pos = 0;                                       // output offset == offset in the game
    while (c.remaining() > 0) {
        const uint64_t skip = c.vlq();                      // bytes that stay as they are
        if (skip > tgt_size - pos) damaged("UPS: skip past the end of the result");
        game.emit(e, pos, skip);
        pos += skip;
        for (;;) {                                          // XOR bytes up to a zero (the zero is an unchanged byte)
            if (c.remaining() == 0) damaged("UPS: block without its end byte");
            const uint8_t x = c.u8();
            if (pos >= tgt_size) {                          // only the end byte may sit one past the result
                if (x != 0) damaged("UPS: block past the end of the result");
                break;
            }
            const uint8_t b = (uint8_t)(game.at(pos) ^ x);
            e.put(&b, 1);
            ++pos;
            if (x == 0) break;
        }
    }
    game.emit(e, pos, tgt_size - pos);                      // the rest is unchanged
    e.finish();
    check_output(e, foot);
}

/* ======================================================================================== BPS */

static void run_bps(Job& j) {
    Source& P = *j.patch;
    Source& G = *j.game;
    const uint64_t psz = P.size();
    if (psz < 4 + 3 + 12) damaged("BPS: too short");
    const Footer foot = read_footer(P);
    check_patch_crc(j, foot);

    Cursor c(P, 0, psz - 12);
    uint8_t magic[4];
    c.read(magic, 4);
    if (std::memcmp(magic, "BPS1", 4) != 0) damaged("not a BPS patch");
    const uint64_t src_size = c.vlq();
    const uint64_t tgt_size = c.vlq();
    const uint64_t meta_size = c.vlq();
    c.skip(meta_size);

    check_game(j, foot, src_size, tgt_size);
    if (tgt_size > j.max_out) fail(Error::TooBig, kMsgTooBig);

    j.phase(0.40, 0.60);
    Emitter e(j);
    e.expect(tgt_size);
    GameCopier game(G);
    const uint64_t gsz = G.size();
    constexpr uint64_t kChunk = 1u << 20;
    std::vector<uint8_t> tmp;
    uint64_t out = 0, src_rel = 0, tgt_rel = 0;
    while (out < tgt_size) {
        const uint64_t data = c.vlq();
        const unsigned cmd = (unsigned)(data & 3);
        const uint64_t len = (data >> 2) + 1;
        if (len > tgt_size - out) damaged("BPS: action runs past the end of the result");
        switch (cmd) {
            case 0:                                         // SourceRead: the game at the same offset
                if (out > gsz || len > gsz - out) damaged("BPS: SourceRead past the end of the game");
                game.emit(e, out, len);
                break;
            case 1: {                                       // TargetRead: bytes carried by the patch
                if (tmp.size() < std::min(len, kChunk)) tmp.resize((size_t)std::min(len, kChunk));
                for (uint64_t left = len; left;) {
                    const size_t k = (size_t)std::min<uint64_t>(left, tmp.size());
                    c.read(tmp.data(), k);
                    e.put(tmp.data(), k);
                    left -= k;
                }
                break;
            }
            case 2: {                                       // SourceCopy: the game at a relative offset
                const uint64_t d = c.vlq(), off = d >> 1;
                if (d & 1) { if (off > src_rel) damaged("BPS: SourceCopy before the start"); src_rel -= off; }
                else { if (off > UINT64_MAX - src_rel) damaged("BPS: SourceCopy offset overflow"); src_rel += off; }
                if (src_rel > gsz || len > gsz - src_rel) damaged("BPS: SourceCopy past the end of the game");
                game.emit(e, src_rel, len);
                src_rel += len;
                break;
            }
            default: {                                      // TargetCopy: earlier output, may overlap what it writes
                const uint64_t d = c.vlq(), off = d >> 1;
                if (d & 1) { if (off > tgt_rel) damaged("BPS: TargetCopy before the start"); tgt_rel -= off; }
                else { if (off > UINT64_MAX - tgt_rel) damaged("BPS: TargetCopy offset overflow"); tgt_rel += off; }
                if (tgt_rel >= out) damaged("BPS: TargetCopy from output that does not exist yet");
                const uint64_t distance = out - tgt_rel;
                if (tmp.size() < std::min(len, kChunk)) tmp.resize((size_t)std::min(len, kChunk));
                uint64_t from = tgt_rel;
                for (uint64_t left = len; left;) {
                    const uint64_t k = std::min<uint64_t>({left, distance, (uint64_t)tmp.size()});
                    e.read_back(from, tmp.data(), (size_t)k);
                    e.put(tmp.data(), (size_t)k);
                    from += k;
                    left -= k;
                }
                tgt_rel += len;
                break;
            }
        }
        out += len;
    }
    if (c.remaining() != 0) damaged("BPS: unused data before the footer");
    e.finish();
    check_output(e, foot);
}

/* ======================================================================================== dispatch */

static void run_patch(Job& j) {
    uint8_t head[8] = {};
    const size_t hn = (size_t)std::min<uint64_t>(j.patch->size(), sizeof head);
    if (hn && !j.patch->read(0, head, hn)) fail(Error::Io, j.patch->io_message());
    switch (detect(head, hn)) {
        case Format::Ips: run_ips(j); break;
        case Format::Ups: run_ups(j); break;
        case Format::Bps: run_bps(j); break;
        case Format::Vcdiff: run_vcdiff(j); break;
        default:
            if (is_ppf(head, hn)) fail(Error::Unsupported, kMsgPpf, "PPF");
            fail(Error::BadPatch, kMsgNotPatch, hn ? "unknown patch format" : "empty patch");
    }
}

/* Called from a catch block: the Result for whatever is being handled. Nothing escapes the API. */
static Result result_from_current_exception() {
    Result r;
    try {
        throw;
    } catch (const Fail& f) {
        r.error = f.error; r.message = f.message; r.detail = f.detail;
    } catch (const std::bad_alloc&) {
        r.error = Error::TooBig; r.message = kMsgNoMemory; r.detail = "out of memory";
    } catch (const std::length_error&) {
        r.error = Error::TooBig; r.message = kMsgNoMemory; r.detail = "allocation too large";
    } catch (const std::exception& ex) {
        r.error = Error::BadPatch; r.message = kMsgDamaged; r.detail = ex.what();
    } catch (...) {
        r.error = Error::BadPatch; r.message = kMsgDamaged; r.detail = "unexpected failure";
    }
    return r;
}

static Result run_job(Job& j) {
    try {
        j.report(0.0);
        run_patch(j);
        return Result();
    } catch (...) {
        return result_from_current_exception();
    }
}

}  // namespace detail

using namespace detail;

/* ======================================================================================== inspect */

/* head / hn: the first bytes of the patch; tail / tn: its last bytes (at most 16); total: the whole size. */
static bool inspect_parts(const uint8_t* head, size_t hn, const uint8_t* tail, size_t tn, uint64_t total,
                          Info* out, std::string* err) {
    Info info;
    try {
        info.format = detect(head, hn);
        switch (info.format) {
            case Format::Unknown:
                fail(is_ppf(head, hn) ? Error::Unsupported : Error::BadPatch, is_ppf(head, hn) ? kMsgPpf : kMsgNotPatch);
            case Format::Ips:
                if (total < 8) damaged("IPS: too short");
                break;
            case Format::Ups:
            case Format::Bps: {
                const bool bps = info.format == Format::Bps;
                if (total < (uint64_t)(bps ? 4 + 3 + 12 : 4 + 2 + 12) || tn < 12) damaged("too short");
                Cursor c(head, hn);
                c.skip(4);
                info.source_size = c.vlq();
                info.target_size = c.vlq();
                if (bps) {
                    const uint64_t meta = c.vlq();
                    const uint64_t avail = c.remaining();
                    const bool whole = hn == total;
                    if (meta > avail && whole) damaged("BPS: metadata past the end");
                    const size_t take = (size_t)std::min(meta, avail);
                    info.metadata.resize(take);
                    c.read(&info.metadata[0], take);
                }
                const uint8_t* f = tail + (tn - 12);
                auto le = [](const uint8_t* q) { return (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24); };
                info.has_source_crc = info.has_target_crc = true;
                info.source_crc = le(f);
                info.target_crc = le(f + 4);
                break;
            }
            case Format::Vcdiff:
                inspect_vcdiff(head, hn, &info);
                break;
        }
    } catch (const Fail& f) {
        if (err) *err = f.message;
        return false;
    } catch (...) {
        if (err) *err = kMsgDamaged;
        return false;
    }
    if (out) *out = std::move(info);
    return true;
}

bool inspect(const uint8_t* patch, size_t n, Info* out, std::string* err) {
    if (!patch && n) {
        if (err) *err = kMsgNotPatch;
        return false;
    }
    const size_t tn = std::min<size_t>(n, 16);
    return inspect_parts(patch, n, patch + (n - tn), tn, n, out, err);
}

/* ======================================================================================== apply (memory) */

Result apply(const std::vector<uint8_t>& source, const uint8_t* patch, size_t n,
             std::vector<uint8_t>* out, uint64_t max_out) {
    std::vector<uint8_t> result;           // never `out` itself: it may be the same vector as `source`
    Result r;
    if (!patch && n) {
        r.error = Error::BadPatch; r.message = kMsgNotPatch;
    } else {
        MemSource game(source.data(), source.size(), kMsgReadGame);
        MemSource pat(patch, n, kMsgReadPatch);
        MemSink sink(result);
        Job j;
        j.game = &game; j.patch = &pat; j.out = &sink;
        j.max_out = max_out ? max_out : UINT64_MAX;
        r = run_job(j);
    }
    if (out) {
        if (r.ok()) *out = std::move(result);
        else out->clear();
    }
    return r;
}

/* ======================================================================================== files */

namespace {

constexpr const char* kMsgOpenPatch = "The patch file couldn't be opened.";
constexpr const char* kMsgOpenGame = "The game file couldn't be opened.";
constexpr const char* kMsgCreateOut = "The patched game couldn't be created.";
constexpr const char* kMsgSameFile = "The patched game can't be saved over the original game or the patch.";
constexpr const char* kMsgReplace = "The patched game couldn't replace the old file. Close the game and try again.";

/* UTF-8 path to a wide path Windows accepts, with the \\?\ prefix for long absolute paths. */
std::wstring wide_path(const std::string& utf8) {
    std::wstring w = winutil::to_wide(utf8);
    for (wchar_t& ch : w) {
        if (ch == L'/') ch = L'\\';
    }
    if (w.size() >= 240 && w.compare(0, 4, L"\\\\?\\") != 0) {
        if (w.size() >= 3 && std::iswalpha(w[0]) && w[1] == L':' && w[2] == L'\\') w = L"\\\\?\\" + w;
        else if (w.compare(0, 2, L"\\\\") == 0) w = L"\\\\?\\UNC\\" + w.substr(2);
    }
    return w;
}

struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE x) : h(x) {}
    Handle(Handle&& o) noexcept : h(o.h) { o.h = INVALID_HANDLE_VALUE; }
    Handle& operator=(Handle&& o) noexcept {
        if (this != &o) { close(); h = o.h; o.h = INVALID_HANDLE_VALUE; }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { close(); }
    void close() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
    bool ok() const { return h != INVALID_HANDLE_VALUE; }
};

Handle open_read(const std::wstring& path, DWORD flags = FILE_ATTRIBUTE_NORMAL) {
    return Handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, flags, nullptr));
}

bool file_size(HANDLE h, uint64_t* out) {
    LARGE_INTEGER li;
    if (!GetFileSizeEx(h, &li) || li.QuadPart < 0) return false;
    *out = (uint64_t)li.QuadPart;
    return true;
}

/* Positioned read / write on a handle that was not opened for overlapped I/O (the offset comes from OVERLAPPED). */
bool pread(HANDLE h, uint64_t off, void* dst, size_t n) {
    uint8_t* d = static_cast<uint8_t*>(dst);
    while (n) {
        const DWORD want = (DWORD)std::min<size_t>(n, 1u << 30);
        OVERLAPPED ov = {};
        ov.Offset = (DWORD)(off & 0xFFFFFFFFu);
        ov.OffsetHigh = (DWORD)(off >> 32);
        DWORD got = 0;
        if (!ReadFile(h, d, want, &got, &ov) || got == 0) return false;
        d += got; off += got; n -= got;
    }
    return true;
}

bool pwrite(HANDLE h, uint64_t off, const void* src, size_t n, bool* disk_full) {
    const uint8_t* s = static_cast<const uint8_t*>(src);
    while (n) {
        const DWORD want = (DWORD)std::min<size_t>(n, 1u << 30);
        OVERLAPPED ov = {};
        ov.Offset = (DWORD)(off & 0xFFFFFFFFu);
        ov.OffsetHigh = (DWORD)(off >> 32);
        DWORD put = 0;
        if (!WriteFile(h, s, want, &put, &ov) || put == 0) {
            const DWORD e = GetLastError();
            if (disk_full && (e == ERROR_DISK_FULL || e == ERROR_HANDLE_DISK_FULL)) *disk_full = true;
            return false;
        }
        s += put; off += put; n -= put;
    }
    return true;
}

bool set_end(HANDLE h, uint64_t size) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)size;
    return SetFilePointerEx(h, li, nullptr, FILE_BEGIN) && SetEndOfFile(h);
}

/* A file read on demand: small reads go through an 8 x 256 KB block cache, big ones straight to the caller's buffer. */
class FileSource : public Source {
public:
    FileSource(HANDLE h, uint64_t size, const char* io_msg) : h_(h), size_(size), io_msg_(io_msg) {}
    uint64_t size() const override { return size_; }
    const char* io_message() const override { return io_msg_; }
    bool read(uint64_t off, void* dst, size_t n) override {
        if (n == 0) return true;
        if (off > size_ || n > size_ - off) return false;
        if (n >= kDirect) return pread(h_, off, dst, n);
        uint8_t* d = static_cast<uint8_t*>(dst);
        while (n) {
            const uint64_t index = off / kBlock;
            Block* b = block(index);
            if (!b) return false;
            const size_t rel = (size_t)(off - index * kBlock);
            if (rel >= b->len) return false;
            const size_t take = std::min(n, b->len - rel);
            std::memcpy(d, b->data.data() + rel, take);
            d += take; off += take; n -= take;
        }
        return true;
    }
private:
    static constexpr size_t kBlock = 256u << 10, kDirect = 128u << 10, kBlocks = 8;
    struct Block {
        uint64_t index = UINT64_MAX;
        std::vector<uint8_t> data;
        size_t len = 0;
        uint64_t used = 0;
    };
    Block* block(uint64_t index) {
        Block* victim = &blocks_[0];
        for (Block& b : blocks_) {
            if (b.index == index) { b.used = ++tick_; return &b; }
            if (b.used < victim->used) victim = &b;
        }
        const uint64_t start = index * kBlock;
        if (start >= size_) return nullptr;
        const size_t len = (size_t)std::min<uint64_t>(kBlock, size_ - start);
        if (victim->data.size() < kBlock) victim->data.resize(kBlock);
        victim->index = UINT64_MAX;
        if (!pread(h_, start, victim->data.data(), len)) return nullptr;
        victim->index = index;
        victim->len = len;
        victim->used = ++tick_;
        return victim;
    }
    HANDLE h_;
    uint64_t size_;
    const char* io_msg_;
    Block blocks_[kBlocks];
    uint64_t tick_ = 0;
};

/* The output file: sequential appends and the occasional small in-place write (IPS), with a write-combining buffer
 * and read-back of what it holds. */
class FileSink : public Sink {
public:
    explicit FileSink(HANDLE h) : h_(h) {}
    uint64_t size() const override {
        const uint64_t buffered = buf_.empty() ? 0 : buf_off_ + buf_.size();
        return std::max(disk_size_, buffered);
    }
    const char* io_message() const override { return disk_full_ ? kMsgDiskFull : kMsgWrite; }
    bool write(uint64_t off, const void* src, size_t n) override {
        if (n == 0) return true;
        const uint8_t* s = static_cast<const uint8_t*>(src);
        if (!buf_.empty() && off >= buf_off_ && off <= buf_off_ + buf_.size() && off - buf_off_ + n <= kCoalesce) {
            const size_t rel = (size_t)(off - buf_off_);
            if (rel + n > buf_.size()) buf_.resize(rel + n);
            std::memcpy(buf_.data() + rel, s, n);
            return true;
        }
        if (!flush()) return false;
        if (off > disk_size_ && !resize(off)) return false;           // the gap reads as zeros
        if (n >= kDirect) return raw_write(off, s, n);
        buf_off_ = off;
        buf_.assign(s, s + n);
        return true;
    }
    bool read(uint64_t off, void* dst, size_t n) override {
        if (off > size() || n > size() - off) return false;
        uint8_t* d = static_cast<uint8_t*>(dst);
        const uint64_t disk_end = std::min<uint64_t>(off + n, disk_size_);
        if (off < disk_end && !pread(h_, off, d, (size_t)(disk_end - off))) return false;
        if (!buf_.empty()) {
            const uint64_t lo = std::max<uint64_t>(off, buf_off_), hi = std::min<uint64_t>(off + n, buf_off_ + buf_.size());
            if (lo < hi) std::memcpy(d + (lo - off), buf_.data() + (lo - buf_off_), (size_t)(hi - lo));
        }
        return true;
    }
    bool resize(uint64_t n) override {
        if (!flush()) return false;
        if (!set_end(h_, n)) return false;
        disk_size_ = n;
        return true;
    }
    bool flush() {
        if (buf_.empty()) return true;
        const bool ok = raw_write(buf_off_, buf_.data(), buf_.size());
        buf_.clear();
        return ok;
    }
private:
    bool raw_write(uint64_t off, const uint8_t* s, size_t n) {
        if (!pwrite(h_, off, s, n, &disk_full_)) return false;
        disk_size_ = std::max<uint64_t>(disk_size_, off + n);
        return true;
    }
    static constexpr size_t kCoalesce = 256u << 10, kDirect = 64u << 10;
    HANDLE h_;
    uint64_t disk_size_ = 0;
    std::vector<uint8_t> buf_;
    uint64_t buf_off_ = 0;
    bool disk_full_ = false;
};

/* Does `path` name the same file as the open handle? (false when it does not exist) */
bool is_same_file(HANDLE open, const std::wstring& path) {
    Handle other(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!other.ok()) return false;
    BY_HANDLE_FILE_INFORMATION a, b;
    if (!GetFileInformationByHandle(open, &a) || !GetFileInformationByHandle(other.h, &b)) return false;
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
}

bool make_dirs(const std::wstring& dir) {
    if (dir.empty()) return true;
    const DWORD a = GetFileAttributesW(dir.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const size_t cut = dir.find_last_of(L'\\');
    if (cut != std::wstring::npos && cut > 0 && dir[cut - 1] != L':') make_dirs(dir.substr(0, cut));
    return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::string win_error(const char* what) { return std::string(what) + ": Windows error " + std::to_string(GetLastError()); }

}  // namespace

Result apply_file(const std::string& source_path, const std::string& patch_path, const std::string& out_path,
                  uint64_t max_out, const std::function<bool(double)>& progress) {
    Result r;
    const std::wstring wout = wide_path(out_path), wtmp = wout + L".tmp";
    bool tmp_made = false, committed = false;
    Handle pf, gf, of;
    try {
        pf = open_read(wide_path(patch_path), FILE_FLAG_SEQUENTIAL_SCAN);
        if (!pf.ok()) fail(Error::Io, kMsgOpenPatch, win_error("open patch"));
        gf = open_read(wide_path(source_path));
        if (!gf.ok()) fail(Error::Io, kMsgOpenGame, win_error("open game"));
        uint64_t psz = 0, gsz = 0;
        if (!file_size(pf.h, &psz)) fail(Error::Io, kMsgReadPatch, win_error("size of patch"));
        if (!file_size(gf.h, &gsz)) fail(Error::Io, kMsgReadGame, win_error("size of game"));
        if (is_same_file(gf.h, wout) || is_same_file(pf.h, wout)) fail(Error::Io, kMsgSameFile);

        const size_t cut = wout.find_last_of(L'\\');
        if (cut != std::wstring::npos && cut > 0 && !make_dirs(wout.substr(0, cut))) fail(Error::Io, kMsgCreateOut, win_error("create folder"));
        of = Handle(CreateFileW(wtmp.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!of.ok()) fail(Error::Io, kMsgCreateOut, win_error("create output"));
        tmp_made = true;

        FileSource patch(pf.h, psz, kMsgReadPatch);
        FileSource game(gf.h, gsz, kMsgReadGame);
        FileSink sink(of.h);
        Job j;
        j.game = &game; j.patch = &patch; j.out = &sink;
        j.max_out = max_out ? max_out : UINT64_MAX;
        j.progress = progress;
        j.report(0.0);
        run_patch(j);

        if (!sink.flush()) fail(Error::Io, sink.io_message(), win_error("write"));
        if (!FlushFileBuffers(of.h)) fail(Error::Io, kMsgWrite, win_error("flush"));
        of.close(); pf.close(); gf.close();
        /* A virus scanner may still hold the new file for a moment: retry a few times for a second at most. */
        bool moved = false;
        for (int attempt = 0; attempt < 10 && !moved; ++attempt) {
            moved = MoveFileExW(wtmp.c_str(), wout.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
            if (moved) break;
            const DWORD err = GetLastError();
            if (err != ERROR_ACCESS_DENIED && err != ERROR_SHARING_VIOLATION && err != ERROR_LOCK_VIOLATION) break;
            Sleep(100);
        }
        if (!moved) fail(Error::Io, kMsgReplace, win_error("rename"));
        committed = true;
    } catch (...) {
        r = result_from_current_exception();
    }
    of.close(); pf.close(); gf.close();
    if (tmp_made && !committed) DeleteFileW(wtmp.c_str());
    if (committed && progress) {
        try { progress(1.0); } catch (...) {}
    }
    return r;
}

bool crc32_file(const std::string& path, uint32_t* out, std::string* err, const std::function<bool(double)>& progress) {
    try {
        Handle h = open_read(wide_path(path), FILE_FLAG_SEQUENTIAL_SCAN);
        if (!h.ok()) { if (err) *err = kMsgOpenGame; return false; }
        uint64_t size = 0;
        if (!file_size(h.h, &size)) { if (err) *err = kMsgReadGame; return false; }
        std::vector<uint8_t> buf((size_t)std::min<uint64_t>(std::max<uint64_t>(size, 1), 1u << 20));
        uint32_t c = 0;
        uint64_t done = 0;
        while (done < size) {
            const size_t k = (size_t)std::min<uint64_t>(buf.size(), size - done);
            if (!pread(h.h, done, buf.data(), k)) { if (err) *err = kMsgReadGame; return false; }
            c = crc32(buf.data(), k, c);
            done += k;
            if (progress && !progress((double)done / (double)size)) { if (err) *err = kMsgCancelled; return false; }
        }
        if (out) *out = c;
        return true;
    } catch (...) {
        if (err) *err = kMsgReadGame;
        return false;
    }
}

bool inspect_file(const std::string& patch_path, Info* out, std::string* err) {
    try {
        Handle h = open_read(wide_path(patch_path));
        if (!h.ok()) { if (err) *err = kMsgOpenPatch; return false; }
        uint64_t size = 0;
        if (!file_size(h.h, &size)) { if (err) *err = kMsgReadPatch; return false; }
        if (size == 0) { if (err) *err = kMsgNotPatch; return false; }
        const size_t hn = (size_t)std::min<uint64_t>(size, 64u << 10);       // header + metadata / application header
        const size_t tn = (size_t)std::min<uint64_t>(size, 16);
        std::vector<uint8_t> head(hn);
        uint8_t tail[16];
        if (!pread(h.h, 0, head.data(), hn) || !pread(h.h, size - tn, tail, tn)) { if (err) *err = kMsgReadPatch; return false; }
        return inspect_parts(head.data(), hn, tail, tn, size, out, err);
    } catch (...) {
        if (err) *err = kMsgReadPatch;
        return false;
    }
}

}  // namespace hub::patch
