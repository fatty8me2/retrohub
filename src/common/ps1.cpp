#include "ps1.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "games.h"   // md5_hex

extern "C" {
#include <formats/rchd.h>
}

namespace fs = std::filesystem;

namespace hub {
namespace ps1 {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
std::string upper(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}
std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}
std::string ext_of(const std::string& p) {
    size_t slash = p.find_last_of("/\\");
    size_t dot = p.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    return lower(p.substr(dot));
}
std::string dir_of(const std::string& p) {
    size_t slash = p.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : p.substr(0, slash + 1);
}
std::string stem_of(const std::string& p) {
    size_t slash = p.find_last_of("/\\");
    std::string n = slash == std::string::npos ? p : p.substr(slash + 1);
    size_t dot = n.find_last_of('.');
    return dot == std::string::npos ? n : n.substr(0, dot);
}
bool is_abs(const std::string& p) { return p.size() > 1 && (p[1] == ':' || p[0] == '\\' || p[0] == '/'); }

bool read_text(const std::string& path, std::string* out, size_t max = 1u << 20) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) return false;
    out->assign(max, '\0');
    f.read(&(*out)[0], (std::streamsize)max);
    out->resize((size_t)f.gcount());
    return true;
}

std::string normalize_path_text(std::string s) {
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s.erase(0, 3);
    return s;
}

std::vector<std::string> split_lines(const std::string& t) {
    std::vector<std::string> v;
    size_t b = 0;
    while (b <= t.size()) {
        size_t e = t.find_first_of("\r\n", b);
        if (e == std::string::npos) e = t.size();
        v.push_back(t.substr(b, e - b));
        b = e + 1;
        if (e + 1 < t.size() && t[e] == '\r' && t[e + 1] == '\n') b = e + 2;
        if (e >= t.size()) break;
    }
    return v;
}

} // namespace

/* ------------------------------------------------------------------ serials / catalog */

std::string normalize_serial(const std::string& in) {
    std::string s = upper(in);
    size_t c = s.find(':');
    if (c != std::string::npos && c < 8) s = s.substr(c + 1);
    size_t sl = s.find_last_of("\\/");
    if (sl != std::string::npos) s = s.substr(sl + 1);
    size_t semi = s.find(';');
    if (semi != std::string::npos) s.resize(semi);
    std::string letters, digits;
    for (char ch : s) {
        if (std::isalpha((unsigned char)ch) && digits.empty()) letters += ch;
        else if (std::isdigit((unsigned char)ch)) digits += ch;
        else if (ch == '_' || ch == '-' || ch == ' ' || ch == '.') continue;
        else return "";
    }
    if (letters.size() != 4 || digits.size() != 5) return "";
    return letters + "-" + digits;
}

std::string serial_from_system_cnf(const std::string& text) {
    for (const std::string& raw : split_lines(text)) {
        std::string l = trim(raw);
        if (l.size() < 5 || upper(l.substr(0, 4)) != "BOOT") continue;
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string v = trim(l.substr(eq + 1));
        std::string s = normalize_serial(v);
        if (!s.empty()) return s;
    }
    return "";
}

namespace {
struct Db {
    std::unordered_map<std::string, const char*> by_serial;
    std::unordered_map<std::string, const char*> by_name;   // lowercase title -> serial
    Db() {
        by_serial.reserve(kPs1DbCount * 2);
        for (size_t i = 0; i < kPs1DbCount; ++i) {
            by_serial.emplace(kPs1Db[i].serial, kPs1Db[i].title);
            by_name.emplace(lower(kPs1Db[i].title), kPs1Db[i].serial);
        }
    }
};
const Db& db() {
    static const Db d;
    return d;
}
} // namespace

std::string title_for_serial(const std::string& serial) {
    auto it = db().by_serial.find(normalize_serial(serial));
    return it == db().by_serial.end() ? std::string() : std::string(it->second);
}

std::string serial_for_name(const std::string& stem) {
    auto it = db().by_name.find(lower(trim(stem)));
    return it == db().by_name.end() ? std::string() : std::string(it->second);
}

int disc_number(const std::string& name) {
    std::string l = lower(name);
    size_t p = l.find("(disc ");
    if (p == std::string::npos) return 0;
    return std::atoi(l.c_str() + p + 6);
}

char serial_region(const std::string& s) {
    std::string n = normalize_serial(s);
    if (n.size() < 4) return 0;
    std::string p = n.substr(0, 4);
    if (p == "SLUS" || p == "SCUS" || p == "PAPX" || p == "LSP-" ) return 'U';
    if (p == "SLES" || p == "SCES" || p == "SCED" || p == "SLED") return 'E';
    if (p == "SLPS" || p == "SLPM" || p == "SCPS" || p == "SCPM" || p == "SIPS" || p == "PCPX" || p == "PAPX") return 'J';
    return 0;
}

