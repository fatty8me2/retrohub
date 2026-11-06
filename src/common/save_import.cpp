#include "save_import.h"
#include "gba_save.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <system_error>

namespace hub::saveimport {

namespace fs = std::filesystem;

namespace {

fs::path P(const std::string& s) { return fs::u8path(s); }
std::string U8(const fs::path& p) { auto s = p.u8string(); return std::string(s.begin(), s.end()); }

std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}
std::string upper(std::string s) {
    for (char& c : s) if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    return s;
}
bool starts_with_ci(const std::string& s, const std::string& pre) {
    return s.size() >= pre.size() && lower(s.substr(0, pre.size())) == lower(pre);
}
std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n' || s.back() == '\t' || s.back() == '"')) s.pop_back();
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '"')) ++a;
    return s.substr(a);
}
std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '\\' || a.back() == '/') return a + b;
    return a + "\\" + b;
}

bool read_file(const std::string& path, std::string* out, size_t max = 64u << 20) {
    std::ifstream f(P(path), std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    if (n < 0 || (size_t)n > max) return false;
    f.seekg(0);
    out->assign((size_t)n, '\0');
    if (n) f.read(&(*out)[0], n);
    return (bool)f || n == 0;
}

int64_t mtime_unix(const fs::path& p) {
    std::error_code ec;
    auto ft = fs::last_write_time(p, ec);
    if (ec) return 0;
    auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return (int64_t)std::chrono::system_clock::to_time_t(sys);
}

bool is_dir(const std::string& p) { std::error_code ec; return !p.empty() && fs::is_directory(P(p), ec); }
bool is_file(const std::string& p) { std::error_code ec; return !p.empty() && fs::is_regular_file(P(p), ec); }

uint16_t be16(const std::string& s, size_t o) { return (uint16_t)(((uint8_t)s[o] << 8) | (uint8_t)s[o + 1]); }
void put_be16(std::string& s, size_t o, uint16_t v) { s[o] = char(v >> 8); s[o + 1] = char(v & 0xFF); }
void put_be32(std::string& s, size_t o, uint32_t v) { s[o] = char(v >> 24); s[o + 1] = char(v >> 16); s[o + 2] = char(v >> 8); s[o + 3] = char(v); }

bool all_same(const std::string& s, size_t off, size_t len) {
    if (off + len > s.size() || len == 0) return true;
    for (size_t i = 1; i < len; ++i) if (s[off + i] != s[off]) return false;
    return true;
}

std::string sanitize_core(std::string s) {   // main.c string_replace_chars(":<>\"/\\|?*", '_')
    for (char& c : s) if (std::strchr(":<>\"/\\|?*", c) && c) c = '_';
    return s;
}

}  // namespace

const char* source_name(Source s) {
    switch (s) {
        case Source::RetroArch: return "RetroArch";
        case Source::Project64: return "Project64";
        case Source::Dolphin: return "Dolphin";
        case Source::PCSX2: return "PCSX2";
        case Source::MGBA: return "mGBA";
        case Source::VBA: return "VisualBoyAdvance";
        case Source::MelonDS: return "melonDS";
        case Source::NextToRom: return "Next to the game";
        case Source::Picked: return "Picked folder";
    }
    return "";
}
const char* how_name(How h) {
    switch (h) {
        case How::Stem: return "same file name";
        case How::Title: return "same title";
        case How::Content: return "found in the save";
        case How::Beside: return "next to the game";
    }
    return "";
}

std::string pcsx2_safe_key(const std::string& key) {
    std::string k = key;
    for (char& ch : k) if (!isalnum((unsigned char)ch) && ch != '-' && ch != '_' && ch != '.') ch = '_';
    return k;
}

/* ------------------------------------------------------------------ config readers */

RaCfg parse_retroarch_cfg(const std::string& text, const std::string& ra_dir) {
    RaCfg c;
    auto resolve = [&](std::string v, const char* dflt) -> std::string {
        v = trim(v);
        if (v.empty() || lower(v) == "default") return join(ra_dir, dflt);
        if (v[0] == ':') { v = v.substr(1); while (!v.empty() && (v[0] == '\\' || v[0] == '/')) v.erase(0, 1); return join(ra_dir, v); }
        if (v.size() >= 2 && v[1] == ':') return v;                       // C:\...
        if (v.size() >= 2 && v[0] == '\\' && v[1] == '\\') return v;       // UNC
        if (v[0] == '~') return v;                                         // cannot resolve here
        return join(ra_dir, v);
    };
    std::string sv, st;
    bool have_sv = false, have_st = false;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t e = text.find('\n', pos);
        std::string line = text.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        pos = e == std::string::npos ? text.size() + 1 : e + 1;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = line.substr(eq + 1);
        if (k == "savefile_directory") { sv = v; have_sv = true; }
        else if (k == "savestate_directory") { st = v; have_st = true; }
        else if (k == "savefiles_in_content_dir") c.saves_in_content_dir = trim(v) == "true";
    }
    c.savefile_dir = resolve(have_sv ? sv : "", "saves");
    c.savestate_dir = resolve(have_st ? st : "", "states");
    return c;
}

