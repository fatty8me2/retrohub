#include "ps2.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <filesystem>
#include <fstream>

#include "ps1.h"
#include "winutil.h"

namespace hub { namespace ps2 {

namespace fs = std::filesystem;
static std::string u8(const fs::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); }
static std::string lower(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c + 32); return s; }

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

char region_from_name(const std::string& name) {
    std::string l = lower(name);
    auto has = [&](const char* t) { return l.find(t) != std::string::npos; };
    if (has("(usa") || has("ntsc-u") || has("(canada") || has("(us)")) return 'U';
    if (has("(europe") || has("(pal") || has("[pal") || has("(germany") || has("(france") || has("(spain") || has("(italy") || has("(uk")) return 'E';
    if (has("(japan") || has("ntsc-j") || has("(asia") ) return 'J';
    return 0;
}

static char serial_region(const std::string& s) {
    std::string p = s.substr(0, 4);
    if (p == "SLUS" || p == "SCUS") return 'U';
    if (p == "SLES" || p == "SCES") return 'E';
    if (p == "SLPS" || p == "SLPM" || p == "SCPS" || p == "SCPM" || p == "SLAJ") return 'J';
    return 0;
}

/* lowercase letters and digits only, "&" read as "and", (...) and [...] groups and a known extension dropped */
static std::string name_key(std::string n, bool keep_brackets) {
    size_t slash = n.find_last_of("/\\");
    if (slash != std::string::npos) n = n.substr(slash + 1);
    for (const char* x : {".7z", ".rar", ".zip", ".iso", ".chd", ".cso", ".cue", ".bin"}) {
        size_t k = std::strlen(x);
        if (n.size() > k && lower(n.substr(n.size() - k)) == x) { n.resize(n.size() - k); break; }
    }
    std::string o;
    int depth = 0;
    for (char c : n) {
        if (c == '(' || (c == '[' && !keep_brackets)) { ++depth; continue; }
        if (c == ')' || (c == ']' && !keep_brackets)) { if (depth) --depth; continue; }
        if (depth) continue;
        if (c == '&') { o += "and"; continue; }
        c = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) o += c;
    }
    return o;
}

std::string serial_for_name(const std::string& name, bool* ambiguous) {
    if (ambiguous) *ambiguous = false;
    static const std::unordered_multimap<std::string, size_t> idx = [] {
        std::unordered_multimap<std::string, size_t> m;
        for (size_t i = 0; i < kDbCount; ++i) {
            const char* t = kDb[i] + 11;
            if (std::strchr(t, '[')) continue;   // "[Limited Edition]" style variants: matched only by their full title below
            m.emplace(name_key(t, false), i);
        }
        return m;
    }();
    const std::string key = name_key(name, false);
    if (key.size() < 3) return "";
    char want = region_from_name(name);
    std::vector<std::string> fit, any;
    auto r = idx.equal_range(key);
    for (auto it = r.first; it != r.second; ++it) {
        std::string serial(kDb[it->second], 10);
        any.push_back(serial);
        if (!want || serial_region(serial) == want) fit.push_back(serial);
    }
    if (fit.empty()) fit = any;   // the name's region has no entry of its own: the title still identifies the game
    if (fit.empty()) return "";
    std::sort(fit.begin(), fit.end());
    if (ambiguous) *ambiguous = fit.size() > 1;
    return fit[0];
}

static bool from_info(const disc::Info& in, Ident* out) {
    if (in.kind == Cnf::Ps1) { out->ps1 = true; return false; }
    if (in.kind != Cnf::Ps2) return false;
    out->serial = in.serial;
    out->title = title_for(in.serial);
    return true;
}

bool identify_buffer(std::shared_ptr<std::string> data, Ident* out) {
    disc::Info in;
    return disc::probe_buffer(std::move(data), &in) && from_info(in, out);
}

/* A CHD that libchdr cannot read (damaged, unsupported codec): fall back to a known PS2 serial in the file name. */
static bool by_name(const std::string& name, Ident* out) {
    std::string s = serial_from_filename(name), t = title_for(s);
    if (s.empty() || t.empty()) return false;
    out->serial = s;
    out->title = t;
    return true;
}

bool identify_chd_slice(const std::string& path, uint64_t base, uint64_t size, const std::string& entry_name, Ident* out) {
    Disc d;
    std::string cnf;
    if (d.open_chd_slice(path, base, size) && read_system_cnf(d.reader(), &cnf)) {
        disc::Info in;
        in.kind = parse_system_cnf(cnf, &in.serial);
        if (in.kind != Cnf::None) return from_info(in, out);
    }
    return by_name(entry_name, out);
}

bool identify_file(const std::string& path, Ident* out) {
    *out = Ident();
    fs::path p = fs::u8path(path);
    std::string ext = lower(u8(p.extension()));
    if (ext != ".chd" && ext != ".cue" && ext != ".iso" && ext != ".bin" && ext != ".cso" && ext != ".ciso") return false;
    disc::Info in;
    if (disc::probe_file(path, &in)) return from_info(in, out);
    if (ext == ".chd") return by_name(u8(p.filename()), out);
    return false;
}