/* ------------------------------------------------------------------ ISO 9660 */

std::string serial_from_disc(SectorReader& r, bool* is_iso, bool* has_boot) {
    if (is_iso) *is_iso = false;
    if (has_boot) *has_boot = false;
    uint8_t pvd[2048];
    if (!r.read(16, pvd) || pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0) return "";
    if (is_iso) *is_iso = true;
    auto u32 = [](const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; };
    uint32_t root = u32(pvd + 156 + 2), rsize = u32(pvd + 156 + 10);
    if (rsize == 0 || rsize > (1u << 20)) return "";
    uint32_t sectors = (rsize + 2047) / 2048;
    for (uint32_t s = 0; s < sectors && s < 64; ++s) {
        uint8_t d[2048];
        if (!r.read(root + s, d)) return "";
        for (size_t o = 0; o < 2048;) {
            uint8_t len = d[o];
            if (len == 0) break;
            if (o + len > 2048 || len < 34) break;
            uint8_t nl = d[o + 32];
            if (o + 33 + nl <= 2048) {
                std::string name = upper(std::string((const char*)d + o + 33, nl));
                if (name == "SYSTEM.CNF;1" || name == "SYSTEM.CNF") {
                    uint32_t ext = u32(d + o + 2), size = u32(d + o + 10);
                    if (size == 0 || size > 2048 * 2) return "";
                    std::string text;
                    for (uint32_t k = 0; k * 2048 < size; ++k) {
                        uint8_t f[2048];
                        if (!r.read(ext + k, f)) return "";
                        text.append((const char*)f, std::min<uint32_t>(2048, size - k * 2048));
                    }
                    if (has_boot && upper(text).find("BOOT") != std::string::npos) *has_boot = true;
                    return serial_from_system_cnf(text);
                }
            }
            o += len;
        }
    }
    return "";
}

/* ------------------------------------------------------------------ cue / m3u */

std::vector<CueFile> parse_cue(const std::string& text) {
    std::vector<CueFile> out;
    for (const std::string& raw : split_lines(normalize_path_text(text))) {
        std::string l = trim(raw);
        std::string u = upper(l);
        if (u.compare(0, 5, "FILE ") == 0) {
            CueFile f;
            size_t q1 = l.find('"');
            if (q1 != std::string::npos) {
                size_t q2 = l.find('"', q1 + 1);
                if (q2 == std::string::npos) continue;
                f.file = l.substr(q1 + 1, q2 - q1 - 1);
            } else {
                std::string rest = trim(l.substr(5));
                size_t sp = rest.find_last_of(' ');
                f.file = sp == std::string::npos ? rest : trim(rest.substr(0, sp));
            }
            out.push_back(f);
        } else if (u.compare(0, 6, "TRACK ") == 0 && !out.empty() && out.back().mode.empty()) {
            size_t sp = l.find_last_of(' ');
            if (sp != std::string::npos) out.back().mode = upper(trim(l.substr(sp + 1)));
        }
    }
    return out;
}

std::vector<std::string> parse_m3u(const std::string& text) {
    std::vector<std::string> out;
    for (const std::string& raw : split_lines(normalize_path_text(text))) {
        std::string l = trim(raw);
        if (l.empty() || l[0] == '#') continue;
        out.push_back(l);
    }
    return out;
}

/* ------------------------------------------------------------------ readers */