std::string parse_mgba_savegame_path(const std::string& text) {
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t e = text.find('\n', pos);
        std::string line = text.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        pos = e == std::string::npos ? text.size() + 1 : e + 1;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (trim(line.substr(0, eq)) == "savegamePath") return trim(line.substr(eq + 1));
    }
    return "";
}

/* ------------------------------------------------------------------ detection */

std::vector<Folder> detect_folders(const Env& env) {
    std::vector<Folder> out;
    std::set<std::string> seen;
    const std::string own = lower(env.own_data_dir);
    auto add = [&](Source s, const std::string& dir) {
        if (dir.empty() || !is_dir(dir)) return;
        std::string key = lower(U8(P(dir).lexically_normal()));
        while (!key.empty() && (key.back() == '\\' || key.back() == '/')) key.pop_back();
        if (!own.empty() && key.rfind(own.substr(0, own.size() - ((own.back() == '\\' || own.back() == '/') ? 1 : 0)), 0) == 0) return;
        if (!seen.insert(key).second) return;
        out.push_back({s, dir});
    };
    /* subfolders of `parent` whose name starts with `prefix` (case-insensitive) */
    auto kids = [&](const std::string& parent, const std::string& prefix) {
        std::vector<std::string> v;
        std::error_code ec;
        if (parent.empty() || !is_dir(parent)) return v;
        for (fs::directory_iterator it(P(parent), ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_directory(e2)) continue;
            std::string n = U8(it->path().filename());
            if (starts_with_ci(n, prefix)) v.push_back(U8(it->path()));
        }
        return v;
    };
    const std::string sd = env.system_drive.empty() ? std::string() : join(env.system_drive, "");
    std::vector<std::string> parents;
    for (const std::string& p : {sd, env.programfiles, env.programfiles_x86, env.userprofile, join(env.userprofile, "Desktop"),
                                 join(env.userprofile, "Downloads"), env.documents, env.localappdata.empty() ? std::string() : join(env.localappdata, "Programs"),
                                 env.programfiles.empty() ? std::string() : join(join(env.programfiles, "Steam"), "steamapps\\common"),
                                 env.programfiles_x86.empty() ? std::string() : join(join(env.programfiles_x86, "Steam"), "steamapps\\common")})
        if (!p.empty()) parents.push_back(p);

    /* RetroArch */
    {
        std::vector<std::string> bases = {join(env.appdata, "RetroArch"), join(env.localappdata, "RetroArch"), join(env.userprofile, "RetroArch")};
        for (auto& p : parents) for (auto& k : kids(p, "RetroArch")) bases.push_back(k);
        for (auto& b : bases) {
            if (b.empty() || !is_dir(b)) continue;
            std::string cfg;
            RaCfg c;
            if (read_file(join(b, "retroarch.cfg"), &cfg, 4u << 20)) c = parse_retroarch_cfg(cfg, b);
            else c = parse_retroarch_cfg("", b);
            add(Source::RetroArch, c.savefile_dir);
            add(Source::RetroArch, join(b, "saves"));
        }
    }
    /* Project64 */
    {
        std::vector<std::string> bases;
        for (auto& p : parents) for (auto& k : kids(p, "Project64")) bases.push_back(k);
        for (auto& k : kids(env.appdata, "Project64")) bases.push_back(k);
        for (auto& k : kids(env.localappdata, "Project64")) bases.push_back(k);
        for (auto& b : bases) { add(Source::Project64, join(b, "Save")); add(Source::Project64, join(b, "Saves")); }
    }
    /* Dolphin */
    {
        add(Source::Dolphin, join(join(env.appdata, "Dolphin Emulator"), "GC"));
        add(Source::Dolphin, join(join(env.documents, "Dolphin Emulator"), "GC"));
        add(Source::Dolphin, join(join(env.userprofile, "Documents\\Dolphin Emulator"), "GC"));
        for (auto& p : parents) for (auto& k : kids(p, "Dolphin")) { add(Source::Dolphin, join(join(k, "User"), "GC")); add(Source::Dolphin, join(k, "GC")); }
    }
    /* PCSX2 */
    {
        add(Source::PCSX2, join(join(env.documents, "PCSX2"), "memcards"));
        add(Source::PCSX2, join(join(env.userprofile, "Documents\\PCSX2"), "memcards"));
        add(Source::PCSX2, join(join(env.appdata, "PCSX2"), "memcards"));
        for (auto& p : parents) for (auto& k : kids(p, "PCSX2")) add(Source::PCSX2, join(k, "memcards"));
    }
    /* mGBA */
    {
        std::string ini;
        for (const std::string& d : {join(env.appdata, "mGBA"), join(env.documents, "mGBA"), join(join(env.userprofile, "Documents"), "mGBA")}) {
            if (read_file(join(d, "config.ini"), &ini, 1u << 20)) {
                std::string sp = parse_mgba_savegame_path(ini);
                if (!sp.empty()) add(Source::MGBA, sp);
            }
            add(Source::MGBA, d);
        }
        for (auto& p : parents) for (auto& k : kids(p, "mGBA")) add(Source::MGBA, k);
    }
    /* VisualBoyAdvance / VBA-M */
    {
        for (auto& p : parents) {
            for (auto& k : kids(p, "VisualBoyAdvance")) add(Source::VBA, k);
            for (auto& k : kids(p, "VBA")) add(Source::VBA, k);
            for (auto& k : kids(p, "vbam")) add(Source::VBA, k);
        }
        for (auto& k : kids(env.appdata, "VBA")) add(Source::VBA, k);
        for (auto& k : kids(env.appdata, "vbam")) add(Source::VBA, k);
    }
    /* melonDS */
    {
        add(Source::MelonDS, join(env.appdata, "melonDS"));
        add(Source::MelonDS, join(env.documents, "melonDS"));
        for (auto& p : parents) for (auto& k : kids(p, "melonDS")) add(Source::MelonDS, k);
    }
    return out;
}

