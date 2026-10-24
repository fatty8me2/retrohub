#include "ps2.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "miniz.h"
#include "winutil.h"

namespace hub { namespace ps2 {

namespace fs = std::filesystem;

static std::string u8(const fs::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); }
static bool is_alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static char up(char c) { return (c >= 'a' && c <= 'z') ? char(c - 32) : c; }
static std::string lower(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c + 32); return s; }

std::string normalize_serial(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && !is_alpha(s[i])) ++i;
    std::string letters, digits;
    for (; i < s.size() && is_alpha(s[i]) && letters.size() < 5; ++i) letters += up(s[i]);
    if (letters.size() != 4) return "";
    for (; i < s.size() && digits.size() < 5; ++i) {
        char c = s[i];
        if (is_digit(c)) digits += c;
        else if (c == '.' || c == '_' || c == '-' || c == ' ') continue;
        else break;
    }
    if (digits.size() != 5) return "";
    return letters + "-" + digits;
}

std::string serial_from_filename(const std::string& n) {
    for (size_t i = 0; i + 8 < n.size() + 1; ++i) {
        if (i > 0 && (is_alpha(n[i - 1]) || is_digit(n[i - 1]))) continue;
        bool ok = true;
        for (int k = 0; k < 4; ++k) if (i + k >= n.size() || !is_alpha(n[i + k])) { ok = false; break; }
        if (!ok) continue;
        size_t j = i + 4;
        if (j < n.size() && (n[j] == '-' || n[j] == '_' || n[j] == ' ')) ++j;
        std::string d;
        size_t last = j;
        for (; j < n.size() && d.size() < 5; ++j) {
            if (is_digit(n[j])) { d += n[j]; last = j + 1; }
            else if (n[j] == '.' && d.size() == 3) continue;
            else break;
        }
        if (d.size() == 5 && (last >= n.size() || !is_digit(n[last]))) return normalize_serial(n.substr(i, last - i));
    }
    return "";
}

std::string title_for(const std::string& serial) {
    if (serial.size() != 10) return "";
    size_t lo = 0, hi = kDbCount;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = std::strncmp(kDb[mid], serial.c_str(), 10);
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    if (lo < kDbCount && std::strncmp(kDb[lo], serial.c_str(), 10) == 0 && kDb[lo][10] == '|') return kDb[lo] + 11;
    return "";
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

Cnf parse_system_cnf(const std::string& text, std::string* serial) {
    Cnf found = Cnf::None;
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find_first_of("\r\n", p);
        std::string line = text.substr(p, e == std::string::npos ? std::string::npos : e - p);
        p = e == std::string::npos ? text.size() : e + 1;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = lower(trim(line.substr(0, eq))), val = trim(line.substr(eq + 1));
        if (key != "boot2" && key != "boot") continue;
        size_t cut = val.find_last_of(":\\/");
        std::string file = cut == std::string::npos ? val : val.substr(cut + 1);
        size_t semi = file.find(';');
        if (semi != std::string::npos) file.resize(semi);
        found = key == "boot2" ? Cnf::Ps2 : Cnf::Ps1;
        if (serial) *serial = normalize_serial(file);
        if (found == Cnf::Ps2) return found;
    }
    return found;
}

static uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

