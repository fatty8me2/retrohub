#include "verify.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>

#include "games.h"
#include "miniz.h"
#include "winutil.h"

namespace fs = std::filesystem;

namespace hub::verify {

namespace {
std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }

const uint32_t* crc_table() {
    static uint32_t table[256];
    static std::once_flag once;
    std::call_once(once, [] {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
    });
    return table;
}

uint64_t mk(uint32_t crc, uint64_t size) { return (uint64_t)crc | (size << 32); }

std::mutex g_db_mu;
std::string g_dir;
KnownDb* g_db = nullptr;
}  // namespace

bool supported(System s) {
    switch (s) {
        case System::NES: case System::SNES: case System::N64: case System::GB: case System::GBC: case System::GBA: case System::NDS: return true;
        default: return false;
    }
}

uint32_t crc32_update(uint32_t crc, const uint8_t* d, size_t n) {
    const uint32_t* t = crc_table();
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = t[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ---------------------------------------------------------------- KnownDb */

bool KnownDb::load_text(const std::string& text) {
    m_.clear();
    System cur = System::Unknown;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '@') {
            size_t t = line.find('\t');
            cur = system_from_short(line.substr(1, t == std::string::npos ? std::string::npos : t - 1));
            continue;
        }
        if (cur == System::Unknown) continue;
        size_t t1 = line.find('\t');
        size_t t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
        if (t2 == std::string::npos || t1 == 0 || t1 > 8) continue;   // malformed row: ignore it
        char* end = nullptr;
        unsigned long crc = std::strtoul(line.substr(0, t1).c_str(), &end, 16);
        if (!end || *end) continue;
        unsigned long long sz = std::strtoull(line.substr(t1 + 1, t2 - t1 - 1).c_str(), nullptr, 10);
        m_[cur].emplace(mk((uint32_t)crc, sz), line.substr(t2 + 1));
    }
    return total() > 0;
}

bool KnownDb::load_blob(const std::string& bytes) {
    m_.clear();
    header_.clear();
    size_t nl = bytes.find('\n');
    if (nl == std::string::npos || bytes.compare(0, 5, "RHKD1") != 0) return false;
    header_ = bytes.substr(0, nl);
    size_t n = 0;
    void* p = tinfl_decompress_mem_to_heap(bytes.data() + nl + 1, bytes.size() - nl - 1, &n, TINFL_FLAG_PARSE_ZLIB_HEADER);
    if (!p) return false;
    std::string text((const char*)p, n);
    mz_free(p);
    return load_text(text);
}

bool KnownDb::load_file(const std::string& path) {
    std::ifstream f(fs::path(winutil::to_wide(path)), std::ios::binary);
    if (!f) return false;
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return load_blob(bytes);
}

const std::string* KnownDb::find_in(System s, uint32_t crc, uint64_t size) const {
    auto it = m_.find(s);
    if (it == m_.end()) return nullptr;
    auto r = it->second.find(mk(crc, size));
    return r == it->second.end() ? nullptr : &r->second;
}

const std::string* KnownDb::find(System s, uint32_t crc, uint64_t size) const {
    if (const std::string* r = find_in(s, crc, size)) return r;
    if (s == System::GB) return find_in(System::GBC, crc, size);
    if (s == System::GBC) return find_in(System::GB, crc, size);
    return nullptr;
}

size_t KnownDb::count(System s) const {
    auto it = m_.find(s);
    return it == m_.end() ? 0 : it->second.size();
}

size_t KnownDb::total() const {
    size_t n = 0;
    for (auto& kv : m_) n += kv.second.size();
    return n;
}

void set_data_dir(const std::string& dir) {
    std::lock_guard<std::mutex> lk(g_db_mu);
    delete g_db;
    g_db = nullptr;
    g_dir = dir;
    if (!g_dir.empty() && g_dir.back() != '/' && g_dir.back() != '\\') g_dir += '/';
}

const KnownDb& known_db() {
    std::lock_guard<std::mutex> lk(g_db_mu);
    if (!g_db) {
        g_db = new KnownDb();
        g_db->load_file((g_dir.empty() ? hub::exe_dir() + "data/" : g_dir) + "known_dumps.bin");
    }
    return *g_db;
}