/* ------------------------------------------------------------------ scanning */

namespace {
std::vector<std::string> exts_for(Source s) {
    switch (s) {
        case Source::RetroArch: return {".srm"};
        case Source::Project64: return {".eep", ".sra", ".fla", ".mpk"};
        case Source::Dolphin: return {".gci", ".raw", ".gcp", ".mcd"};
        case Source::PCSX2: return {".ps2"};
        case Source::MGBA: case Source::VBA: case Source::MelonDS: return {".sav"};
        default: return {".srm", ".sav", ".eep", ".sra", ".fla", ".mpk", ".gci", ".raw", ".gcp", ".mcd", ".ps2"};
    }
}
int depth_for(Source s) {
    switch (s) {
        case Source::RetroArch: return 3;
        case Source::Dolphin: return 5;
        case Source::Picked: return 4;
        case Source::MGBA: case Source::VBA: case Source::MelonDS: return 2;
        default: return 1;
    }
}
bool add_found(std::vector<Found>* v, Source s, const fs::path& p, const std::vector<std::string>& exts) {
    std::string ext = lower(U8(p.extension()));
    if (std::find(exts.begin(), exts.end(), ext) == exts.end()) return false;
    std::error_code ec;
    Found f;
    f.src = s;
    f.path = U8(p);
    f.name = U8(p.filename());
    f.ext = ext;
    f.size = (int64_t)fs::file_size(p, ec);
    if (ec) return false;
    f.mtime = mtime_unix(p);
    v->push_back(f);
    return true;
}
}  // namespace

std::vector<Found> scan_folder(const Folder& f, size_t max_files) {
    std::vector<Found> out;
    std::error_code ec;
    if (!is_dir(f.dir)) return out;
    const auto exts = exts_for(f.src);
    const int maxd = depth_for(f.src);
    for (fs::recursive_directory_iterator it(P(f.dir), fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_directory(e2)) {
            if (lower(U8(it->path().filename())) == "retrohub" || it.depth() + 1 >= maxd) it.disable_recursion_pending();   // RetroHub's own cards are not a source
            continue;
        }
        if (!it->is_regular_file(e2)) continue;
        add_found(&out, f.src, it->path(), exts);
        if (out.size() >= max_files) break;
    }
    return out;
}

bool found_from_path(const std::string& path, Source src, Found* out) {
    std::vector<Found> v;
    if (!is_file(path) || !add_found(&v, src, P(path), exts_for(Source::Picked))) return false;
    *out = v[0];
    return true;
}

std::vector<Found> scan_next_to_rom(const std::string& rom_path, const std::string& stem) {
    std::vector<Found> out;
    fs::path dir = P(rom_path).parent_path();
    if (dir.empty()) return out;
    const auto exts = exts_for(Source::NextToRom);
    std::set<std::string> seen;
    for (const fs::path& d : {dir, dir / "saves", dir / "Saves", dir / "save"}) {
        std::error_code ec;
        if (!fs::is_directory(d, ec)) continue;
        if (!seen.insert(lower(U8(d.lexically_normal()))).second) continue;   // "saves" and "Saves" are one folder on Windows
        for (const std::string& e : exts) {
            fs::path p = d / P(stem + e);
            if (fs::is_regular_file(p, ec)) add_found(&out, Source::NextToRom, p, exts);
        }
    }
    return out;
}

int count_files_with_ext(const std::string& dir, const std::string& ext_prefix) {
    int n = 0;
    std::error_code ec;
    if (!is_dir(dir)) return 0;
    for (fs::recursive_directory_iterator it(P(dir), fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2) && starts_with_ci(lower(U8(it->path().extension())), ext_prefix)) ++n;
        if (n > 9999) break;
    }
    return n;
}

/* ------------------------------------------------------------------ names */

std::string norm_name(const std::string& s) {
    std::string out;
    int depth = 0;
    for (char c : s) {
        if (c == '(' || c == '[') { ++depth; continue; }
        if (c == ')' || c == ']') { if (depth) --depth; continue; }
        if (depth) continue;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')) out.push_back(c);
        else if (c >= 'A' && c <= 'Z') out.push_back(char(c - 'A' + 'a'));
    }
    if (out.empty()) for (char c : s) if (isalnum((unsigned char)c)) out.push_back((char)tolower((unsigned char)c));
    return out;
}

