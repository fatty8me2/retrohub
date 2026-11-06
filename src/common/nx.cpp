#include "nx.h"
/* Switch / Switch 2 identification (nx.h). Header-only reads, no keys, every offset bounds-checked. */

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace hub::nx {
namespace {

constexpr uint32_t kMaxIconBytes = 4u << 20;   // an icon larger than this is ignored (real ones are 20-200 KB)

uint32_t rd32(const uint8_t* d, size_t at) { return (uint32_t)d[at] | ((uint32_t)d[at + 1] << 8) | ((uint32_t)d[at + 2] << 16) | ((uint32_t)d[at + 3] << 24); }
uint64_t rd64(const uint8_t* d, size_t at) { return (uint64_t)rd32(d, at) | ((uint64_t)rd32(d, at + 4) << 32); }
bool has(size_t n, uint64_t at, uint64_t len) { return at <= n && len <= n - at; }   // [at, at+len) inside the n bytes

std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
int hexv(char c) { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; if (c >= 'A' && c <= 'F') return c - 'A' + 10; return -1; }
bool hex_run(const std::string& s, size_t at, size_t len, uint64_t* v16 = nullptr) {   // len hex digits at `at`; v16 = the first 16 as a number
    if (at + len > s.size()) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < len; ++i) { int h = hexv(s[at + i]); if (h < 0) return false; if (i < 16) v = (v << 4) | (uint64_t)h; }
    if (v16) *v16 = v;
    return true;
}

void finish(Ident* o) { if (o->title_id && !is_base_title(o->title_id)) o->update_or_dlc = true; }

std::string cstr(const uint8_t* d, size_t max) {
    size_t n = 0;
    while (n < max && d[n]) ++n;
    std::string s((const char*)d, n);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

bool read_at(const fs::path& path, uint64_t off, size_t len, std::vector<uint8_t>* out, uint64_t* file_size = nullptr) {
    FILE* f = nullptr;
#ifdef _WIN32
    f = _wfopen(path.c_str(), L"rb");
#else
    f = std::fopen(path.string().c_str(), "rb");
#endif
    if (!f) return false;
    bool ok = false;
    if (_fseeki64(f, 0, SEEK_END) == 0) {
        long long sz = _ftelli64(f);
        if (file_size) *file_size = sz < 0 ? 0 : (uint64_t)sz;
        if (sz >= 0 && (uint64_t)sz > off) {
            size_t want = (size_t)std::min<uint64_t>(len, (uint64_t)sz - off);
            out->assign(want, 0);
            if (_fseeki64(f, (long long)off, SEEK_SET) == 0) ok = std::fread(out->data(), 1, want, f) == want;
        }
    }
    std::fclose(f);
    return ok;
}

}  // namespace

uint64_t title_id_from_name(const std::string& name) {
    for (size_t i = 0; i + 18 <= name.size(); ++i) {
        if (name[i] != '[' || name[i + 17] != ']') continue;
        uint64_t v;
        if (hex_run(name, i + 1, 16, &v)) return v;
    }
    return 0;
}

std::string clean_title(const std::string& name) {
    std::string o;
    int depth = 0;
    for (char c : name) {
        if (c == '(' || c == '[') { ++depth; continue; }
        if (c == ')' || c == ']') { if (depth > 0) --depth; continue; }
        if (depth == 0) o.push_back(c == '_' ? ' ' : c);
    }
    std::string r;
    for (char c : o) { if (c == ' ' || c == '\t') { if (!r.empty() && r.back() != ' ') r.push_back(' '); } else r.push_back(c); }
    while (!r.empty() && r.back() == ' ') r.pop_back();
    return r;
}

bool is_base_title(uint64_t id) { return id != 0 && (id & 0xFFF) == 0; }

bool parse_pfs0(const uint8_t* d, size_t n, Ident* out) {
    if (!d || !out || n < 16 || std::memcmp(d, "PFS0", 4) != 0) return false;
    uint32_t count = rd32(d, 4), strsz = rd32(d, 8);
    out->format = "nsp";
    if (count == 0 || count > 0x10000) return true;
    const uint64_t ent = 16, strtab = ent + (uint64_t)count * 0x18;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t e = ent + (uint64_t)i * 0x18;
        if (!has(n, e, 0x18)) break;
        uint32_t no = rd32(d, (size_t)e + 16);
        if (no >= strsz) continue;
        uint64_t s = strtab + no;
        if (s >= n) continue;
        size_t avail = (size_t)std::min<uint64_t>(n - s, std::min<uint64_t>(strsz - no, 128));
        std::string name((const char*)d + s, strnlen((const char*)d + s, avail));
        if (name.size() == 36 && lower(name.substr(32)) == ".tik") {
            uint64_t v;
            if (hex_run(name, 0, 32, &v) && v) { out->title_id = v; break; }
        }
    }
    finish(out);
    return true;
}