bool read_system_cnf(const SectorFn& read, std::string* text) {
    uint8_t s[2048];
    if (!read(16, s) || s[0] != 1 || std::memcmp(s + 1, "CD001", 5) != 0) return false;
    uint32_t dir = le32(s + 156 + 2), dsize = le32(s + 156 + 10);
    if (dsize == 0 || dsize > (1u << 20)) return false;
    for (uint32_t off = 0; off < dsize; off += 2048) {
        if (!read(dir + off / 2048, s)) return false;
        uint32_t pos = 0;
        while (pos < 2048) {
            uint32_t len = s[pos];
            if (len == 0) break;
            if (pos + len > 2048 || len < 34) break;
            uint32_t nl = s[pos + 32];
            if (33 + nl <= len) {
                std::string name((const char*)s + pos + 33, nl);
                size_t semi = name.find(';');
                if (semi != std::string::npos) name.resize(semi);
                if (lower(name) == "system.cnf") {
                    uint32_t ext = le32(s + pos + 2), sz = le32(s + pos + 10);
                    if (sz == 0 || sz > 8192) return false;
                    std::string out;
                    uint8_t b[2048];
                    for (uint32_t o = 0; o < sz; o += 2048) {
                        if (!read(ext + o / 2048, b)) return false;
                        out.append((const char*)b, std::min<uint32_t>(2048, sz - o));
                    }
                    *text = out;
                    return true;
                }
            }
            pos += len;
        }
    }
    return false;
}

SectorFn buffer_reader(std::shared_ptr<std::string> data) {
    return [data](uint32_t lba, uint8_t* out) {
        uint64_t off = uint64_t(lba) * 2048;
        if (off + 2048 > data->size()) return false;
        std::memcpy(out, data->data() + off, 2048);
        return true;
    };
}

struct Disc::Impl {
    FILE* f = nullptr;
    enum { Iso, Raw, Cso } kind = Iso;
    int raw_off = 16;
    uint32_t block = 0;
    int align = 0;
    uint64_t total = 0;
    std::vector<uint32_t> idx;
    std::vector<uint8_t> cache;
    int64_t cached = -1;
    ~Impl() { if (f) std::fclose(f); }
    bool at(uint64_t off, void* buf, size_t n) {
        if (_fseeki64(f, (long long)off, SEEK_SET) != 0) return false;
        return std::fread(buf, 1, n, f) == n;
    }
};

Disc::Disc() {}
Disc::~Disc() {}

bool Disc::open(const std::string& path) {
    p_ = std::make_shared<Impl>();
    Impl& d = *p_;
    d.f = _wfopen(winutil::to_wide(path).c_str(), L"rb");
    if (!d.f) return false;
    std::string ext = lower(u8(fs::u8path(path).extension()));
    uint8_t h[32];
    if (ext == ".cso" || ext == ".ciso") {
        if (!d.at(0, h, 24) || std::memcmp(h, "CISO", 4) != 0) return false;
        uint32_t hs = le32(h + 4);
        d.total = uint64_t(le32(h + 8)) | (uint64_t(le32(h + 12)) << 32);
        d.block = le32(h + 16);
        d.align = h[21];
        if (d.block < 2048 || d.block > (1u << 20) || hs < 24 || d.total == 0 || (d.block & (d.block - 1))) return false;
        uint64_t n = (d.total + d.block - 1) / d.block;
        if (n > (1ull << 24)) return false;
        d.idx.resize(size_t(n + 1));
        if (!d.at(24, d.idx.data(), d.idx.size() * 4)) return false;
        d.kind = Impl::Cso;
        return true;
    }
    if (ext == ".bin") {
        uint8_t sync[16];
        static const uint8_t pat[12] = {0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0};
        if (d.at(0, sync, 16) && std::memcmp(sync, pat, 12) == 0) {
            d.kind = Impl::Raw;
            d.raw_off = sync[15] == 2 ? 24 : 16;
            return true;
        }
    }
    d.kind = Impl::Iso;
    return true;
}