namespace {

struct FileSectors : SectorReader {
    std::ifstream f;
    uint32_t stride = 2048, off = 0;
    uint64_t base = 0;
    bool open(const std::string& path, uint32_t sector_size, uint32_t user_off) {
        f.open(fs::u8path(path), std::ios::binary);
        stride = sector_size;
        off = user_off;
        return (bool)f;
    }
    bool read(uint32_t lba, uint8_t* out) override {
        f.clear();
        f.seekg((std::streamoff)(base + (uint64_t)lba * stride + off));
        f.read((char*)out, 2048);
        return f.gcount() == 2048;
    }
};

/* Guess sector layout of a raw .bin/.img/.iso from its first sector and size; mode hint from a cue ("MODE2/2352"). */
bool open_binary(const std::string& path, const std::string& mode_hint, FileSectors* fsr) {
    std::error_code ec;
    uintmax_t size = fs::file_size(fs::u8path(path), ec);
    if (ec || size < 2048 * 17) return false;
    std::ifstream f(fs::u8path(path), std::ios::binary);
    uint8_t h[24] = {};
    f.read((char*)h, sizeof h);
    static const uint8_t sync[12] = {0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0};
    bool raw = std::memcmp(h, sync, 12) == 0 || (!mode_hint.empty() && mode_hint.find("2352") != std::string::npos);
    if (raw) return fsr->open(path, 2352, h[15] == 2 || mode_hint.find("MODE2") == 0 ? 24 : 16);
    return fsr->open(path, 2048, 0);
}

struct ChdSectors : SectorReader {
    FILE* fp = nullptr;
    rchd_t* chd = nullptr;
    ~ChdSectors() {
        if (chd) rchd_free(chd);
        if (fp) std::fclose(fp);
    }
    bool pump(rchd_request_t* rq, int (*step)(rchd_t*, rchd_request_t*)) {
        std::vector<uint8_t> buf;
        for (;;) {
            int e = step(chd, rq);
            if (e == RCHD_OK) return true;
            if (e != RCHD_PENDING) return false;
            if (rq->source != RCHD_SOURCE_SELF) return false;
            buf.resize(rq->length);
            if (_fseeki64(fp, (long long)rq->offset, SEEK_SET) != 0) return false;
            size_t n = std::fread(buf.data(), 1, buf.size(), fp);
            if (n == 0) return false;
            if (rchd_feed(chd, buf.data(), n) < 0) return false;
        }
    }
    bool open(const std::string& path) {
        fp = _wfopen(fs::u8path(path).wstring().c_str(), L"rb");
        if (!fp) return false;
        chd = rchd_new();
        if (!chd) return false;
        rchd_request_t rq;
        if (!pump(&rq, rchd_open_step)) return false;
        return rchd_track_count(chd) > 0;
    }
    bool read(uint32_t lba, uint8_t* out) override {
        const rchd_track_t* t = rchd_track_for_lba(chd, lba);
        if (!t) return false;
        uint8_t buf[2352 + 16];
        uint32_t ds = t->data_size;
        if (ds > 2352) return false;
        if (rchd_read_sectors_begin(chd, lba, 1, buf, sizeof buf, 0) != RCHD_OK) return false;
        rchd_request_t rq;
        if (!pump(&rq, rchd_read_step)) return false;
        if (ds == 2048) std::memcpy(out, buf, 2048);
        else if (ds == 2352) std::memcpy(out, buf + (buf[15] == 2 ? 24 : 16), 2048);
        else return false;
        return true;
    }
};

/* PBP (PSone Classics): DISC_ID from the embedded PARAM.SFO. */
std::string pbp_disc_id(const std::string& path) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    uint8_t h[0x28];
    f.read((char*)h, sizeof h);
    if (f.gcount() != (std::streamsize)sizeof h || std::memcmp(h, "\0PBP", 4) != 0) return "";
    auto u32 = [](const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; };
    uint32_t sfo = u32(h + 8), next = u32(h + 12);
    if (next <= sfo || next - sfo > (1u << 16)) return "";
    std::vector<uint8_t> d(next - sfo);
    f.seekg(sfo);
    f.read((char*)d.data(), (std::streamsize)d.size());
    if (d.size() < 20 || std::memcmp(d.data(), "\0PSF", 4) != 0) return "";
    uint32_t kt = u32(d.data() + 8), dt = u32(d.data() + 12), n = u32(d.data() + 16);
    for (uint32_t i = 0; i < n && 20 + i * 16 + 16 <= d.size(); ++i) {
        const uint8_t* e = d.data() + 20 + i * 16;
        uint32_t ko = e[0] | e[1] << 8, doff = u32(e + 12);
        if (kt + ko >= d.size() || dt + doff >= d.size()) continue;
        if (std::strncmp((const char*)d.data() + kt + ko, "DISC_ID", 7) == 0)
            return normalize_serial(std::string((const char*)d.data() + dt + doff, strnlen((const char*)d.data() + dt + doff, d.size() - dt - doff)));
    }
    return "";
}

DiscInfo probe_disc_file(const std::string& path, const std::string& ext, const std::string& mode_hint) {
    DiscInfo di;
    bool iso = false, boot = false;
    if (ext == ".chd") {
        ChdSectors c;
        if (!c.open(path)) return di;
        di.serial = serial_from_disc(c, &iso, &boot);
    } else {
        FileSectors fsr;
        if (!open_binary(path, mode_hint, &fsr)) return di;
        di.serial = serial_from_disc(fsr, &iso, &boot);
    }
    di.ok = iso && (boot || !di.serial.empty());   // a data disc without a Sony BOOT line (PC/GameCube/Wii ISO) is not a PlayStation game
    return di;
}

} // namespace

