#pragma once
/* Internal pieces shared by patcher.cpp (IPS / UPS / BPS, files, CRC) and patcher_vcdiff.cpp (VCDIFF).
 * Not part of the API: include "patcher.h" instead. No Windows headers in here (patcher_vcdiff.cpp also pulls in
 * the LZMA SDK, whose headers define True / False macros). */

#include "patcher.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace hub::patch::detail {

/* ---- what the player sees (Result::message) ---- */
inline constexpr const char* kMsgDamaged = "This patch file is damaged or incomplete.";
inline constexpr const char* kMsgNotPatch = "This file isn't a patch the hub can read.";
inline constexpr const char* kMsgWrongGame = "This patch is for a different version of the game.";
inline constexpr const char* kMsgAlreadyPatched = "This game already has this patch applied.";
inline constexpr const char* kMsgBadOutput = "The patched game didn't pass its safety check, so it was thrown away.";
inline constexpr const char* kMsgTooBig = "The patched game would be larger than the hub allows.";
inline constexpr const char* kMsgNoMemory = "There isn't enough memory to apply this patch.";
inline constexpr const char* kMsgPpf = "PPF patches aren't supported yet.";
inline constexpr const char* kMsgUnsupported = "This patch uses a feature the hub can't read yet.";
inline constexpr const char* kMsgCancelled = "Patching was cancelled.";
inline constexpr const char* kMsgReadGame = "The game file couldn't be read.";
inline constexpr const char* kMsgReadPatch = "The patch file couldn't be read.";
inline constexpr const char* kMsgWrite = "The patched game couldn't be saved.";
inline constexpr const char* kMsgDiskFull = "There isn't enough free disk space to save the patched game.";

/* Thrown inside the engines, caught at the top of apply() / apply_file() / inspect(). Never escapes the API. */
struct Fail {
    Error error;
    std::string message;
    std::string detail;
};
[[noreturn]] inline void fail(Error e, const char* message, std::string detail = std::string()) {
    throw Fail{e, message, std::move(detail)};
}
[[noreturn]] inline void damaged(std::string detail = std::string()) { fail(Error::BadPatch, kMsgDamaged, std::move(detail)); }

/* ---- random-access bytes in: the game, or the patch ---- */
class Source {
public:
    virtual ~Source() = default;
    virtual uint64_t size() const = 0;
    /* Reads exactly n bytes at off. Callers guarantee off + n <= size(); false means an I/O error. */
    virtual bool read(uint64_t off, void* dst, size_t n) = 0;
    /* What to tell the player when read() fails. */
    virtual const char* io_message() const = 0;
};

class MemSource : public Source {
public:
    MemSource(const uint8_t* p, size_t n, const char* io_msg) : p_(p), n_(n), io_msg_(io_msg) {}
    uint64_t size() const override { return n_; }
    bool read(uint64_t off, void* dst, size_t n) override {
        if (off > n_ || n > n_ - off) return false;
        if (n) std::memcpy(dst, p_ + off, n);
        return true;
    }
    const char* io_message() const override { return io_msg_; }
private:
    const uint8_t* p_;
    size_t n_;
    const char* io_msg_;
};

/* ---- bytes out: the patched game ---- */
class Sink {
public:
    virtual ~Sink() = default;
    virtual uint64_t size() const = 0;
    /* Writes at off, growing the output (the gap, if any, reads as zeros). */
    virtual bool write(uint64_t off, const void* src, size_t n) = 0;
    /* Reads back exactly n bytes at off (off + n <= size()). */
    virtual bool read(uint64_t off, void* dst, size_t n) = 0;
    /* Shrinks, or grows with zeros, to exactly n bytes. */
    virtual bool resize(uint64_t n) = 0;
    /* What to tell the player when write() / resize() fails. */
    virtual const char* io_message() const = 0;
};

class MemSink : public Sink {
public:
    explicit MemSink(std::vector<uint8_t>& v) : v_(v) {}
    uint64_t size() const override { return v_.size(); }
    bool write(uint64_t off, const void* src, size_t n) override {
        if (off > SIZE_MAX || n > SIZE_MAX - off) return false;
        if (off + n > v_.size()) v_.resize((size_t)(off + n));
        if (n) std::memcpy(v_.data() + off, src, n);
        return true;
    }
    bool read(uint64_t off, void* dst, size_t n) override {
        if (off > v_.size() || n > v_.size() - off) return false;
        if (n) std::memcpy(dst, v_.data() + off, n);
        return true;
    }
    bool resize(uint64_t n) override {
        if (n > SIZE_MAX) return false;
        v_.resize((size_t)n);
        return true;
    }
    const char* io_message() const override { return kMsgWrite; }
private:
    std::vector<uint8_t>& v_;
};

