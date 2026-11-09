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

/* One row per known title. Sizes are of the container named in `ext`.
 * Ocarina of Time (Switch 2): the owner's incomplete copy is 25,020,741,061 bytes (23.30 GB) and renders the Water Temple without walls; a
 * clean re-dump is about 25.62e9 bytes (23.86 GB). Evidence: Lichii issue tracker #577, #624, #685 (a clean re-dump fixed it on every
 * build) and #487 (version 1.1.1 files show it too, 2.0.0 fixed it). 25.4e9 sits between the two. */
const KnownSize kKnownSizes[] = {
    {0x0400486024DEC000ull, "The Legend of Zelda: Ocarina of Time (Switch 2)", ".nup", 25400000000ull, 25620000000ull,
     "Some areas, like the Water Temple, will be missing walls or textures.", "Lichii issues #577 #624 #685 #487; owner's dump"},
};

uint32_t rd32(const uint8_t* d, size_t at) { return (uint32_t)d[at] | ((uint32_t)d[at + 1] << 8) | ((uint32_t)d[at + 2] << 16) | ((uint32_t)d[at + 3] << 24); }
uint64_t rd64(const uint8_t* d, size_t at) { return (uint64_t)rd32(d, at) | ((uint64_t)rd32(d, at + 4) << 32); }

constexpr uint64_t kSane = 1ull << 50;   // a declared size beyond 1 PiB is a malformed header, not a verdict

Result truncated(uint64_t have, uint64_t expected) {
    Result r;
    r.v = Verdict::Truncated;
    r.have = have;
    r.expected = expected;
    r.reason = "This file looks cut short (" + format_gb(have) + " of " + format_gb(expected) + "). The copy may have been interrupted.";
    return r;
}

Result check_pfs0(const ReadFn& read, uint64_t fsz) {
    std::vector<uint8_t> h;
    if (!read(0, 16, &h) || h.size() < 16 || std::memcmp(h.data(), "PFS0", 4) != 0) return {};
    const uint32_t count = rd32(h.data(), 4), strsz = rd32(h.data(), 8);
    if (count == 0 || count > 0x10000) return {};
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

const KnownSize* known_sizes(size_t* count) {
    if (count) *count = sizeof(kKnownSizes) / sizeof(kKnownSizes[0]);
    return kKnownSizes;
}

std::string format_gb(uint64_t bytes) {
    char b[48];
    std::snprintf(b, sizeof b, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    return b;
}

Result check_size(uint64_t title_id, const std::string& ext, uint64_t file_size) {
    if (!title_id) return {};
    for (const KnownSize& k : kKnownSizes) {
        if (k.title_id != title_id || ext != k.ext) continue;
        if (file_size >= k.complete_at_least) return {};
        Result r;
        r.v = Verdict::LooksIncomplete;
        r.have = file_size;
        r.expected = k.typical_complete;
        r.area_note = k.area_note ? k.area_note : "";
        r.reason = "This copy looks incomplete (" + format_gb(file_size) + "). Complete copies are about " + format_gb(k.typical_complete) + ".";
        return r;
    }
    return {};
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
    const std::string ext = lower(p.extension().string());
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
    Result r = check_structure(rd, fsz, ext);
    if (r.v == Verdict::Truncated) return r;
    return check_size(title_id, ext, fsz);
}

std::string warning_text(const Result& r) {
    if (r.v == Verdict::Ok) return "";
    if (r.v == Verdict::Truncated)
        return r.reason + " Parts of the game may be missing or crash. To fix it, copy or re-dump your own copy of the game again.";
    return r.reason + (r.area_note.empty() ? "" : " " + r.area_note) + " To fix it, re-dump your own copy of the game.";
}

std::string short_line(const Result& r) {
    if (r.v == Verdict::Truncated) return "Cut short: " + format_gb(r.have) + " of " + format_gb(r.expected);
    if (r.v == Verdict::LooksIncomplete) return "Looks incomplete: " + format_gb(r.have) + ", complete copies are about " + format_gb(r.expected);
    return "";
}

/* ---- DismissList ---- */
bool DismissList::has(const std::string& path, uint64_t size) const {
    auto it = m_.find(lower(path));
    return it != m_.end() && it->second == size;
}
void DismissList::add(const std::string& path, uint64_t size) {
    if (path.empty() || path.find_first_of("\t\r\n") != std::string::npos) return;
    m_[lower(path)] = size;
}
std::string DismissList::serialize() const {
    std::string o;
    for (const auto& kv : m_) o += std::to_string(kv.second) + "\t" + kv.first + "\n";
    return o;
}
void DismissList::parse(const std::string& text) {
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(p, e - p);
        p = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t t = line.find('\t');
        if (t == std::string::npos || t == 0 || t + 1 >= line.size()) continue;
        bool digits = true;
        for (size_t i = 0; i < t; ++i) if (line[i] < '0' || line[i] > '9') digits = false;
        if (!digits || t > 19) continue;
        add(line.substr(t + 1), std::strtoull(line.c_str(), nullptr, 10));
    }
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
};

Cache::Cache() : s_(std::make_shared<State>()) {}

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
        if (!e.pending && (!e.have || std::chrono::steady_clock::now() - e.checked > std::chrono::seconds(30))) {
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