/* ---------------------------------------------------------------- hashing */

bool hash_stream(System s, uint64_t total, const ReadFn& read, FileHash* out, const std::atomic<bool>* stop) {
    if (total == 0) return false;
    const size_t kChunk = 1u << 20;   // a multiple of 4, so N64 words never straddle two chunks
    std::vector<uint8_t> buf(kChunk);
    auto fill = [&](size_t want) {
        size_t got = 0;
        while (got < want) {
            size_t r = read(buf.data() + got, want - got);
            if (!r) break;
            got += r;
        }
        return got;
    };
    uint64_t skip = 0;
    int n64order = 0;   // 0 = already z64 (or unknown), 1 = .v64 (byte-swapped pairs), 2 = .n64 (little-endian words)
    uint32_t crc = 0;
    uint64_t hashed = 0, seen = 0;
    bool first = true;
    while (seen < total) {
        if (stop && *stop) return false;
        size_t got = fill((size_t)std::min<uint64_t>(kChunk, total - seen));
        if (!got) break;
        uint8_t* p = buf.data();
        size_t n = got;
        if (first) {
            first = false;
            if (s == System::NES && n >= 16 && p[0] == 'N' && p[1] == 'E' && p[2] == 'S' && p[3] == 0x1A) skip = 16;   // No-Intro NES CRCs are headerless
            else if (s == System::SNES) skip = snes_copier_header_size(total);
            else if (s == System::N64 && n >= 4) {
                if (p[0] == 0x37 && p[1] == 0x80 && p[2] == 0x40 && p[3] == 0x12) n64order = 1;
                else if (p[0] == 0x40 && p[1] == 0x12 && p[2] == 0x37 && p[3] == 0x80) n64order = 2;
            }
        }
        if (n64order) {
            size_t w = n & ~(size_t)3;
            for (size_t i = 0; i < w; i += 4) {
                if (n64order == 1) { std::swap(p[i], p[i + 1]); std::swap(p[i + 2], p[i + 3]); }
                else { std::swap(p[i], p[i + 3]); std::swap(p[i + 1], p[i + 2]); }
            }
        }
        seen += got;
        if (skip > 0) {   // drop the header bytes (a header always fits in the first chunk)
            size_t d = (size_t)std::min<uint64_t>(skip, n);
            p += d; n -= d; skip -= d;
        }
        crc = crc32_update(crc, p, n);
        hashed += n;
    }
    if (seen < total) return false;   // short read: the file changed or the drive went away
    out->crc = crc;
    out->size = hashed;
    return true;
}

bool hash_file(System s, const std::string& path, FileHash* out, const std::atomic<bool>* stop) {
    FILE* f = _wfopen(winutil::to_wide(path).c_str(), L"rbN");
    if (!f) return false;
    _fseeki64(f, 0, SEEK_END);
    uint64_t total = (uint64_t)_ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    bool ok = hash_stream(s, total, [&](uint8_t* b, size_t n) { return std::fread(b, 1, n, f); }, out, stop);
    std::fclose(f);
    return ok;
}

/* ---------------------------------------------------------------- cache */

bool HashCache::get(const std::string& path, System s, uint64_t size, long long mtime, FileHash* out) const {
    auto it = m_.find(lower(path));
    if (it == m_.end()) return false;
    const Rec& r = it->second;
    if (r.size != size || r.mtime != mtime || r.sys != system_short(s)) return false;
    *out = r.h;
    return true;
}

void HashCache::put(const std::string& path, System s, uint64_t size, long long mtime, const FileHash& h) {
    Rec r;
    r.sys = system_short(s);
    r.size = size;
    r.mtime = mtime;
    r.h = h;
    m_[lower(path)] = r;
}

