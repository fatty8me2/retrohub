#include "wiiu_meta.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include <zstd.h>

namespace hub {

namespace {

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint64_t be64(const uint8_t* p) { return (uint64_t(be32(p)) << 32) | be32(p + 4); }
uint32_t be16(const uint8_t* p) { return (uint32_t(p[0]) << 8) | p[1]; }

int hexv(char h) { return (h >= '0' && h <= '9') ? h - '0' : (h >= 'a' && h <= 'f') ? h - 'a' + 10 : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1; }

bool parse_hex16(const std::string& s, size_t at, uint64_t* out) {
    if (s.size() < at + 16) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < 16; ++i) {
        int d = hexv(s[at + i]);
        if (d < 0) return false;
        v = (v << 4) | uint64_t(d);
    }
    *out = v;
    return true;
}

std::string lower(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c + 32); return s; }

std::string hex8(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "%08x", v); return b; }

std::string with_sep(std::string s) { if (!s.empty() && s.back() != '\\' && s.back() != '/') s += '\\'; return s; }

void utf8_append(std::string& o, uint32_t cp) {
    if (cp < 0x80) o += char(cp);
    else if (cp < 0x800) { o += char(0xC0 | (cp >> 6)); o += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { o += char(0xE0 | (cp >> 12)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
    else { o += char(0xF0 | (cp >> 18)); o += char(0x80 | ((cp >> 12) & 0x3F)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
}

std::string xml_unescape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { o += s[i]; continue; }
        size_t e = s.find(';', i);
        if (e == std::string::npos || e - i > 10) { o += s[i]; continue; }
        std::string n = s.substr(i + 1, e - i - 1);
        if (n == "amp") o += '&';
        else if (n == "lt") o += '<';
        else if (n == "gt") o += '>';
        else if (n == "quot") o += '"';
        else if (n == "apos") o += '\'';
        else if (!n.empty() && n[0] == '#') {
            uint32_t cp = 0;
            bool ok = n.size() > 1;
            if (n.size() > 1 && (n[1] == 'x' || n[1] == 'X')) { for (size_t k = 2; k < n.size() && ok; ++k) { int d = hexv(n[k]); if (d < 0) ok = false; else cp = cp * 16 + uint32_t(d); } ok = ok && n.size() > 2; }
            else for (size_t k = 1; k < n.size() && ok; ++k) { if (n[k] < '0' || n[k] > '9') ok = false; else cp = cp * 10 + uint32_t(n[k] - '0'); }
            if (!ok || cp == 0 || cp > 0x10FFFF) { o += s[i]; continue; }
            utf8_append(o, cp);
        } else { o += s[i]; continue; }
        i = e;
    }
    return o;
}

std::string collapse_ws(const std::string& s) {
    std::string o;
    bool sp = false;
    for (char c : s) {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') { sp = true; continue; }
        if (sp && !o.empty()) o += ' ';
        sp = false;
        o += c;
    }
    return o;
}

/* Text of the first <tag ...>text</tag>; false when absent. */
bool tag_text(const std::string& xml, const std::string& tag, std::string* out) {
    size_t p = 0;
    for (;;) {
        p = xml.find("<" + tag, p);
        if (p == std::string::npos) return false;
        size_t after = p + 1 + tag.size();
        if (after < xml.size() && (xml[after] == ' ' || xml[after] == '>' || xml[after] == '/' || xml[after] == '\t' || xml[after] == '\n')) break;
        p = after;
    }
    size_t gt = xml.find('>', p);
    if (gt == std::string::npos) return false;
    if (xml[gt - 1] == '/') { out->clear(); return true; }
    size_t end = xml.find("</" + tag + ">", gt);
    if (end == std::string::npos) return false;
    *out = xml.substr(gt + 1, end - gt - 1);
    return true;
}

/* ---------------- ZArchive ---------------- */
struct ZArc {
    const ReadAtFn* read = nullptr;
    uint64_t size = 0;
    uint64_t data_off = 0, rec_off = 0, rec_sz = 0, names_off = 0, names_sz = 0, tree_off = 0, tree_sz = 0;

    bool open(const ReadAtFn& r, uint64_t file_size) {
        read = &r;
        size = file_size;
        if (!r || file_size < 144 + 16) return false;
        uint8_t f[144];
        if (!r(file_size - 144, f, sizeof f)) return false;
        if (be32(f + 140) != 0x169f52d6u || be32(f + 136) != 0x61bf3a01u || be64(f + 128) != file_size) return false;
        data_off = be64(f + 0);
        rec_off = be64(f + 16); rec_sz = be64(f + 24);
        names_off = be64(f + 32); names_sz = be64(f + 40);
        tree_off = be64(f + 48); tree_sz = be64(f + 56);
        if (tree_sz < 16 || tree_sz > (1ull << 31) || tree_off > file_size || tree_sz > file_size - tree_off) return false;
        if (names_off > file_size || names_sz > file_size - names_off) return false;
        if (rec_off > file_size || rec_sz > file_size - rec_off) return false;
        return true;
    }
    struct Node { bool file = false; uint32_t name_at = 0; uint32_t first = 0, count = 0; uint64_t off = 0, sz = 0; };
    bool node(uint32_t i, Node* n) const {
        if (uint64_t(i) * 16 + 16 > tree_sz) return false;
        uint8_t b[16];
        if (!(*read)(tree_off + uint64_t(i) * 16, b, 16)) return false;
        uint32_t w0 = be32(b);
        n->file = (w0 & 0x80000000u) != 0;
        n->name_at = w0 & 0x7FFFFFFFu;
        if (n->file) {
            n->off = uint64_t(be32(b + 4)) | (uint64_t(be16(b + 12)) << 32);
            n->sz = uint64_t(be32(b + 8)) | (uint64_t(be16(b + 14)) << 32);
        } else {
            n->first = be32(b + 4);
            n->count = be32(b + 8);
            if (n->count > 100000 || uint64_t(n->first) + n->count > tree_sz / 16) return false;
        }
        return true;
    }
    bool name(uint32_t at, std::string* out) const {
        if (at >= names_sz) return false;
        uint8_t nb[2 + 256] = {0};
        size_t want = size_t(std::min<uint64_t>(sizeof nb, names_sz - at));
        if (want == 0 || !(*read)(names_off + at, nb, want)) return false;
        size_t len = nb[0], skip = 1;
        if (len & 0x80) { if (want < 2) return false; len = ((len & 0x7F) << 8) | nb[1]; skip = 2; }
        if (skip + len > want) return false;
        out->assign(reinterpret_cast<char*>(nb) + skip, len);
        return true;
    }
    bool child(const Node& dir, const std::string& want, Node* out) const {
        std::string w = lower(want);
        for (uint32_t i = 0; i < dir.count; ++i) {
            Node n;
            std::string nm;
            if (!node(dir.first + i, &n) || !name(n.name_at, &nm)) continue;
            if (lower(nm) == w) { *out = n; return true; }
        }
        return false;
    }
    /* Uncompressed data stream bytes [off, off+n). Blocks are 64 KiB, zstd-compressed unless stored raw. */
    bool read_stream(uint64_t off, uint64_t n, std::string* out) const {
        out->clear();
        if (n == 0) return true;
        const uint64_t kBlock = 65536;
        std::vector<uint8_t> comp, dec(kBlock);
        for (uint64_t b = off / kBlock; b <= (off + n - 1) / kBlock; ++b) {
            uint64_t rec = b / 16;
            if ((rec + 1) * 40 > rec_sz) return false;
            uint8_t r[40];
            if (!(*read)(rec_off + rec * 40, r, 40)) return false;
            uint64_t coff = be64(r);
            uint32_t idx = uint32_t(b % 16);
            for (uint32_t j = 0; j < idx; ++j) coff += uint64_t(be16(r + 8 + j * 2)) + 1;
            uint64_t clen = uint64_t(be16(r + 8 + idx * 2)) + 1;
            if (data_off + coff + clen > size) return false;
            comp.resize(size_t(clen));
            if (!(*read)(data_off + coff, comp.data(), size_t(clen))) return false;
            size_t got = 0;
            if (clen == kBlock) { std::memcpy(dec.data(), comp.data(), size_t(clen)); got = size_t(clen); }
            else {
                size_t d = ZSTD_decompress(dec.data(), dec.size(), comp.data(), comp.size());
                if (ZSTD_isError(d)) { std::memcpy(dec.data(), comp.data(), size_t(clen)); got = size_t(clen); }   // last block stored raw
                else got = d;
            }
            uint64_t bstart = b * kBlock, from = off > bstart ? off - bstart : 0, to = std::min<uint64_t>(got, off + n - bstart);
            if (from >= to) return false;
            out->append(reinterpret_cast<char*>(dec.data()) + from, size_t(to - from));
        }
        return out->size() == n;
    }
};

bool root_node(const ZArc& z, ZArc::Node* root) { return z.node(0, root) && !root->file; }

WuaTitle classify(const std::string& dir) {
    WuaTitle t;
    t.dir = dir;
    uint64_t id = 0;
    if (parse_hex16(dir, 0, &id)) {
        t.title_id = id;
        uint32_t hi = uint32_t(id >> 32);
        t.kind = hi == 0x00050000u ? WuaTitle::Base : hi == 0x0005000Eu ? WuaTitle::Update : hi == 0x0005000Cu ? WuaTitle::Dlc : WuaTitle::Other;
    }
    size_t v = dir.find("_v");
    if (v != std::string::npos && v + 2 < dir.size()) {
        int n = 0;
        bool ok = true;
        for (size_t i = v + 2; i < dir.size(); ++i) { if (dir[i] < '0' || dir[i] > '9') { ok = false; break; } n = n * 10 + (dir[i] - '0'); if (n > 1000000) break; }
        if (ok) t.version = n;
    }
    return t;
}

}  // namespace

/* ---------------- meta.xml ---------------- */

const char* wiiu_region_name(char r) {
    switch (r) {
    case 'E': case 'e': return "USA";
    case 'P': case 'p': return "Europe";
    case 'J': case 'j': return "Japan";
    case 'K': case 'k': return "Korea";
    case 'A': case 'a': return "Australia";
    case 'U': case 'u': return "Australia";
    case 'C': case 'c': return "China";
    case 'T': case 't': return "Taiwan";
    default: return "";
    }
}

bool parse_wiiu_meta_xml(const std::string& xml, WiiuMeta* out) {
    if (!out || xml.find("<menu") == std::string::npos) return false;
    WiiuMeta m;
    std::string t;
    if (tag_text(xml, "title_id", &t)) {
        std::string c = collapse_ws(t);
        parse_hex16(c, 0, &m.title_id);
    }
    auto name_for = [&](const char* base) {
        std::string pref;
        if (tag_text(xml, std::string(base) + "_en", &pref)) { pref = collapse_ws(xml_unescape(pref)); if (!pref.empty()) return pref; }
        static const char* langs[] = {"ja", "fr", "de", "it", "es", "zhs", "ko", "nl", "pt", "ru", "zht"};
        for (const char* l : langs) { std::string v; if (tag_text(xml, std::string(base) + "_" + l, &v)) { v = collapse_ws(xml_unescape(v)); if (!v.empty()) return v; } }
        return std::string();
    };
    m.longname = name_for("longname");
    if (m.longname.empty()) m.longname = name_for("shortname");
    m.publisher = name_for("publisher");
    if (tag_text(xml, "product_code", &t)) {
        m.product_code = collapse_ws(xml_unescape(t));
        size_t d = m.product_code.rfind('-');
        if (d != std::string::npos && m.product_code.size() - d - 1 == 4) {
            m.game_id4 = m.product_code.substr(d + 1);
            m.region = m.game_id4[3];
        }
    }
    if (m.longname.empty() && !m.title_id) return false;
    *out = m;
    return true;
}

/* ---------------- .wua ---------------- */

std::vector<WuaTitle> wua_titles(const ReadAtFn& read, uint64_t file_size) {
    std::vector<WuaTitle> v;
    ZArc z;
    ZArc::Node root;
    if (!z.open(read, file_size) || !root_node(z, &root)) return v;
    for (uint32_t i = 0; i < root.count && i < 4096; ++i) {
        ZArc::Node n;
        std::string nm;
        if (!z.node(root.first + i, &n) || n.file || !z.name(n.name_at, &nm)) continue;
        WuaTitle t = classify(nm);
        if (t.title_id) v.push_back(t);
    }
    return v;
}

bool wua_read_file(const ReadAtFn& read, uint64_t file_size, const std::string& path, std::string* out, size_t max_bytes) {
    ZArc z;
    ZArc::Node cur;
    if (!z.open(read, file_size) || !root_node(z, &cur)) return false;
    size_t p = 0;
    while (p <= path.size()) {
        size_t q = path.find('/', p);
        std::string part = path.substr(p, q == std::string::npos ? std::string::npos : q - p);
        p = q == std::string::npos ? path.size() + 1 : q + 1;
        if (part.empty()) continue;
        if (cur.file) return false;
        ZArc::Node nx;
        if (!z.child(cur, part, &nx)) return false;
        cur = nx;
    }
    if (!cur.file || cur.sz > max_bytes) return false;
    return z.read_stream(cur.off, cur.sz, out);
}

bool wua_read_meta(const ReadAtFn& read, uint64_t file_size, WiiuMeta* out) {
    std::vector<WuaTitle> ts = wua_titles(read, file_size);
    std::stable_sort(ts.begin(), ts.end(), [](const WuaTitle& a, const WuaTitle& b) { return (a.kind == WuaTitle::Base) > (b.kind == WuaTitle::Base); });
    for (const WuaTitle& t : ts) {
        std::string xml;
        WiiuMeta m;
        if (wua_read_file(read, file_size, t.dir + "/meta/meta.xml", &xml) && parse_wiiu_meta_xml(xml, &m)) {
            if (!m.title_id) m.title_id = t.title_id;
            *out = m;
            return true;
        }
    }
    return false;
}

/* ---------------- updates / DLC ---------------- */

int parse_app_xml_version(const std::string& xml) {
    std::string t;
    if (!tag_text(xml, "title_version", &t)) return -1;
    t = collapse_ws(t);
    if (t.empty() || t.size() > 8) return -1;
    int v = 0;
    for (char c : t) { int d = hexv(c); if (d < 0) return -1; v = v * 16 + d; }
    return v;
}

WiiuAddons wiiu_installed_addons(uint64_t base, const std::string& mlc, const FsProbe& fs) {
    WiiuAddons a;
    if (!base || mlc.empty() || !fs.dir_exists) return a;
    std::string low = hex8(uint32_t(base & 0xFFFFFFFFu)), root = with_sep(mlc) + "usr\\title\\";
    auto probe = [&](const char* hi, bool* have, int* ver) {
        std::string d = root + hi + "\\" + low;
        if (!fs.dir_exists(d)) return;
        *have = true;
        if (fs.read_text) *ver = parse_app_xml_version(fs.read_text(d + "\\code\\app.xml"));
    };
    probe("0005000e", &a.update, &a.update_version);
    probe("0005000c", &a.dlc, &a.dlc_version);
    return a;
}

void wiiu_addons_from_wua(const std::vector<WuaTitle>& titles, WiiuAddons* a) {
    for (const WuaTitle& t : titles) {
        if (t.kind == WuaTitle::Update) { a->update_in_archive = true; a->archive_update_version = std::max(a->archive_update_version, t.version); }
        if (t.kind == WuaTitle::Dlc) a->dlc_in_archive = true;
    }
}

std::string wiiu_addons_text(const WiiuAddons& a) {
    std::vector<std::string> p;
    auto ver = [](int v) { return v >= 0 ? " v" + std::to_string(v) : std::string(); };
    if (a.update_in_archive) p.push_back("Update" + ver(a.archive_update_version) + " (inside the .wua)");
    if (a.update) p.push_back("Update" + ver(a.update_version) + " (installed in Cemu)");
    if (a.dlc_in_archive) p.push_back("DLC (inside the .wua)");
    if (a.dlc) p.push_back("DLC" + ver(a.dlc_version) + " (installed in Cemu)");
    if (p.empty()) return "No update or DLC found";
    std::string o;
    for (size_t i = 0; i < p.size(); ++i) o += (i ? ", " : "") + p[i];
    return o;
}

/* ---------------- keys ---------------- */

WiiuKeys wiiu_keys_check(const std::string& user_data, const std::string& exe_dir, const FsProbe& fs) {
    WiiuKeys k;
    k.expected_path = with_sep(user_data) + "keys.txt";
    for (const std::string& c : {k.expected_path, with_sep(exe_dir) + "keys.txt"}) {
        if (c.size() <= 9 || !fs.file_exists || !fs.file_exists(c)) continue;
        k.found_path = c;
        std::string text = fs.read_text ? fs.read_text(c) : std::string();
        size_t p = 0;
        while (p < text.size()) {
            size_t e = text.find('\n', p);
            if (e == std::string::npos) e = text.size();
            std::string line = text.substr(p, e - p);
            p = e + 1;
            size_t s = line.find_first_not_of(" \t\r");
            if (s == std::string::npos || line[s] == '#') continue;
            int digits = 0;
            size_t i = s;
            while (i < line.size() && hexv(line[i]) >= 0) { ++digits; ++i; }
            if (digits == 32) ++k.key_lines;
        }
        break;
    }
    return k;
}

std::string wiiu_keys_message(const WiiuKeys& k, const std::string& container) {
    if (container != "wud" && container != "wux") return std::string();
    if (k.found_path.empty())
        return "This disc image (." + container + ") is encrypted. Cemu needs the disc key from your own console in a keys.txt file; none was found. Put it at " +
               k.expected_path + " (one 32-digit key per line, optionally followed by '# the game name'). A .wua archive needs no keys.";
    if (k.key_lines == 0)
        return "keys.txt exists (" + k.found_path + ") but has no 32-digit key lines. Add your disc key as '<32 hex digits> # game name'. A .wua archive needs no keys.";
    return std::string();   // a key file with keys: Cemu tries them all; nothing to explain
}

/* ---------------- saves ---------------- */

std::vector<std::string> wiiu_save_rels(uint64_t base) {
    uint32_t low = uint32_t(base & 0xFFFFFFFFu);
    return {"usr/save/00050000/" + hex8(low), "usr/save/0005000e/" + hex8(low)};
}

std::vector<std::string> wiiu_save_accounts(const std::string& title_save_dir) {
    std::vector<std::string> v;
    std::error_code ec;
    namespace fs = std::filesystem;
    fs::path u = fs::u8path(with_sep(title_save_dir) + "user");
    for (fs::directory_iterator it(u, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_directory(e2)) continue;
        std::string n = it->path().filename().u8string();
        if (n != "common") v.push_back(n);
    }
    std::sort(v.begin(), v.end());
    return v;
}

/* ---------------- game profiles ---------------- */

std::string wiiu_profile_file_name(uint64_t id) {
    char b[40];
    std::snprintf(b, sizeof b, "%08x%08x.ini", uint32_t(id >> 32), uint32_t(id & 0xFFFFFFFFu));
    return b;
}

namespace {
struct IniLine { size_t begin, end; };   // [begin,end) without the line break
}

std::string ini_get(const std::string& text, const std::string& section, const std::string& key) {
    std::string cur;
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(p, e - p);
        p = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t s = line.find_first_not_of(" \t");
        if (s == std::string::npos || line[s] == '#' || line[s] == ';') continue;
        if (line[s] == '[') { size_t c = line.find(']', s); cur = c == std::string::npos ? std::string() : line.substr(s + 1, c - s - 1); continue; }
        if (lower(cur) != lower(section)) continue;
        size_t eq = line.find('=', s);
        if (eq == std::string::npos) continue;
        std::string k = line.substr(s, eq - s), v = line.substr(eq + 1);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        size_t vs = v.find_first_not_of(" \t");
        v = vs == std::string::npos ? std::string() : v.substr(vs);
        if (lower(k) == lower(key)) return v;
    }
    return std::string();
}

std::string ini_put(const std::string& text, const std::string& section, const std::string& key, const std::string& value) {
    std::string out;
    std::string cur;
    bool in_sec = false, done = false, sec_seen = false;
    size_t p = 0;
    auto flush_missing = [&]() { if (in_sec && !done) { out += key + " = " + value + "\n"; done = true; } };
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        bool had_nl = e != std::string::npos;
        if (!had_nl) e = text.size();
        std::string line = text.substr(p, e - p);
        p = e + 1;
        std::string t = line;
        while (!t.empty() && (t.back() == '\r' || t.back() == ' ')) t.pop_back();
        size_t s = t.find_first_not_of(" \t");
        if (s != std::string::npos && t[s] == '[') {
            flush_missing();
            size_t c = t.find(']', s);
            std::string name = c == std::string::npos ? std::string() : t.substr(s + 1, c - s - 1);
            in_sec = lower(name) == lower(section);
            if (in_sec) sec_seen = true;
        } else if (in_sec && !done && s != std::string::npos && t[s] != '#' && t[s] != ';') {
            size_t eq = t.find('=', s);
            if (eq != std::string::npos) {
                std::string k = t.substr(s, eq - s);
                while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
                if (lower(k) == lower(key)) { out += k + " = " + value + "\n"; done = true; continue; }
            }
        }
        out += line;
        if (had_nl || p <= text.size()) out += "\n";
    }
    flush_missing();
    if (!sec_seen) {
        if (!out.empty() && out.back() != '\n') out += "\n";
        if (!out.empty()) out += "\n";
        out += "[" + section + "]\n" + key + " = " + value + "\n";
    }
    return out;
}

std::string wiiu_profile_value(const std::string& def, const std::string& user, const std::string& section, const std::string& key) {
    std::string v = ini_get(user, section, key);
    return v.empty() ? ini_get(def, section, key) : v;
}

const char* wiiu_cpu_mode_ini(const std::string& c) {
    if (c == "single-interp") return "Singlecore-Interpreter";
    if (c == "single-rec") return "Singlecore-Recompiler";
    if (c == "dual-rec") return "Dualcore-Recompiler";
    if (c == "triple-rec") return "Triplecore-Recompiler";
    return "";
}

}  // namespace hub