/* ------------------------------------------------------------------ .cue for PCSX2 */

namespace {
struct Layout { uint32_t stride = 0, off = 0; bool plain = false; };   // plain: PCSX2 reads the file as it is

bool layout_of(const fs::path& bin, const std::string& mode, Layout* L) {
    std::ifstream f(bin, std::ios::binary);
    uint8_t h[16] = {};
    if (!f) return false;
    f.read((char*)h, sizeof h);
    if (f.gcount() != (std::streamsize)sizeof h) return false;
    static const uint8_t sync[12] = {0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0};
    if (std::memcmp(h, sync, 12) == 0) { L->stride = 2352; L->off = h[15] == 2 ? 24 : 16; L->plain = true; return true; }
    if (mode.find("2336") != std::string::npos) { L->stride = 2336; L->off = 8; L->plain = false; return true; }
    L->stride = 2048; L->off = 0; L->plain = true;
    return true;
}

bool build_iso(const fs::path& bin, const Layout& L, const fs::path& iso, std::string* err) {
    std::error_code ec;
    uintmax_t size = fs::file_size(bin, ec);
    if (ec || size < L.stride * 17u) { *err = "the disc image is too small to be a game"; return false; }
    fs::create_directories(iso.parent_path(), ec);
    fs::space_info sp = fs::space(iso.parent_path(), ec);
    uint64_t need = size / L.stride * 2048ull;
    if (!ec && sp.available < need + (64ull << 20)) { *err = "not enough free disk space (" + std::to_string((need >> 20) + 64) + " MB needed)"; return false; }
    fs::path part = iso;
    part += L".part";
    std::ifstream in(bin, std::ios::binary);
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    if (!in || !out) { *err = "cannot write " + u8(part); return false; }
    std::vector<char> sec(L.stride);
    for (uintmax_t n = size / L.stride; n > 0; --n) {
        in.read(sec.data(), (std::streamsize)sec.size());
        if (in.gcount() != (std::streamsize)sec.size()) break;
        out.write(sec.data() + L.off, 2048);
    }
    out.close();
    if (!out) { fs::remove(part, ec); *err = "cannot write " + u8(part); return false; }
    fs::remove(iso, ec);
    fs::rename(part, iso, ec);
    if (ec) { *err = "cannot write " + u8(iso); return false; }
    return true;
}
} // namespace

bool launch_image(const std::string& path, const std::string& out_dir, std::string* image, std::string* err) {
    std::string dummy;
    if (!err) err = &dummy;
    fs::path p = fs::u8path(path);
    if (lower(u8(p.extension())) != ".cue") { *image = path; return true; }
    std::string who = u8(p.filename());
    std::error_code ec;
    std::ifstream f(p, std::ios::binary);
    if (!f) { *err = "cannot read " + who; return false; }
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<ps1::CueFile> files = ps1::parse_cue(text);
    const ps1::CueFile* pick = nullptr;   // first data track
    for (const ps1::CueFile& c : files) if (c.mode.compare(0, 5, "AUDIO") != 0) { pick = &c; break; }
    if (!pick) { *err = who + " lists no data track"; return false; }
    fs::path bin = fs::u8path(pick->file);
    if (bin.is_relative()) bin = p.parent_path() / bin;
    if (!fs::is_regular_file(bin, ec)) {   // the cue may name the bin with another case / path separators
        fs::path alt = p.parent_path() / bin.filename();
        if (fs::is_regular_file(alt, ec)) bin = alt;
        else { *err = who + " points at " + u8(bin.filename()) + ", which is missing"; return false; }
    }
    Layout L;
    if (!layout_of(bin, pick->mode, &L)) { *err = "cannot read " + u8(bin.filename()); return false; }
    std::string iso = u8(fs::u8path(out_dir) / (p.stem().wstring() + L".iso"));
    std::string stamp = std::to_string(fs::file_size(bin, ec)) + "|" + std::to_string((long long)fs::last_write_time(bin, ec).time_since_epoch().count());
    if (L.plain) {
        *image = u8(bin);
    } else {
        fs::path sf = fs::u8path(out_dir) / L"stamp.txt";
        std::string have;
        { std::ifstream sin(sf, std::ios::binary); std::getline(sin, have); }
        if (have != stamp || !fs::is_regular_file(fs::u8path(iso), ec)) {
            if (!build_iso(bin, L, fs::u8path(iso), err)) return false;
            std::ofstream sout(sf, std::ios::binary | std::ios::trunc);
            sout << stamp << '\n';
        }
        *image = iso;
    }
    disc::Info info;
    if (!disc::probe_file(*image, &info) || info.kind == disc::Cnf::None) {
        if (!L.plain) { fs::remove(fs::u8path(iso), ec); }
        *err = "the disc inside " + who + " is not a readable PlayStation 2 disc";
        return false;
    }
    if (info.kind == disc::Cnf::Ps1) { *err = who + " is a PlayStation 1 disc, not PlayStation 2"; return false; }
    return true;
}

std::string key_for(const std::string& serial, const std::string& stem_key) {
    return serial.empty() ? "ps2:file-" + stem_key : "ps2:" + lower(serial);
}

}} // namespace hub::ps2