/* One patching run. The engines read job.game / job.patch and write job.out; progress goes through report(). */
struct Job {
    Source* game = nullptr;
    Source* patch = nullptr;
    Sink* out = nullptr;
    uint64_t max_out = UINT64_MAX;
    std::function<bool(double)> progress;   // may be empty

    /* The next stretch of work fills [base, base + span) of the 0..1 bar; report() then takes 0..1 within it. */
    void phase(double base, double span) { base_ = base; span_ = span; }
    /* Throws Fail{Cancelled} when the callback asks to stop. Cheap to call often. */
    void report(double fraction_of_phase);
private:
    double base_ = 0.0, span_ = 1.0, last_ = -1.0;
};

/* ---- bounds-checked sequential reader over [begin, end) of a Source, or over a block of memory ----
 * Every read that would pass `end` throws Fail{BadPatch}; a failing Source throws Fail{Io}. */
class Cursor {
public:
    Cursor(Source& s, uint64_t begin, uint64_t end) : src_(&s), next_(begin), end_(end) {}
    Cursor(const uint8_t* mem, size_t n) : p_(mem), e_(mem + n), next_(n), end_(n) {}

    uint64_t pos() const { return next_ - (uint64_t)(e_ - p_); }          // offset of the next unread byte
    uint64_t remaining() const { return end_ - pos(); }
    uint8_t u8() {
        if (p_ == e_) refill();
        return *p_++;
    }
    void read(void* dst, size_t n);
    void skip(uint64_t n);
    uint32_t le32();
    uint32_t be32();
    uint64_t vlq();       // byuu's variable-length number (UPS and BPS)
    uint64_t varint();    // RFC 3284 section 2 big-endian base-128 (VCDIFF)

private:
    void refill();
    Source* src_ = nullptr;
    std::vector<uint8_t> buf_;
    const uint8_t* p_ = nullptr;
    const uint8_t* e_ = nullptr;
    uint64_t next_ = 0;    // absolute offset of the first byte after the loaded block
    uint64_t end_ = 0;
};

/* ---- sequential output with a running CRC, the size cap, and read-back of what was already written ---- */
class Emitter {
public:
    explicit Emitter(Job& j);
    /* Total bytes the engine expects to write (for the progress bar); 0 = the engine reports progress itself. */
    void expect(uint64_t total) { expect_ = total; }
    uint64_t pos() const { return total_; }
    uint32_t crc() const { return crc_; }
    void put(const void* p, size_t n);
    /* Bytes [off, off + n) of what was already emitted (off + n <= pos()). */
    void read_back(uint64_t off, void* dst, size_t n);
    void finish();       // pushes everything to the sink
private:
    void room(uint64_t n);
    void flush();
    Job& j_;
    std::vector<uint8_t> buf_;
    uint64_t total_ = 0;      // emitted
    uint64_t flushed_ = 0;    // in the sink
    uint64_t expect_ = 0;
    uint32_t crc_ = 0;
};

/* CRC-32 of a whole Source in 1 MB reads, reporting progress through the current phase. */
uint32_t crc32_source(Job& j, Source& s);
/* CRC-32 of [0, n) of the patch (the "everything but the last 4 bytes" check of UPS and BPS). */
uint32_t crc32_patch_prefix(Job& j, uint64_t n);

/* ---- VCDIFF ---- */
struct VcdCode { uint8_t type1, size1, mode1, type2, size2, mode2; };   // types: 0 NOOP, 1 ADD, 2 RUN, 3 COPY
const VcdCode* vcd_default_table();                                      // the 256 entries of RFC 3284 section 5.6
void run_vcdiff(Job& j);
/* Header only: magic, indicators, application header. Throws Fail on a bad header. */
void inspect_vcdiff(const uint8_t* patch, size_t n, Info* out);

}  // namespace hub::patch::detail
