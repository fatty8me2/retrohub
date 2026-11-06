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