std::string strip_save_suffix(const std::string& stem, int* controller) {
    std::string s = stem;
    if (controller) *controller = 0;
    std::string l = lower(s);
    size_t k = l.rfind("_cont_");
    if (k != std::string::npos && k + 7 == l.size() && l[k + 6] >= '1' && l[k + 6] <= '4') {
        if (controller) *controller = l[k + 6] - '1';
        s = s.substr(0, k);
    }
    if (s.size() > 9 && s[s.size() - 9] == '-') {
        bool hex = true;
        for (size_t i = s.size() - 8; i < s.size(); ++i) if (!isxdigit((unsigned char)s[i])) hex = false;
        if (hex) s = s.substr(0, s.size() - 9);
    }
    return s;
}

/* ------------------------------------------------------------------ N64 naming and formats */

std::string ini_goodname(const std::string& ini, const std::string& md5) {
    if (md5.empty()) return "";
    std::string want = upper(md5);
    for (int hop = 0; hop < 2; ++hop) {
        std::string tag = "[" + want + "]";
        size_t p = 0;
        bool found = false;
        while ((p = ini.find(tag, p)) != std::string::npos) {
            if (p == 0 || ini[p - 1] == '\n') { found = true; break; }
            p += tag.size();
        }
        if (!found) return "";
        size_t body = p + tag.size();
        size_t next = ini.find("\n[", body);
        std::string sec = ini.substr(body, next == std::string::npos ? std::string::npos : next - body);
        std::string ref;
        size_t pos = 0;
        while (pos <= sec.size()) {
            size_t e = sec.find('\n', pos);
            std::string line = sec.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
            pos = e == std::string::npos ? sec.size() + 1 : e + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.rfind("GoodName=", 0) == 0) return line.substr(9);
            if (line.rfind("RefMD5=", 0) == 0) ref = upper(line.substr(7));
        }
        if (ref.empty()) return "";
        want = ref;
    }
    return "";
}

std::string n64_header_name(const std::string& b) {
    if (b.size() < 0x34) return "";
    std::string z = b.substr(0, 0x34);
    const unsigned char b0 = (unsigned char)z[0], b1 = (unsigned char)z[1];
    if (b0 == 0x80 && b1 == 0x37) {}                                                   // .z64
    else if (b0 == 0x37 && b1 == 0x80) { for (size_t i = 0; i + 1 < z.size(); i += 2) std::swap(z[i], z[i + 1]); }    // .v64
    else if (b0 == 0x40 && b1 == 0x12) { for (size_t i = 0; i + 3 < z.size(); i += 4) { std::swap(z[i], z[i + 3]); std::swap(z[i + 1], z[i + 2]); } }   // .n64
    else return "";
    std::string n = z.substr(0x20, 20);
    size_t nul = n.find('\0');
    if (nul != std::string::npos) n.resize(nul);
    while (!n.empty() && (n.back() == ' ' || n.back() == '\t' || n.back() == '\r' || n.back() == '\n')) n.pop_back();
    return n;
}

std::string n64_save_base(const std::string& md5, const std::string& goodname, const std::string& header) {
    std::string m8 = upper(md5).substr(0, 8);
    if (md5.size() < 8) return "";
    if (!goodname.empty()) return sanitize_core(goodname.substr(0, 32) + "-" + m8);
    if (!header.empty()) return sanitize_core(header + "-" + m8);
    return "";
}

/* ------------------------------------------------------------------ Controller Pak */

std::string blank_mempak(const std::string& seed) {
    std::string m(32768, '\0');
    uint32_t serial[6];
    uint64_t h = 1469598103934665603ull;
    for (char c : seed) { h ^= (unsigned char)c; h *= 1099511628211ull; }
    for (int i = 0; i < 6; ++i) { h ^= (uint64_t)(i + 1); h *= 1099511628211ull; serial[i] = (uint32_t)(h >> 16); }
    std::string id(32, '\0');
    for (int i = 0; i < 6; ++i) put_be32(id, (size_t)i * 4, serial[i]);
    put_be16(id, 24, 0x0001);    // device id
    id[26] = 0x01;               // banks
    id[27] = 0x00;               // version
    uint16_t acc = 0;
    for (size_t i = 0; i < 28; i += 2) acc = (uint16_t)(acc + be16(id, i));
    put_be16(id, 28, acc);
    put_be16(id, 30, (uint16_t)(0xFFF2 - acc));
    for (size_t blk : {1, 3, 4, 6}) std::memcpy(&m[blk * 32], id.data(), 32);
    // page 1: inode table, first 5 reserved, the rest empty (0x0003); byte 1 holds the checksum of the 123 normal entries
    for (size_t i = 5; i < 128; ++i) put_be16(m, 256 + 2 * i, 0x0003);
    unsigned sum = 0;
    for (size_t i = 10; i < 256; ++i) sum += (unsigned char)m[256 + i];
    m[256 + 1] = (char)(sum & 0xFF);
    std::memcpy(&m[512], &m[256], 256);   // page 2 = backup of page 1
    return m;
}

bool mempak_has_notes(const std::string& pak) {
    if (pak.size() != 32768) return false;
    if (all_same(pak, 32, 32)) return false;   // no ID block: not a formatted pak
    for (size_t i = 5; i < 128; ++i) if (be16(pak, 256 + 2 * i) != 0x0003) return true;
    return false;
}

/* ------------------------------------------------------------------ GameCube raw card */

namespace {
const size_t kBlk = 0x2000;
}

