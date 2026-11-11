#include "dump_check.h"
/* Incomplete-dump detection (dump_check.h). Header-only reads, every offset bounds- and overflow-checked. */

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;

namespace hub::dumpcheck {
namespace {

uint32_t rd32(const uint8_t* d, size_t at) { return (uint32_t)d[at] | ((uint32_t)d[at + 1] << 8) | ((uint32_t)d[at + 2] << 16) | ((uint32_t)d[at + 3] << 24); }
uint64_t rd64(const uint8_t* d, size_t at) { return (uint64_t)rd32(d, at) | ((uint64_t)rd32(d, at + 4) << 32); }

constexpr uint64_t kSane = 1ull << 50;   // a declared size beyond 1 PiB is a malformed header, not a verdict

Result truncated(uint64_t have, uint64_t expected) {
    Result r;
    r.v = Verdict::Truncated;
    r.have = have;
    r.expected = expected;
    r.reason = "This copy looks cut short (" + format_gb(have) + " of " + format_gb(expected) + "). The copy may have been interrupted.";
    return r;
}

Result check_pfs0(const ReadFn& read, uint64_t fsz) {
    std::vector<uint8_t> h;
    if (!read(0, 16, &h) || h.size() < 16 || std::memcmp(h.data(), "PFS0", 4) != 0) return {};
    const uint32_t count = rd32(h.data(), 4), strsz = rd32(h.data(), 8);
    if (count == 0 || count > 0x1000) return {};   // real dumps have a few dozen entries; 4096 caps the table read at ~100 KB
    const uint64_t hdr = 16 + (uint64_t)count * 0x18 + strsz;
    std::vector<uint8_t> t;
    const size_t tl = (size_t)count * 0x18;
    if (!read(16, tl, &t) || t.size() < tl) return {};
    uint64_t end = hdr;
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t off = rd64(t.data(), (size_t)i * 0x18), sz = rd64(t.data(), (size_t)i * 0x18 + 8);
        if (off > kSane || sz > kSane) return {};
        end = std::max(end, hdr + off + sz);
    }
    if (fsz < end) return truncated(fsz, end);
    return {};
}

Result check_xci(const ReadFn& read, uint64_t fsz) {
    std::vector<uint8_t> h;
    if (!read(0x100, 0x20, &h) || h.size() < 0x1C || std::memcmp(h.data(), "HEAD", 4) != 0) return {};
    const uint32_t valid_end = rd32(h.data(), 0x18);   // file offset 0x118: last valid 0x200-byte unit of the cartridge image
    if (valid_end == 0 || valid_end == 0xFFFFFFFFu) return {};
    const uint64_t expected = ((uint64_t)valid_end + 1) * 0x200;
    if (fsz < expected) return truncated(fsz, expected);
    return {};
}

Result check_nup(const ReadFn& read, uint64_t fsz) {
    std::vector<uint8_t> h;
    if (!read(0, 0x80, &h) || h.size() < 0x80 || std::memcmp(h.data(), "NUP\x01", 4) != 0) return {};
    const uint64_t declared = rd64(h.data(), 0x40);
    if (declared < 0x80 || declared > kSane) return {};
    if (fsz < declared) return truncated(fsz, declared);
    return {};
}

std::string lower(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); return s; }

}  // namespace

std::string format_gb(uint64_t bytes) {
    char b[48];
    std::snprintf(b, sizeof b, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    return b;
}

Result check_structure(const ReadFn& read, uint64_t file_size, const std::string& ext) {
    if (!read) return {};
    if (ext == ".nsp" || ext == ".nsz") return check_pfs0(read, file_size);
    if (ext == ".xci") return check_xci(read, file_size);
    if (ext == ".nup") return check_nup(read, file_size);
    return {};   // .xcz and everything else: can't tell
}

Result check_file(const std::string& path, uint64_t title_id) {
    std::error_code ec;
    const fs::path p = fs::u8path(path);
    const uint64_t fsz = fs::file_size(p, ec);
    if (ec) return {};
    const std::string ext = lower(p.extension().u8string());
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    ReadFn rd = [&](uint64_t off, size_t len, std::vector<uint8_t>* out) {
        out->clear();
        if (off >= fsz) return true;
        const size_t n = (size_t)std::min<uint64_t>(len, fsz - off);
        out->resize(n);
        f.clear();
        f.seekg((std::streamoff)off);
        f.read(reinterpret_cast<char*>(out->data()), (std::streamsize)n);
        out->resize((size_t)std::max<std::streamsize>(0, f.gcount()));
        return !f.bad();
    };
    (void)title_id;
    return check_structure(rd, fsz, ext);
}

std::string warning_text(const Result& r) {
    if (r.v == Verdict::Ok) return "";
    return r.reason + " To fix it, re-dump your own copy of the game.";
}

/* ---- Cache ---- */
struct Cache::State {
    struct Entry {
        Result res;
        bool have = false, pending = false;
        uint64_t size = 0;
        long long mtime = 0;
        std::chrono::steady_clock::time_point checked;
    };
    std::mutex mu;
    std::condition_variable cv;
    std::map<std::string, Entry> m;
    int pending = 0, runs = 0;
    int recheck_ms = 30000;
};

Cache::Cache(int recheck_ms) : s_(std::make_shared<State>()) { s_->recheck_ms = recheck_ms; }

static void worker(std::shared_ptr<Cache::State> s, std::string path, uint64_t tid) {
    std::error_code ec;
    const fs::path p = fs::u8path(path);
    const uint64_t sz = fs::file_size(p, ec);
    bool ok = !ec;
    long long mt = 0;
    if (ok) {
        auto t = fs::last_write_time(p, ec);
        if (!ec) mt = (long long)t.time_since_epoch().count();
    }
    bool rerun = true;
    {
        std::lock_guard<std::mutex> l(s->mu);
        auto& e = s->m[path];
        if (e.have && ok && e.size == sz && e.mtime == mt) rerun = false;
    }
    Result r;
    if (ok && rerun) r = check_file(path, tid);
    std::lock_guard<std::mutex> l(s->mu);
    auto& e = s->m[path];
    if (ok && rerun) { e.res = r; e.size = sz; e.mtime = mt; ++s->runs; }
    e.have = true;   // a vanished file stays Ok / its last answer
    e.pending = false;
    e.checked = std::chrono::steady_clock::now();
    --s->pending;
    s->cv.notify_all();
}

bool Cache::get(const std::string& path, uint64_t title_id, Result* out) {
    bool spawn = false, have = false;
    {
        std::lock_guard<std::mutex> l(s_->mu);
        auto& e = s_->m[path];
        have = e.have;
        if (have && out) *out = e.res;
        if (!e.pending && (!e.have || std::chrono::steady_clock::now() - e.checked >= std::chrono::milliseconds(s_->recheck_ms))) {
            e.pending = true;
            ++s_->pending;
            spawn = true;
        }
    }
    if (spawn) {
        try { std::thread(worker, s_, path, title_id).detach(); }
        catch (...) {
            std::lock_guard<std::mutex> l(s_->mu);
            auto& e = s_->m[path];
            e.pending = false;
            e.have = true;
            e.checked = std::chrono::steady_clock::now();
            --s_->pending;
        }
    }
    return have;
}

void Cache::wait_idle() {
    std::unique_lock<std::mutex> l(s_->mu);
    s_->cv.wait_for(l, std::chrono::seconds(20), [&] { return s_->pending == 0; });
}

int Cache::runs() const {
    std::lock_guard<std::mutex> l(s_->mu);
    return s_->runs;
}

}  // namespace hub::dumpcheck
