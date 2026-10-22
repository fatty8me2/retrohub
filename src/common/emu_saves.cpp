#include "emu_saves.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace hub {

namespace fs = std::filesystem;

namespace {

fs::path u8(const std::string& s) { return fs::u8path(s); }
std::string to_u8(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string hex8(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%08x", v);
    return b;
}

std::string with_sep(std::string d) {
    if (!d.empty() && d.back() != '\\' && d.back() != '/') d.push_back('\\');
    return d;
}

bool same_content(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (fs::file_size(a, ec) != fs::file_size(b, ec)) return false;
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    char ba[65536], bb[65536];
    for (;;) {
        fa.read(ba, sizeof ba);
        fb.read(bb, sizeof bb);
        std::streamsize na = fa.gcount(), nb = fb.gcount();
        if (na != nb) return false;
        if (na == 0) return true;
        if (std::memcmp(ba, bb, (size_t)na) != 0) return false;
    }
}

bool copy_keep_time(const fs::path& from, const fs::path& to, std::error_code& ec) {
    fs::create_directories(to.parent_path(), ec);
    if (ec) return false;
    fs::path tmp = to;
    tmp += L".hubtmp";
    fs::copy_file(from, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    auto t = fs::last_write_time(from, ec);
    if (!ec) fs::last_write_time(tmp, t, ec);
    ec.clear();
    fs::rename(tmp, to, ec);   // replaces an existing file on Windows
    if (ec) { std::error_code e2; fs::remove(tmp, e2); return false; }
    return true;
}

} // namespace

std::string n3ds_save_rel(uint64_t t) {
    return "title/" + hex8(uint32_t(t >> 32)) + "/" + hex8(uint32_t(t & 0xFFFFFFFFu)) + "/data";
}
std::string n3ds_extdata_rel(uint64_t t) {
    return "extdata/00000000/" + hex8(uint32_t(t & 0xFFFFFFFFu));
}

std::string n3ds_sd_root(const std::string& user) {
    fs::path base = u8(with_sep(user)) / L"sdmc" / L"Nintendo 3DS";
    std::error_code ec;
    std::vector<std::string> id0s;
    for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec)) id0s.push_back(to_u8(it->path().filename()));
    std::sort(id0s.begin(), id0s.end());
    for (const auto& a : id0s) {
        std::vector<std::string> id1s;
        std::error_code e2;
        for (fs::directory_iterator it(base / u8(a), e2), end; !e2 && it != end; it.increment(e2))
            if (it->is_directory(e2)) id1s.push_back(to_u8(it->path().filename()));
        std::sort(id1s.begin(), id1s.end());
        if (!id1s.empty()) return to_u8(base / u8(a) / u8(id1s.front()));
    }
    const std::string z(32, '0');
    return to_u8(base / u8(z) / u8(z));
}

std::string wiiu_save_rel(uint64_t t) {
    return "usr/save/" + hex8(uint32_t(t >> 32)) + "/" + hex8(uint32_t(t & 0xFFFFFFFFu));
}

std::string cemu_settings_mlc_path(const std::string& xml) {
    size_t a = xml.find("<mlc_path>");
    if (a == std::string::npos) return "";
    a += 10;
    size_t b = xml.find("</mlc_path>", a);
    if (b == std::string::npos) return "";
    std::string v = xml.substr(a, b - a);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\r' || v.back() == '\n')) v.pop_back();
    size_t s = 0;
    while (s < v.size() && (v[s] == ' ' || v[s] == '\r' || v[s] == '\n')) ++s;
    return v.substr(s);
}

CemuDirs cemu_dirs_for(const std::string& exe_dir, const std::string& appdata, const FsProbe& p) {
    CemuDirs d;
    std::string ed = with_sep(exe_dir);
    if (p.dir_exists(ed + "portable")) {
        d.user_data = ed + "portable";
        d.portable = true;
    } else if (p.file_exists(ed + "settings.xml")) {
        d.user_data = exe_dir;
        d.portable = true;
        while (d.user_data.size() > 3 && (d.user_data.back() == '\\' || d.user_data.back() == '/')) d.user_data.pop_back();
    } else {
        d.user_data = with_sep(appdata) + "Cemu";
    }
    std::string settings = p.read_text ? p.read_text(with_sep(d.user_data) + "settings.xml") : std::string();
    std::string mlc = cemu_settings_mlc_path(settings);
    d.mlc = mlc.empty() ? with_sep(d.user_data) + "mlc01" : mlc;
    return d;
}

bool tree_has_files(const std::string& dir) {
    std::error_code ec;
    if (!fs::is_directory(u8(dir), ec)) return false;
    for (fs::recursive_directory_iterator it(u8(dir), ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) return true;
    return false;
}

TreeSyncReport sync_tree_newest(const std::string& src, const std::string& dst, const std::string& backup_dir,
                                const std::string& tag) {
    TreeSyncReport r;
    std::error_code ec;
    fs::path s = u8(src), d = u8(dst);
    if (!fs::is_directory(s, ec)) return r;
    for (fs::recursive_directory_iterator it(s, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (it->path().extension() == L".hubtmp") continue;
        fs::path rel = fs::relative(it->path(), s, ec);
        fs::path to = d / rel;
        std::error_code e1;
        if (!fs::exists(to, e1)) {
            if (copy_keep_time(it->path(), to, e1)) ++r.copied;
            else { r.ok = false; r.error = "cannot write " + to_u8(to); }
            continue;
        }
        if (same_content(it->path(), to)) { ++r.identical; continue; }
        auto ts = fs::last_write_time(it->path(), e1);
        auto td = fs::last_write_time(to, e1);
        if (ts <= td) { ++r.kept; continue; }   // the destination is newer (or the same age): it wins
        if (!backup_dir.empty()) {
            fs::path bk = u8(backup_dir) / rel;
            bk += u8("." + tag);
            if (copy_keep_time(to, bk, e1)) ++r.backed_up;
        }
        if (copy_keep_time(it->path(), to, e1)) ++r.copied;
        else { r.ok = false; r.error = "cannot write " + to_u8(to); }
    }
    return r;
}

std::vector<std::string> subdirs_modified_since(const std::string& dir, int64_t since_unix) {
    std::vector<std::string> out;
    std::error_code ec;
    fs::path base = u8(dir);
    if (!fs::is_directory(base, ec)) return out;
    auto since = std::chrono::system_clock::from_time_t((time_t)since_unix);
    for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        bool hit = false;
        std::error_code e2;
        for (fs::recursive_directory_iterator f(it->path(), e2), fe; !e2 && f != fe && !hit; f.increment(e2)) {
            if (!f->is_regular_file(e2)) continue;
            auto ft = fs::last_write_time(f->path(), e2);
            auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            if (sys >= since) hit = true;
        }
        if (hit) out.push_back(to_u8(it->path().filename()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace hub