std::vector<std::string> referenced_files(const std::string& path) {
    std::vector<std::string> out;
    std::string ext = ext_of(path), text;
    std::string base = dir_of(path);
    auto resolve = [&](const std::string& n) { return is_abs(n) ? n : base + n; };
    if (ext == ".cue") {
        if (!read_text(path, &text)) return out;
        for (const CueFile& cf : parse_cue(text)) out.push_back(resolve(cf.file));
    } else if (ext == ".m3u") {
        if (!read_text(path, &text)) return out;
        for (const std::string& l : parse_m3u(text)) {
            std::string p = resolve(l);
            out.push_back(p);
            if (ext_of(p) == ".cue") for (const std::string& r : referenced_files(p)) out.push_back(r);
        }
    }
    return out;
}

DiscInfo probe_file(const std::string& path) {
    DiscInfo di;
    std::string ext = ext_of(path), text;
    di.title_hint = stem_of(path);
    if (ext == ".m3u") {
        if (!read_text(path, &text)) return di;
        std::vector<std::string> lines = parse_m3u(text);
        if (lines.empty()) return di;
        std::string first = is_abs(lines[0]) ? lines[0] : dir_of(path) + lines[0];
        if (ext_of(first) == ".m3u") return di;
        DiscInfo d1 = probe_file(first);
        d1.title_hint = stem_of(path);
        return d1;
    }
    if (ext == ".cue") {
        if (!read_text(path, &text)) return di;
        std::vector<CueFile> files = parse_cue(text);
        if (files.empty()) return di;
        std::string f = is_abs(files[0].file) ? files[0].file : dir_of(path) + files[0].file;
        DiscInfo d = probe_disc_file(f, ext_of(f), files[0].mode);
        d.title_hint = di.title_hint;
        return d;
    }
    if (ext == ".pbp") {
        di.serial = pbp_disc_id(path);
        di.ok = !di.serial.empty();
        return di;
    }
    if (ext == ".exe") {
        std::ifstream f(fs::u8path(path), std::ios::binary);
        char m[8] = {};
        f.read(m, 8);
        di.ok = std::memcmp(m, "PS-X EXE", 8) == 0;
        return di;
    }
    if (ext == ".chd" || ext == ".bin" || ext == ".img" || ext == ".iso") {
        DiscInfo d = probe_disc_file(path, ext, "");
        d.title_hint = di.title_hint;
        return d;
    }
    return di;
}

/* ------------------------------------------------------------------ BIOS */

static const std::vector<BiosInfo> kBios = {
    {"scph5501.bin", "490f666e1afb15b7362b406ed1cea246", 'U', "SCPH-5501 (North America, v3.0)"},
    {"scph5502.bin", "32736f17079d0b2b7024407c39bd3050", 'E', "SCPH-5502 (Europe, v3.0)"},
    {"scph5500.bin", "8dd7d5296a650fac7319bce665a6a53c", 'J', "SCPH-5500 (Japan, v3.0)"},
    {"scph7003.bin", "8abc1b549a4a80954addc48ef02c4521", 'U', "SCPH-7003 (North America, v4.1)"},
    {"scph7001.bin", "1e68c231d0896b7eadcad1d7d8e76129", 'U', "SCPH-7001 (North America, v4.1)"},
    {"scph1001.bin", "924e392ed05558ffdb115408c263dccf", 'U', "SCPH-1001 (North America, v2.0)"},
};

const std::vector<BiosInfo>& bios_list() { return kBios; }

const BiosInfo* bios_known(const std::string& md5) {
    std::string m = lower(md5);
    for (const BiosInfo& b : kBios) if (m == b.md5) return &b;
    return nullptr;
}

const char* bios_core_name(char region) { return region == 'E' ? "scph5502.bin" : region == 'J' ? "scph5500.bin" : "scph5501.bin"; }

bool bios_name_plausible(const std::string& name) {
    std::string l = lower(name);
    if (ext_of(l) != ".bin") return false;
    return l.compare(0, 4, "scph") == 0 || l.compare(0, 5, "scph-") == 0 || l.compare(0, 6, "psxbios") == 0;
}

static char region_from_name(const std::string& name) {
    std::string l = lower(stem_of(name));
    for (size_t i = l.size(); i-- > 0;) {
        if (!std::isdigit((unsigned char)l[i])) continue;
        return l[i] == '2' ? 'E' : l[i] == '0' ? 'J' : 'U';
    }
    return 'U';
}