bool parse_xci(const uint8_t* d, size_t n, Ident* out) {
    if (!d || !out) return false;
    if (!(has(n, 0x100, 4) && std::memcmp(d + 0x100, "HEAD", 4) == 0) && !(has(n, 0x1100, 4) && std::memcmp(d + 0x1100, "HEAD", 4) == 0)) return false;
    out->format = "xci";
    return true;
}

bool parse_nro(const uint8_t* d, size_t n, uint64_t file_size, Ident* out) {
    if (!d || !out || !has(n, 0x10, 4) || std::memcmp(d + 0x10, "NRO0", 4) != 0) return false;
    out->format = "nro";
    if (!has(n, 0x18, 4)) return true;
    uint64_t sz = rd32(d, 0x18);
    if (sz == 0 || (file_size && sz + 0x38 > file_size) || !has(n, sz, 0x38) || std::memcmp(d + sz, "ASET", 4) != 0) return true;
    uint64_t noff = rd64(d, (size_t)sz + 0x18), nsz = rd64(d, (size_t)sz + 0x20);
    if (nsz < 0x200 || noff >= n || !has(n, sz + noff, 0x200)) return true;
    std::string t = cstr(d + sz + noff, 0x200);
    if (!t.empty()) out->title = t;
    return true;
}

bool parse_npdm(const uint8_t* d, size_t n, Ident* out) {
    if (!d || !out || n < 0x80 || std::memcmp(d, "META", 4) != 0) return false;
    out->format = "exefs";
    out->loose = true;
    std::string name = cstr(d + 0x20, 0x10);
    if (!name.empty() && name != "Application") out->title = name;
    uint32_t aci = rd32(d, 0x70), acid = rd32(d, 0x78);
    if (has(n, aci, 0x18) && std::memcmp(d + aci, "ACI0", 4) == 0) out->title_id = rd64(d, (size_t)aci + 0x10);
    if (has(n, (uint64_t)acid + 0x300, 4) && std::memcmp(d + (size_t)acid + 0x300, "ACID", 4) == 0) out->nx2 = true;   // 0x200 = Switch 1 layout
    finish(out);
    return true;
}

