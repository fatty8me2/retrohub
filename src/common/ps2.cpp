#include "ps2.h"

#include <cstring>
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