std::vector<BiosFound> find_bios(const std::vector<std::string>& dirs) {
    std::vector<BiosFound> out;
    std::vector<std::string> seen;
    for (const std::string& d : dirs) {
        std::error_code ec;
        fs::path dir = fs::u8path(d);
        if (!fs::is_directory(dir, ec)) continue;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            std::string name = it->path().filename().u8string();
            if (!bios_name_plausible(name)) continue;
            uintmax_t sz = it->file_size(ec);
            if (sz != 524288 && sz != 1048576) continue;
            std::string full = it->path().u8string();
            std::string lf = lower(full);
            if (std::find(seen.begin(), seen.end(), lf) != seen.end()) continue;
            seen.push_back(lf);
            std::ifstream f(it->path(), std::ios::binary);
            std::vector<uint8_t> data((size_t)sz);
            f.read((char*)data.data(), (std::streamsize)sz);
            BiosFound bf;
            bf.path = full;
            bf.md5 = lower(md5_hex(data.data(), data.size()));
            bf.known = bios_known(bf.md5);
            bf.region = bf.known ? bf.known->region : region_from_name(name);
            out.push_back(bf);
        }
    }
    return out;
}

std::vector<std::string> bios_search_dirs(const std::vector<std::string>& rom_folders, const std::string& user_pick, const std::string& staging_dir,
                                          const std::string& home, const std::string& exe_dir, const std::string& appdata) {
    std::vector<std::string> out;
    auto add = [&](std::string d) {
        while (d.size() > 3 && (d.back() == '\\' || d.back() == '/')) d.pop_back();
        if (d.empty()) return;
        std::string l = lower(d);
        for (char& ch : l) if (ch == '/') ch = '\\';
        for (const std::string& o : out) {
            std::string ol = lower(o);
            for (char& ch : ol) if (ch == '/') ch = '\\';
            if (ol == l) return;
        }
        out.push_back(d);
    };
    auto parent = [](const std::string& p) {
        std::string t = p;
        while (t.size() > 3 && (t.back() == '\\' || t.back() == '/')) t.pop_back();
        size_t s = t.find_last_of("/\\");
        return s == std::string::npos || s < 2 ? std::string() : t.substr(0, s);
    };
    if (!user_pick.empty()) {
        std::error_code ec;
        add(fs::is_directory(fs::u8path(user_pick), ec) ? user_pick : parent(user_pick));
    }
    add(staging_dir);
    for (const std::string& f : rom_folders) {
        add(f);
        for (const char* sub : {"BIOS", "bios", "System", "system"}) add(f + "\\" + sub);
        std::string p = parent(f);
        for (int up = 0; up < 2 && !p.empty(); ++up, p = parent(p))
            for (const char* sub : {"BIOS", "bios"}) add(p + "\\" + sub);
    }
    if (!home.empty()) {
        add(home + "\\Desktop\\Everything\\BIOS");
        add(home + "\\Documents\\RetroHub Games\\BIOS");
    }
    if (!exe_dir.empty()) { add(exe_dir + "\\system"); add(exe_dir + "\\bios"); }
    if (!appdata.empty()) add(appdata + "\\RetroArch\\system");
    return out;
}

int stage_bios(const std::vector<BiosFound>& found, const std::string& system_dir) {
    std::error_code ec;
    fs::create_directories(fs::u8path(system_dir), ec);
    int provided = 0;
    for (char region : {'U', 'E', 'J'}) {
        const BiosFound* best = nullptr;
        int best_rank = 1 << 20;
        for (const BiosFound& f : found) {
            if (f.region != region) continue;
            int rank = 1000;
            if (f.known) rank = (int)(f.known - kBios.data());
            if (rank < best_rank) { best_rank = rank; best = &f; }
        }
        if (!best) continue;
        fs::path dst = fs::u8path(system_dir) / bios_core_name(region);
        fs::copy_file(fs::u8path(best->path), dst, fs::copy_options::overwrite_existing, ec);
        if (!ec) ++provided;
    }
    /* a region without a BIOS of its own: borrow the best one found (any retail BIOS runs any region's discs) */
    const BiosFound* any = nullptr;
    int any_rank = 1 << 20;
    for (const BiosFound& f : found) {
        int rank = f.known ? (int)(f.known - kBios.data()) : 1000;
        if (rank < any_rank) { any_rank = rank; any = &f; }
    }
    if (any) {
        for (char region : {'U', 'E', 'J'}) {
            fs::path dst = fs::u8path(system_dir) / bios_core_name(region);
            if (!fs::exists(dst, ec)) fs::copy_file(fs::u8path(any->path), dst, fs::copy_options::overwrite_existing, ec);
        }
    }
    return provided;
}

} // namespace ps1
} // namespace hub