bool parse_nup(const ReadFn& read, uint64_t file_size, Ident* out, std::vector<uint8_t>* icon) {
    if (icon) icon->clear();
    if (!read || !out) return false;
    std::vector<uint8_t> sb;
    if (!read(0, 0x80, &sb) || sb.size() < 0x80 || std::memcmp(sb.data(), "NUP\x01", 4) != 0) return false;
    /* The checks Lichii's NupContainer::Parse makes on these headers (a file it would refuse is not listed: it would not boot). The TOC SHA-256 is
     * not checked (the TOC can be tens of MB). */
    const uint64_t toc_off = rd64(sb.data(), 0x08), toc_size = rd64(sb.data(), 0x10);
    const uint64_t blob_off = rd64(sb.data(), 0x38), declared_size = rd64(sb.data(), 0x40);
    const uint32_t count = rd32(sb.data(), 0x48);
    if ((uint32_t)(sb[4] | (sb[5] << 8)) < 0x80 || rd32(sb.data(), 0x4C) != 0) return false;
    if (file_size && declared_size != file_size) return false;   // truncated or padded copy
    if (toc_off < 0x80 || blob_off > declared_size || toc_off > blob_off || toc_size > blob_off - toc_off || blob_off % 0x10) return false;
    if (count == 0 || count > 4096) return false;
    const uint64_t want = 0x30 + (uint64_t)count * 0x40;
    std::vector<uint8_t> toc;
    if (want > toc_size || !read(toc_off, (size_t)want, &toc) || toc.size() < want || std::memcmp(toc.data(), "TOC\x01", 4) != 0) return false;
    if (rd32(toc.data(), 4) != count) return false;
    const uint64_t dir_count = rd64(toc.data(), 8);
    if (dir_count > toc_size / 0x18) return false;
    constexpr uint32_t kNoDir = 0xFFFFFFFFu;
    auto rec = [&](uint32_t i) { return toc.data() + 0x30 + (size_t)i * 0x40; };
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* r = rec(i);
        const uint32_t ex = rd32(r, 0x0C), ro = rd32(r, 0x10);
        if (r[0x18] > 2 || (r[0x19] & ~1u) || rd32(r, 0x08) >= dir_count || (ex != kNoDir && ex >= dir_count) || (ro != kNoDir && ro >= dir_count)) return false;
    }
    out->format = "nup";
    out->update_or_dlc = true;   // until a playable package is found
    /* GetExeFSDir: no ExeFS dir, or an empty one (dir record child_count 0), cannot boot. Dir records follow the package records. */
    auto has_exefs = [&](const uint8_t* r) {
        const uint32_t ex = rd32(r, 0x0C);
        if (ex == kNoDir) return false;
        const uint64_t at = toc_off + want + (uint64_t)ex * 0x18;
        std::vector<uint8_t> d;
        return want + ((uint64_t)ex + 1) * 0x18 <= toc_size && read(at, 0x18, &d) && d.size() == 0x18 && rd32(d.data(), 0x0C) != 0;
    };
    long pick = -1;   // Lichii's loader: the FIRST base package if it has an ExeFS, else the newest update that has one
    for (uint32_t i = 0; i < count; ++i)
        if (rec(i)[0x18] == 0) { if (has_exefs(rec(i))) pick = (long)i; break; }
    if (pick < 0)
        for (uint32_t i = 0; i < count; ++i)   // FindBestUpdatePackage: highest version among the updates, then its ExeFS
            if (rec(i)[0x18] == 1 && (pick < 0 || rd32(rec(i), 0x14) > rd32(rec((uint32_t)pick), 0x14))) pick = (long)i;
    if (pick >= 0 && rec((uint32_t)pick)[0x18] == 1 && !has_exefs(rec((uint32_t)pick))) pick = -1;
    if (pick < 0) return true;
    const uint8_t* r = rec((uint32_t)pick);
    out->update_or_dlc = false;
    out->title_id = rd64(r, 0x00) & ~0xFFFull;   // an update's id is the base id + 0x800; saves and the key follow the base game

    const uint64_t meta_off = rd64(r, 0x20);
    const uint32_t meta_size = rd32(r, 0x28);
    std::vector<uint8_t> m;
    if (meta_off && meta_size && meta_size <= (16u << 20) && (!file_size || (meta_off <= file_size && meta_size <= file_size - meta_off)) &&
        read(meta_off, meta_size, &m)) {
        std::string localized;
        std::vector<uint8_t> en_icon, icon_fallback, first_icon;
        for (size_t p = 0; p + 8 <= m.size();) {
            const uint16_t type = (uint16_t)(m[p] | (m[p + 1] << 8));
            const uint32_t sz = rd32(m.data(), p + 4);
            p += 8;
            if (sz > m.size() - p) break;
            const uint8_t* pl = m.data() + p;
            if (type == 1) {   // NPDM
                Ident n;
                if (parse_npdm(pl, sz, &n)) out->nx2 = n.nx2;
            } else if (type == 5) {
                out->title = cstr(pl, sz);
            } else if (icon && type == 3) {   // the package's own icon (JPEG / PNG), bounded
                if (sz > 0 && sz <= kMaxIconBytes && icon_fallback.empty()) icon_fallback.assign(pl, pl + sz);
            } else if (icon && type == 10) {   // localized icons: same record layout as type 9, English preferred, else the first
                for (size_t q = 0; q < sz;) {
                    const size_t ll = pl[q++];
                    if (q + ll + 4 > sz) break;
                    const std::string lang((const char*)pl + q, ll);
                    q += ll;
                    const uint32_t il = rd32(pl, q);
                    q += 4;
                    if (il > sz - q) break;
                    if (il > 0 && il <= kMaxIconBytes) {
                        if (lower(lang).rfind("en", 0) == 0 && en_icon.empty()) en_icon.assign(pl + q, pl + q + il);
                        else if (first_icon.empty()) first_icon.assign(pl + q, pl + q + il);
                    }
                    q += il;
                }
            } else if (type == 9 && localized.empty()) {   // [u8 lang len][lang][u32 len][title]...: English first, else the first one
                std::string first;
                for (size_t q = 0; q < sz;) {
                    const size_t ll = pl[q++];
                    if (q + ll + 4 > sz) break;
                    const std::string lang((const char*)pl + q, ll);
                    q += ll;
                    const uint32_t tl = rd32(pl, q);
                    q += 4;
                    if (tl > sz - q) break;
                    std::string t = cstr(pl + q, tl);
                    q += tl;
                    if (first.empty()) first = t;
                    if (lower(lang).rfind("en", 0) == 0 && !t.empty()) { localized = t; break; }
                }
                if (localized.empty()) localized = first;
            }
            p += sz;
        }
        if (out->title.empty()) out->title = localized;
        if (icon) *icon = !en_icon.empty() ? en_icon : !icon_fallback.empty() ? icon_fallback : first_icon;
    }
    if (!out->nx2 && (out->title_id >> 56) == 0x04) out->nx2 = true;   // Switch 2 program ids are 04xx...
    return true;
}