SectorFn Disc::reader() {
    std::shared_ptr<Impl> p = p_;
    return [p](uint32_t lba, uint8_t* out) -> bool {
        if (!p || !p->f) return false;
        Impl& d = *p;
        if (d.kind == Impl::Iso) return d.at(uint64_t(lba) * 2048, out, 2048);
        if (d.kind == Impl::Raw) return d.at(uint64_t(lba) * 2352 + d.raw_off, out, 2048);
        uint64_t off = uint64_t(lba) * 2048;
        if (off + 2048 > d.total) return false;
        int64_t b = int64_t(off / d.block);
        if (b != d.cached) {
            uint32_t e0 = d.idx[size_t(b)], e1 = d.idx[size_t(b) + 1];
            uint64_t p0 = uint64_t(e0 & 0x7FFFFFFFu) << d.align, p1 = uint64_t(e1 & 0x7FFFFFFFu) << d.align;
            if (p1 <= p0 || p1 - p0 > (2u << 20)) return false;
            std::vector<uint8_t> raw(size_t(p1 - p0));
            if (!d.at(p0, raw.data(), raw.size())) return false;
            d.cache.assign(d.block, 0);
            if (e0 & 0x80000000u) std::memcpy(d.cache.data(), raw.data(), std::min<size_t>(raw.size(), d.block));
            else if (tinfl_decompress_mem_to_mem(d.cache.data(), d.block, raw.data(), raw.size(), 0) == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) return false;
            d.cached = b;
        }
        std::memcpy(out, d.cache.data() + (off % d.block), 2048);
        return true;
    };
}

std::string cue_first_file(const std::string& t) {
    size_t p = 0;
    while (p < t.size()) {
        size_t e = t.find_first_of("\r\n", p);
        std::string line = trim(t.substr(p, e == std::string::npos ? std::string::npos : e - p));
        p = e == std::string::npos ? t.size() : e + 1;
        if (line.size() > 5 && lower(line.substr(0, 4)) == "file" && (line[4] == ' ' || line[4] == '\t')) {
            std::string r = trim(line.substr(5));
            if (!r.empty() && r[0] == '"') {
                size_t q = r.find('"', 1);
                return q == std::string::npos ? "" : r.substr(1, q - 1);
            }
            size_t sp = r.find_first_of(" \t");
            return sp == std::string::npos ? r : r.substr(0, sp);
        }
    }
    return "";
}

static bool finish(const std::string& cnf, Ident* out) {
    std::string serial;
    Cnf k = parse_system_cnf(cnf, &serial);
    if (k == Cnf::Ps1) { out->ps1 = true; return false; }
    if (k != Cnf::Ps2) return false;
    out->serial = serial;
    out->title = title_for(serial);
    return true;
}

bool identify_buffer(std::shared_ptr<std::string> data, Ident* out) {
    std::string cnf;
    if (!read_system_cnf(buffer_reader(std::move(data)), &cnf)) return false;
    return finish(cnf, out);
}

bool identify_file(const std::string& path, Ident* out) {
    *out = Ident();
    fs::path p = fs::u8path(path);
    std::string ext = lower(u8(p.extension()));
    if (ext == ".chd") {   // compressed hunks are not decoded here: the serial must be in the file name and a known PS2 game
        std::string s = serial_from_filename(u8(p.filename()));
        std::string t = title_for(s);
        if (s.empty() || t.empty()) return false;
        out->serial = s;
        out->title = t;
        return true;
    }
    std::string image = path;
    if (ext == ".cue") {
        std::ifstream f(p, std::ios::binary);
        if (!f) return false;
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::string bin = cue_first_file(text);
        if (bin.empty()) return false;
        fs::path bp = fs::u8path(bin);
        if (bp.is_relative()) bp = p.parent_path() / bp;
        image = u8(bp);
        if (lower(u8(bp.extension())) == ".chd") return identify_file(image, out);
    } else if (ext != ".iso" && ext != ".bin" && ext != ".cso" && ext != ".ciso") {
        return false;
    }
    Disc d;
    std::string cnf;
    if (!d.open(image) || !read_system_cnf(d.reader(), &cnf)) return false;
    return finish(cnf, out);
}

std::string key_for(const std::string& serial, const std::string& stem_key) {
    return serial.empty() ? "ps2:file-" + stem_key : "ps2:" + lower(serial);
}

}} // namespace hub::ps2