bool gc_card_entries(const std::string& raw, std::vector<GcEntry>* out) {
    if (raw.size() < 5 * kBlk + kBlk || raw.size() % kBlk) return false;
    const int nblocks = (int)(raw.size() / kBlk);
    int16_t diff = (int16_t)(be16(raw, 1 * kBlk + 0x1FFA) - be16(raw, 2 * kBlk + 0x1FFA));   // update counters wrap
    const int dir = diff >= 0 ? 1 : 2;
    out->clear();
    for (int i = 0; i < 127; ++i) {
        size_t off = (size_t)dir * kBlk + (size_t)i * 0x40;
        if ((unsigned char)raw[off] == 0xFF && (unsigned char)raw[off + 1] == 0xFF && (unsigned char)raw[off + 2] == 0xFF && (unsigned char)raw[off + 3] == 0xFF) continue;
        GcEntry e;
        e.gamecode = raw.substr(off, 4);
        e.maker = raw.substr(off + 4, 2);
        e.filename = raw.substr(off + 8, 32);
        size_t z = e.filename.find('\0');
        if (z != std::string::npos) e.filename.resize(z);
        e.first = be16(raw, off + 0x36);
        e.count = be16(raw, off + 0x38);
        e.off = off;
        if (e.first < 5 || e.count < 1 || e.first + e.count > nblocks) continue;
        out->push_back(e);
    }
    return true;
}

bool gc_card_export_gci(const std::string& raw, const GcEntry& e, std::string* gci) {
    if (raw.size() < 6 * kBlk || raw.size() % kBlk) return false;
    const int nblocks = (int)(raw.size() / kBlk);
    int16_t diff = (int16_t)(be16(raw, 3 * kBlk + 4) - be16(raw, 4 * kBlk + 4));
    const size_t bat = (diff >= 0 ? 3 : 4) * kBlk;
    std::string g = raw.substr(e.off, 0x40);
    int blk = e.first;
    for (int i = 0; i < e.count; ++i) {
        if (blk < 5 || blk >= nblocks) return false;
        g += raw.substr((size_t)blk * kBlk, kBlk);
        size_t mo = bat + 10 + 2 * (size_t)(blk - 5);
        if (mo + 2 > raw.size()) return false;
        uint16_t next = be16(raw, mo);
        if (i + 1 < e.count) { if (next == 0xFFFF) return false; blk = next; }
    }
    *gci = std::move(g);
    return true;
}

/* ------------------------------------------------------------------ conversion */