bool HashCache::load(const std::string& file) {
    m_.clear();
    std::ifstream f(fs::path(winutil::to_wide(file)), std::ios::binary);
    if (!f) return false;
    std::string line;
    if (!std::getline(f, line) || line != "RHVC1") return false;
    while (std::getline(f, line)) {   // path \t sys \t size \t mtime \t crc(hex) \t hashed size
        std::vector<std::string> c;
        size_t a = 0;
        for (;;) {
            size_t t = line.find('\t', a);
            c.push_back(line.substr(a, t == std::string::npos ? std::string::npos : t - a));
            if (t == std::string::npos) break;
            a = t + 1;
        }
        if (c.size() != 6 || c[0].empty()) continue;
        Rec r;
        r.sys = c[1];
        r.size = std::strtoull(c[2].c_str(), nullptr, 10);
        r.mtime = std::strtoll(c[3].c_str(), nullptr, 10);
        r.h.crc = (uint32_t)std::strtoul(c[4].c_str(), nullptr, 16);
        r.h.size = std::strtoull(c[5].c_str(), nullptr, 10);
        m_[c[0]] = r;
    }
    return true;
}

bool HashCache::save(const std::string& file) const {
    std::wstring tmp = winutil::to_wide(file) + L".tmp";
    {
        std::ofstream f(fs::path(tmp), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << "RHVC1\n";
        for (const auto& kv : m_) {
            const Rec& r = kv.second;
            char nums[96];
            std::snprintf(nums, sizeof nums, "\t%llu\t%lld\t%08X\t%llu", (unsigned long long)r.size, r.mtime, r.h.crc, (unsigned long long)r.h.size);
            f << kv.first << '\t' << r.sys << nums << '\n';
        }
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(fs::path(tmp), fs::path(winutil::to_wide(file)), ec);
    if (ec) {
        fs::remove(fs::path(winutil::to_wide(file)), ec);
        ec.clear();
        fs::rename(fs::path(tmp), fs::path(winutil::to_wide(file)), ec);
    }
    return !ec;
}

/* ---------------------------------------------------------------- runner */

Result judge(const KnownDb& db, System s, const FileHash& h) {
    Result r;
    r.crc = h.crc;
    if (const std::string* t = db.find(s, h.crc, h.size)) { r.v = Verdict::Good; r.title = *t; }
    else r.v = Verdict::Unknown;
    return r;
}

void Runner::start(std::vector<Item> items, const KnownDb* db, const std::string& cache_file, bool cache_only) {
    stop_and_join();
    cancel_ = false;
    done_ = 0;
    total_ = 0;
    items.erase(std::remove_if(items.begin(), items.end(), [](const Item& i) { return !supported(i.system) || i.path.empty(); }), items.end());
    total_ = (int)items.size();
    { std::lock_guard<std::mutex> lk(mu_); res_.clear(); }
    running_ = true;
    th_ = std::thread([this, it = std::move(items), db, cf = cache_file, co = cache_only]() mutable { run(std::move(it), db, std::move(cf), co); });
}

void Runner::run(std::vector<Item> items, const KnownDb* db, std::string cache_file, bool cache_only) {
    HashCache cache;
    if (!cache_file.empty()) cache.load(cache_file);
    bool dirty = false;
    for (const Item& it : items) {
        if (cancel_) break;
        Result r;
        r.v = Verdict::Unreadable;
        std::error_code e1, e2;
        fs::path p(winutil::to_wide(it.path));
        uint64_t size = fs::file_size(p, e1);
        long long mtime = (long long)fs::last_write_time(p, e2).time_since_epoch().count();
        if (!e1 && !e2) {
            FileHash h;
            if (cache.get(it.path, it.system, size, mtime, &h)) r = judge(*db, it.system, h);
            else if (cache_only) { ++done_; continue; }   // never hashed (or changed since): nothing to say yet
            else if (hash_file(it.system, it.path, &h, &cancel_)) {
                cache.put(it.path, it.system, size, mtime, h);
                dirty = true;
                r = judge(*db, it.system, h);
            } else if (cancel_) break;
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            res_[it.key] = r;
        }
        ++done_;
    }
    if (dirty && !cache_file.empty()) cache.save(cache_file);
    running_ = false;
}

bool Runner::get(const std::string& key, Result* out) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = res_.find(key);
    if (it == res_.end()) return false;
    if (out) *out = it->second;
    return true;
}

std::map<std::string, Result> Runner::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return res_;
}

}  // namespace hub::verify
