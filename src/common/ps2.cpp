#include "ps2.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <filesystem>
#include <fstream>

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

std::string key_for(const std::string& serial, const std::string& stem_key) {
    return serial.empty() ? "ps2:file-" + stem_key : "ps2:" + lower(serial);
}

}} // namespace hub::ps2