namespace {

bool is_retro_system(const std::string& s) { return s == "gb" || s == "gbc" || s == "gba" || s == "nes" || s == "snes" || s == "nds" || s == "ps1"; }
const size_t kRaN64 = 0x800 + 4 * 0x8000 + 0x8000 + 0x20000;   // libretro mupen64plus: eeprom, 4 paks, sram, flash in one .srm

bool compatible(const Found& f, const GameRef& g) {
    const std::string& e = f.ext;
    const std::string& s = g.system;
    if (e == ".srm") return s == "n64" ? f.size == (int64_t)kRaN64 : is_retro_system(s);
    if (e == ".sav") return s == "gb" || s == "gbc" || s == "gba" || s == "nds";
    if (e == ".eep" || e == ".sra" || e == ".fla" || e == ".mpk") return s == "n64";
    return false;
}

std::string base_stem(const std::string& name) { return U8(P(name).stem()); }

std::string dir_with_slash(std::string d) { while (!d.empty() && (d.back() == '\\' || d.back() == '/')) d.pop_back(); return d; }

bool n64_file_name(const GameRef& g, const std::string& ext, const Ctx& ctx, std::string* name, std::string* why) {
    if (g.key.rfind("n64:", 0) != 0 || g.key.size() < 12) { *why = "This game has no N64 id."; return false; }
    std::string md5 = upper(g.key.substr(4));
    std::string gn = ini_goodname(ctx.n64_ini, md5);
    std::string header;
    {
        std::string head;
        std::ifstream f(P(g.rom_path), std::ios::binary);
        if (f) { head.assign(0x40, '\0'); f.read(&head[0], 0x40); head.resize((size_t)f.gcount()); header = n64_header_name(head); }
    }
    std::string eff = !gn.empty() ? gn : (!header.empty() ? header + " (unknown rom)" : std::string());
    std::string dir = dir_with_slash(ctx.battery_dir ? ctx.battery_dir(g) : std::string());
    if (!eff.empty() && !dir.empty() && eff.find_first_of(":<>\"/\\|?*") == std::string::npos) {   // the core keeps using an old "<goodname>.<ext>" file when one exists
        std::error_code ec;
        fs::path old = P(dir) / P(eff + ext);
        if (fs::is_regular_file(old, ec) && fs::file_size(old, ec) > 0) { *name = eff + ext; return true; }
    }
    std::string base = n64_save_base(md5, gn, header);
    if (base.empty()) { *why = "Start this game once in RetroHub first, so it knows what to call the save."; return false; }
    *name = base + ext;
    return true;
}

bool read_src(const Found& f, std::string* data, std::string* why) {
    if (!read_file(f.path, data)) { *why = "Could not read " + f.name + "."; return false; }
    return true;
}

std::string safe_gci_name(const GcEntry& e) {
    std::string n;
    for (char c : e.filename) n.push_back((isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_' || c == ' ') ? c : '_');
    auto clean = [](std::string s) { for (char& c : s) if (!isalnum((unsigned char)c)) c = '_'; return s; };
    return clean(e.maker) + "-" + clean(e.gamecode) + "-" + n + ".gci";
}

std::string ps2_serial(const GameRef& g) {
    if (!g.serial.empty()) return upper(g.serial);
    if (g.key.rfind("ps2:", 0) == 0 && g.key.compare(4, 5, "file-") != 0) return upper(g.key.substr(4));
    return "";
}

}  // namespace

bool convert(const Row& r, const GameRef& g, const Ctx& ctx, std::vector<OutFile>* out, std::string* why) {
    std::string dummy;
    if (!why) why = &dummy;
    out->clear();
    if (r.files.empty()) { *why = "Nothing to import."; return false; }
    const Found& f = r.files[0];
    const std::string& sys = g.system;

    if (sys == "gc") {
        for (const Found& gf : r.files) {
            std::string d;
            if (!read_src(gf, &d, why)) return false;
            if (gf.ext == ".gci") {
                if (d.size() < 0x40 + 0x2000) { *why = gf.name + " is not a GameCube save."; return false; }
                out->push_back({"GCI/" + gf.name, std::move(d)});
            } else {
                std::vector<GcEntry> es;
                if (!gc_card_entries(d, &es)) { *why = gf.name + " is not a GameCube memory card."; return false; }
                for (auto& e : es) {
                    if (e.gamecode != r.gc_game) continue;
                    std::string gci;
                    if (!gc_card_export_gci(d, e, &gci)) { *why = "A save on the card is damaged."; return false; }
                    out->push_back({"GCI/" + safe_gci_name(e), std::move(gci)});
                }
            }
        }
        if (out->empty()) { *why = "No save for this game on the card."; return false; }
        return true;
    }
    if (sys == "ps2") {
        if (f.size != 8388608 && f.size != 8650752) { *why = "Only 8 MB PS2 memory cards can be imported."; return false; }
        std::string d;
        if (!read_src(f, &d, why)) return false;
        out->push_back({"memcards/RetroHub/" + pcsx2_safe_key(g.key) + "/Mcd001.ps2", std::move(d)});
        return true;
    }
    if (sys == "n64") {
        std::string d;
        if (!read_src(f, &d, why)) return false;
        auto name = [&](const char* ext, std::string* n) { return n64_file_name(g, ext, ctx, n, why); };
        if (f.ext == ".eep") {
            if (d.size() != 512 && d.size() != 2048) { *why = "That is not an N64 EEPROM save."; return false; }
            d.resize(2048, '\xFF');   // the core always keeps 2 KB
            std::string n;
            if (!name(".eep", &n)) return false;
            out->push_back({n, std::move(d)});
        } else if (f.ext == ".sra" || f.ext == ".fla") {
            const size_t want = f.ext == ".sra" ? 32768 : 131072;
            if (d.size() != want) { *why = std::string("That is not an N64 ") + (f.ext == ".sra" ? "SRAM" : "Flash") + " save."; return false; }
            std::string n;
            if (!name(f.ext.c_str(), &n)) return false;
            out->push_back({n, std::move(d)});
        } else if (f.ext == ".mpk") {
            std::string n;
            if (!name(".mpk", &n)) return false;
            if (d.size() == 131072) out->push_back({n, std::move(d)});
            else if (d.size() == 32768) {
                std::string all;
                std::string dir = dir_with_slash(ctx.battery_dir ? ctx.battery_dir(g) : std::string());
                if (!dir.empty()) { std::string ex; if (read_file(dir + "\\" + n, &ex) && ex.size() == 131072) all = ex; }
                if (all.empty()) for (int i = 0; i < 4; ++i) all += blank_mempak(g.key + "#" + std::to_string(i));
                int slot = std::max(0, std::min(3, r.controller));
                std::memcpy(&all[(size_t)slot * 32768], d.data(), 32768);
                out->push_back({n, std::move(all)});
            } else { *why = "That is not an N64 Controller Pak."; return false; }
        } else if (f.ext == ".srm") {
            if (d.size() != kRaN64) { *why = "That is not an N64 save from RetroArch."; return false; }
            std::string n;
            if (!all_same(d, 0, 0x800) && name(".eep", &n)) out->push_back({n, d.substr(0, 0x800)});
            if (!why->empty()) return false;
            bool pak = false;
            for (int i = 0; i < 4; ++i) if (mempak_has_notes(d.substr(0x800 + (size_t)i * 0x8000, 0x8000))) pak = true;
            if (pak && name(".mpk", &n)) out->push_back({n, d.substr(0x800, 4 * 0x8000)});
            if (!why->empty()) return false;
            if (!all_same(d, 0x800 + 4 * 0x8000, 0x8000) && name(".sra", &n)) out->push_back({n, d.substr(0x800 + 4 * 0x8000, 0x8000)});
            if (!why->empty()) return false;
            if (!all_same(d, 0x800 + 5 * 0x8000, 0x20000) && name(".fla", &n)) out->push_back({n, d.substr(0x800 + 5 * 0x8000, 0x20000)});
            if (!why->empty()) return false;
            if (out->empty()) { *why = "There is no save inside this file yet."; return false; }
        } else { *why = "Not an N64 save."; return false; }
        return true;
    }
    if (is_retro_system(sys)) {
        std::string d;
        if (!read_src(f, &d, why)) return false;
        if (d.empty()) { *why = "The save is empty."; return false; }
        if (g.save_stem.empty()) { *why = "This game has no save name."; return false; }
        if (sys == "gba") gbasave::strip_rtc_footer(&d);   // mGBA / Dolphin add a 16 byte clock block; retrorun's core keeps (and expects) the bare save
        out->push_back({g.save_stem + ".srm", std::move(d)});
        return true;
    }
    *why = "This kind of game has no importable save.";
    return false;
}

/* ------------------------------------------------------------------ rows */

namespace {

void fill_state(Row* r, const GameRef& g, const Ctx& ctx) {
    std::vector<OutFile> outs;
    std::string why;
    r->src_size = 0;
    r->src_mtime = 0;
    for (auto& f : r->files) { r->src_mtime = std::max(r->src_mtime, f.mtime); r->src_size += f.size; }
    if (!convert(*r, g, ctx, &outs, &why)) { r->problem = why; return; }
    std::string dir = dir_with_slash(ctx.battery_dir ? ctx.battery_dir(g) : std::string());
    bool all_same_content = !outs.empty();
    bool first = true;
    for (auto& o : outs) {
        r->out_rel.push_back(o.rel);
        fs::path vp = P(dir) / P(o.rel);
        std::error_code ec;
        if (!dir.empty() && fs::is_regular_file(vp, ec)) {
            r->vault_has = true;
            r->vault_mtime = std::max(r->vault_mtime, mtime_unix(vp));
            if (first) r->vault_size = (int64_t)fs::file_size(vp, ec);
            std::string cur;
            if (!(read_file(U8(vp), &cur) && cur == o.data)) all_same_content = false;
        } else all_same_content = false;
        first = false;
    }
    r->same_as_vault = r->vault_has && all_same_content;
    if (!r->vault_has) r->newer = Row::NoVault;
    else if (r->same_as_vault) r->newer = Row::Same;
    else r->newer = r->src_mtime > r->vault_mtime ? Row::SourceNewer : Row::VaultNewer;
}

}  // namespace

std::vector<Row> build_rows(const std::vector<Found>& found, const std::vector<GameRef>& games, const Ctx& ctx) {
    std::vector<Row> rows;
    std::map<std::pair<size_t, std::string>, size_t> gci_rows;   // (game, folder) -> row

    for (const Found& f : found) {
        const std::string stem0 = base_stem(f.name);

        if (f.ext == ".gci") {
            std::string head;
            std::ifstream in(P(f.path), std::ios::binary);
            if (!in) continue;
            head.assign(4, '\0');
            in.read(&head[0], 4);
            if (in.gcount() != 4) continue;
            for (size_t gi = 0; gi < games.size(); ++gi) {
                const GameRef& g = games[gi];
                if (g.system != "gc" || g.serial.size() < 4 || g.serial.compare(0, 4, head) != 0) continue;
                std::string folder = U8(P(f.path).parent_path());
                auto key = std::make_pair(gi, folder);
                auto it = gci_rows.find(key);
                if (it == gci_rows.end()) {
                    Row r; r.files.push_back(f); r.game = gi; r.how = How::Content;
                    rows.push_back(r);
                    gci_rows[key] = rows.size() - 1;
                } else rows[it->second].files.push_back(f);
            }
            continue;
        }
        if (f.ext == ".raw" || f.ext == ".gcp" || f.ext == ".mcd") {
            std::string d;
            std::vector<GcEntry> es;
            if (!read_file(f.path, &d) || !gc_card_entries(d, &es)) continue;
            std::set<std::string> codes;
            for (auto& e : es) codes.insert(e.gamecode);
            for (size_t gi = 0; gi < games.size(); ++gi) {
                const GameRef& g = games[gi];
                if (g.system != "gc" || g.serial.size() < 4) continue;
                std::string code = g.serial.substr(0, 4);
                if (!codes.count(code)) continue;
                Row r; r.files.push_back(f); r.game = gi; r.how = How::Content; r.gc_game = code;
                rows.push_back(r);
            }
            continue;
        }
        if (f.ext == ".ps2") {
            if (f.size != 8388608 && f.size != 8650752) continue;
            std::string d;
            if (!read_file(f.path, &d)) continue;
            for (size_t gi = 0; gi < games.size(); ++gi) {
                const GameRef& g = games[gi];
                if (g.system != "ps2") continue;
                std::string ser = ps2_serial(g);
                if (ser.size() < 6 || std::search(d.begin(), d.end(), ser.begin(), ser.end()) == d.end()) continue;
                Row r; r.files.push_back(f); r.game = gi; r.how = How::Content;
                rows.push_back(r);
            }
            continue;
        }

        /* in-game saves named after the game */
        int ctl = 0;
        const std::string s0 = strip_save_suffix(stem0, &ctl);
        const std::string ls = lower(s0), ns = norm_name(s0);
        std::vector<size_t> stem_hits, title_hits;
        for (size_t gi = 0; gi < games.size(); ++gi) {
            const GameRef& g = games[gi];
            if (!compatible(f, g)) continue;
            if (!g.save_stem.empty() && lower(g.save_stem) == ls) { stem_hits.push_back(gi); continue; }
            if (!g.rom_path.empty() && lower(base_stem(g.rom_path)) == ls) { stem_hits.push_back(gi); continue; }
            if (!ns.empty() && (norm_name(g.title) == ns || (!g.save_stem.empty() && norm_name(g.save_stem) == ns))) title_hits.push_back(gi);
        }
        const std::vector<size_t>& hits = !stem_hits.empty() ? stem_hits : title_hits;
        if (hits.empty()) continue;
        Row r;
        r.files.push_back(f);
        r.game = hits[0];
        r.how = f.src == Source::NextToRom ? How::Beside : (!stem_hits.empty() ? How::Stem : How::Title);
        r.ambiguous = hits.size() > 1;
        r.controller = ctl;
        rows.push_back(r);
    }

    for (Row& r : rows) fill_state(&r, games[r.game], ctx);

    for (Row& r : rows) r.selected = r.problem.empty() && !r.ambiguous && !r.same_as_vault && (r.newer == Row::NoVault || r.newer == Row::SourceNewer);
    /* several sources for the same vault file: only the newest is pre-selected */
    std::map<std::pair<size_t, std::string>, size_t> best;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (!rows[i].selected || rows[i].out_rel.empty()) continue;
        for (const std::string& o : rows[i].out_rel) {
            auto key = std::make_pair(rows[i].game, o);
            auto it = best.find(key);
            if (it == best.end()) best[key] = i;
            else if (rows[i].src_mtime > rows[it->second].src_mtime) { rows[it->second].selected = false; it->second = i; }
            else rows[i].selected = false;
        }
    }
    std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
        std::string ta = lower(games[a.game].title), tb = lower(games[b.game].title);
        if (ta != tb) return ta < tb;
        return a.src_mtime > b.src_mtime;
    });
    return rows;
}