bool nup_icon_file(const std::string& path, std::vector<uint8_t>* icon) {
    if (!icon) return false;
    icon->clear();
    fs::path p = fs::u8path(path);
    std::error_code ec;
    const uint64_t fsz = fs::file_size(p, ec);
    if (ec) return false;
    Ident id;
    bool ok = parse_nup([&](uint64_t off, size_t len, std::vector<uint8_t>* o) { return read_at(p, off, len, o); }, fsz, &id, icon);
    return ok && !icon->empty();
}

bool identify_file(const std::string& path, Ident* out) {
    if (!out) return false;
    *out = Ident();
    fs::path p = fs::u8path(path);
    std::string ext = lower(p.extension().string());
    std::string stem = p.stem().string();
    std::vector<uint8_t> b;
    uint64_t fsz = 0;
    Ident id;
    bool ok = false;
    if (ext == ".nsp" || ext == ".nsz") {
        if (!read_at(p, 0, 16, &b, &fsz) || b.size() < 16 || std::memcmp(b.data(), "PFS0", 4) != 0) return false;
        uint64_t hdr = 16 + (uint64_t)rd32(b.data(), 4) * 0x18 + rd32(b.data(), 8);
        hdr = std::min<uint64_t>(hdr, 4u << 20);
        if (!read_at(p, 0, (size_t)hdr, &b)) return false;
        ok = parse_pfs0(b.data(), b.size(), &id);
        id.format = ext.substr(1);
        if (ok) id.title = clean_title(stem);
    } else if (ext == ".xci" || ext == ".xcz") {
        if (!read_at(p, 0, 0x1104, &b, &fsz)) return false;
        ok = parse_xci(b.data(), b.size(), &id);
        id.format = ext.substr(1);
        if (ok) id.title = clean_title(stem);
    } else if (ext == ".nup") {
        std::error_code ec;
        fsz = fs::file_size(p, ec);
        if (ec) return false;
        ok = parse_nup([&](uint64_t off, size_t len, std::vector<uint8_t>* o) { return read_at(p, off, len, o); }, fsz, &id);
        if (!ok) return false;
        if (id.title.empty()) id.title = clean_title(stem);
        if (!id.title_id) id.title_id = title_id_from_name(stem);
        *out = id;   // update_or_dlc is decided by the package kinds, not by the id bits
        return true;
    } else if (ext == ".nro") {
        if (!read_at(p, 0, 0x20, &b, &fsz)) return false;
        if (!parse_nro(b.data(), b.size(), fsz, &id)) return false;
        ok = true;
        uint64_t sz = rd32(b.data(), 0x18);
        std::vector<uint8_t> h;
        if (b.size() >= 0x1c && sz && sz + 0x38 <= fsz && read_at(p, sz, 0x38, &h) && h.size() == 0x38 && std::memcmp(h.data(), "ASET", 4) == 0) {
            uint64_t noff = rd64(h.data(), 0x18), nsz = rd64(h.data(), 0x20);
            if (nsz >= 0x200 && noff <= fsz && sz + noff + 0x200 <= fsz && noff < (64u << 20)) {
                std::vector<uint8_t> nacp;
                if (read_at(p, sz + noff, 0x200, &nacp) && nacp.size() == 0x200) {
                    std::string t = cstr(nacp.data(), 0x200);
                    if (!t.empty()) id.title = t;
                }
            }
        }
        if (id.title.empty()) id.title = clean_title(stem);
    } else if (p.filename().string() == "main" && lower(p.parent_path().filename().string()) == "exefs") {
        fs::path npdm = p.parent_path() / "main.npdm";
        if (!read_at(npdm, 0, 0x10000, &b)) return false;
        ok = parse_npdm(b.data(), b.size(), &id);
        const std::string top = p.parent_path().parent_path().filename().string();
        if (ok) {
            std::string folder = clean_title(top);
            if (!folder.empty()) id.title = folder;   // players name dumps; the NPDM name is a fallback
            if (!id.title_id) id.title_id = title_id_from_name(top);
        }
        if (ok) { id.update_or_dlc = false; finish(&id); }
        *out = id;
        return ok;
    }
    if (!ok) return false;
    if (!id.title_id) id.title_id = title_id_from_name(stem);
    finish(&id);
    *out = id;
    return true;
}

std::string key_for(uint64_t title_id, const std::string& sanitized_name) {
    if (!title_id) return "switch:file-" + sanitized_name;
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)title_id);
    return std::string("switch:") + buf;
}

uint64_t title_id_from_key(const std::string& key) {
    if (key.compare(0, 7, "switch:") != 0 || key.size() != 7 + 16) return 0;
    uint64_t v;
    return hex_run(key, 7, 16, &v) ? v : 0;
}

}  // namespace hub::nx