/* ------------------------------------------------------------------ apply */

namespace {
std::string stamp_of(int64_t t) {
    std::time_t tt = (std::time_t)t;
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    char b[32];
    std::snprintf(b, sizeof b, "%04d%02d%02d-%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return b;
}

bool write_atomic(const fs::path& to, const std::string& data) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    fs::path tmp = to;
    tmp += L".hubtmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), (std::streamsize)data.size());
        f.flush();
        if (!f) { f.close(); fs::remove(tmp, ec); return false; }
    }
    fs::rename(tmp, to, ec);
    if (ec) { std::error_code e2; fs::remove(tmp, e2); return false; }
    return true;
}
}  // namespace

ApplyReport apply_row(const Row& r, const GameRef& g, const Ctx& ctx, const std::string& vault_root, int64_t now) {
    ApplyReport rep;
    std::vector<OutFile> outs;
    std::string why;
    if (!convert(r, g, ctx, &outs, &why)) { rep.ok = false; rep.error = why; return rep; }
    std::string dir = dir_with_slash(ctx.battery_dir ? ctx.battery_dir(g) : std::string());
    if (dir.empty()) { rep.ok = false; rep.error = "No vault folder for this game."; return rep; }
    std::error_code ec;
    fs::path rel_game = fs::relative(P(dir), P(vault_root), ec);
    if (ec || rel_game.empty() || *rel_game.begin() == "..") rel_game = fs::path(g.system) / P(pcsx2_safe_key(g.key)) / "battery";
    const std::string stamp = stamp_of(now);
    {   // the save must still be the one the preview showed (a game or a sync may have written it since)
        int64_t cur = 0;
        bool any = false;
        for (const OutFile& o : outs) { std::error_code e0; fs::path p = P(dir) / P(o.rel); if (fs::exists(p, e0)) { any = true; cur = std::max(cur, mtime_unix(p)); } }
        if (any != r.vault_has || (any && cur != r.vault_mtime)) { rep.ok = false; rep.error = "Your save here changed since you looked. Look again and pick it again."; return rep; }
    }
    for (const OutFile& o : outs) {
        fs::path to = P(dir) / P(o.rel);
        std::error_code e1;
        const bool exists = fs::exists(to, e1);
        if (e1) { rep.ok = false; rep.error = "Could not look at the current save, so nothing was changed."; return rep; }   // unknown state: never overwrite
        if (exists) {
            std::string cur;
            if (read_file(U8(to), &cur) && cur == o.data) { ++rep.unchanged; continue; }
            fs::path bk = P(vault_root) / "_conflicts" / rel_game / P(o.rel);
            fs::path bk2 = bk;
            bk2 += P(".import." + stamp);
            for (int n = 2; fs::exists(bk2, e1) && n < 100; ++n) { bk2 = bk; bk2 += P(".import." + stamp + "-" + std::to_string(n)); }
            fs::create_directories(bk2.parent_path(), e1);
            fs::copy_file(to, bk2, fs::copy_options::none, e1);
            if (e1) { rep.ok = false; rep.error = "Could not back up the current save, so nothing was changed."; return rep; }
            ++rep.backed_up;
        }
        if (!write_atomic(to, o.data)) { rep.ok = false; rep.error = "Could not write " + o.rel + "."; return rep; }
        ++rep.written;
        rep.wrote.push_back(U8(to));
    }
    return rep;
}

}  // namespace hub::saveimport
