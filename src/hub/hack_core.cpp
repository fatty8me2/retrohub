#include "hack_core.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace fs = std::filesystem;

namespace hubui {
namespace hacks {

namespace {
fs::path P(const std::string& u) { return fs::u8path(u); }
std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

bool read_all(const std::string& path, std::string* out) {
    std::ifstream f(P(path), std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

void swap_chunk(uint8_t* b, size_t n, char order) {   // v64: swap byte pairs; n64: reverse each 4 bytes (n is a multiple of 4)
    if (order == 'v') for (size_t i = 0; i + 1 < n; i += 2) std::swap(b[i], b[i + 1]);
    else if (order == 'n') for (size_t i = 0; i + 3 < n; i += 4) { std::swap(b[i], b[i + 3]); std::swap(b[i + 1], b[i + 2]); }
}
}  // namespace

char n64_order(const uint8_t* h) {
    if (h[0] == 0x80 && h[1] == 0x37 && h[2] == 0x12 && h[3] == 0x40) return 'z';
    if (h[0] == 0x37 && h[1] == 0x80 && h[2] == 0x40 && h[3] == 0x12) return 'v';
    if (h[0] == 0x40 && h[1] == 0x12 && h[2] == 0x37 && h[3] == 0x80) return 'n';
    return 0;
}

std::vector<Cand> candidates_from(const std::vector<hub::GameEntry>& lib) {
    std::vector<Cand> v;
    for (const auto& e : lib) {
        if (!e.hack_patch.empty() || !e.issue.empty() || e.path.empty() || e.size == 0) continue;
        if (e.catalog_id == "smash64") continue;
        switch (e.system) {   // consoles whose images patch like a plain file (GameCube: uncompressed .iso / .gcm only)
            case hub::System::N64: case hub::System::GBA: case hub::System::GB: case hub::System::GBC:
            case hub::System::NDS: case hub::System::NES: case hub::System::SNES: break;
            case hub::System::GC: { std::string x = lower(P(e.path).extension().u8string()); if (x != ".iso" && x != ".gcm") continue; break; }
            default: continue;
        }
        if (e.key.find(':') == std::string::npos || e.key.rfind("native", 0) == 0) continue;
        Cand c;
        c.key = e.key; c.title = e.title; c.path = e.path; c.system = e.system; c.size = e.size;
        v.push_back(c);
    }
    return v;
}

bool image_crcs(const Cand& c, ImageCrcs* out, std::string* err) {
    std::ifstream f(P(c.path), std::ios::binary);
    if (!f) { if (err) *err = "Could not read " + c.title + "."; return false; }
    ImageCrcs r;
    std::vector<uint8_t> buf(1 << 20);
    uint64_t off = 0;
    char order = 0;
    const bool n64 = c.system == hub::System::N64;
    uint32_t cr = 0, cz = 0, ch = 0;
    for (;;) {
        f.read((char*)buf.data(), (std::streamsize)buf.size());
        size_t n = (size_t)f.gcount();
        if (!n) break;
        if (off == 0 && n64 && n >= 4) order = n64_order(buf.data());
        cr = patch::crc32(buf.data(), n, cr);
        if (off + n > 512) {   // bytes from 512 on, for a copier-header-less CRC
            size_t s = off >= 512 ? 0 : (size_t)(512 - off);
            ch = patch::crc32(buf.data() + s, n - s, ch);
        }
        if (n64 && order) {
            std::vector<uint8_t> t(buf.begin(), buf.begin() + (long)n);
            swap_chunk(t.data(), n & ~(size_t)3, order == 'z' ? 0 : order);
            cz = patch::crc32(t.data(), n, cz);
        }
        off += n;
    }
    r.size = off; r.raw = cr;
    if (n64 && order) { r.has_z64 = true; r.z64 = cz; }
    if (off % 1024 == 512 && off > 512) { r.has_nohdr = true; r.nohdr = ch; r.nohdr_size = off - 512; }
    *out = r;
    return true;
}

int name_score(const std::string& a, const std::string& b) {
    auto words = [](const std::string& s) {
        std::set<std::string> w;
        std::string cur;
        for (unsigned char ch : s + " ") {
            if (std::isalnum(ch)) cur.push_back((char)std::tolower(ch));
            else { if (cur.size() >= 3) w.insert(cur); cur.clear(); }
        }
        return w;
    };
    auto wa = words(a), wb = words(b);
    int n = 0;
    for (const auto& x : wa) if (wb.count(x)) ++n;
    return n;
}

uint64_t ips_extent(const uint8_t* d, size_t n) {
    if (n < 8 || std::memcmp(d, "PATCH", 5) != 0) return 0;
    size_t p = 5;
    uint64_t ext = 0;
    while (p + 3 <= n) {
        if (d[p] == 'E' && d[p + 1] == 'O' && d[p + 2] == 'F') {
            if (p + 6 <= n) ext = std::max<uint64_t>(ext, (uint64_t)d[p + 3] << 16 | (uint64_t)d[p + 4] << 8 | d[p + 5]);
            return ext;
        }
        if (p + 5 > n) return 0;
        uint64_t off = (uint64_t)d[p] << 16 | (uint64_t)d[p + 1] << 8 | d[p + 2];
        uint64_t sz = (uint64_t)d[p + 3] << 8 | d[p + 4];
        p += 5;
        if (sz == 0) { if (p + 3 > n) return 0; sz = (uint64_t)d[p] << 8 | d[p + 1]; p += 3; }
        else { if (p + sz > n) return 0; p += (size_t)sz; }
        ext = std::max(ext, off + sz);
    }
    return 0;
}
uint64_t ips_extent_file(const std::string& patch_path) {
    std::string s;
    if (!read_all(patch_path, &s)) return 0;
    return ips_extent((const uint8_t*)s.data(), s.size());
}

std::string default_prep(const Cand& c) {
    if (c.system != hub::System::N64) return std::string();
    std::ifstream f(P(c.path), std::ios::binary);
    uint8_t h[4] = {0, 0, 0, 0};
    f.read((char*)h, 4);
    char o = n64_order(h);
    return (o == 'v' || o == 'n') ? "z64" : "";
}

FindResult find_base(const std::vector<Cand>& lib, const patch::Info& info, const std::string& patch_name, uint64_t ips_ext) {
    FindResult r;
    auto ranked = [&](std::vector<Cand> v) {
        std::stable_sort(v.begin(), v.end(), [&](const Cand& a, const Cand& b) {
            int sa = name_score(patch_name, a.title), sb = name_score(patch_name, b.title);
            if (sa != sb) return sa > sb;
            if (a.system != b.system) return (int)a.system < (int)b.system;
            return lower(a.title) < lower(b.title);
        });
        return v;
    };
    auto size_ok = [&](const Cand& c) {
        if (info.source_size) return c.size == info.source_size || (c.size % 1024 == 512 && c.size - 512 == info.source_size);
        return true;
    };
    if (info.has_source_crc) {   // UPS / BPS
        std::vector<Cand> plausible;
        std::set<std::string> done;
        for (const Cand& c : lib) {
            if (!size_ok(c) || !done.insert(c.key).second) continue;
            plausible.push_back(c);
            ImageCrcs ic;
            if (!image_crcs(c, &ic, nullptr)) continue;
            const uint64_t ss = info.source_size;
            if (ic.raw == info.source_crc && (!ss || ic.size == ss)) { r.status = FindStatus::Found; r.base = c; r.prep = ""; return r; }
            if (ic.has_z64 && ic.z64 == info.source_crc && (!ss || ic.size == ss)) { r.status = FindStatus::Found; r.base = c; r.prep = "z64"; return r; }
            if (ic.has_nohdr && ic.nohdr == info.source_crc && (!ss || ic.nohdr_size == ss)) { r.status = FindStatus::Found; r.base = c; r.prep = "nohdr"; return r; }
        }
        if (!plausible.empty()) {
            plausible = ranked(plausible);
            r.status = FindStatus::WrongVersion;
            r.message = "This patch is for a different version of " + plausible[0].title + ".";
        } else {
            r.status = FindStatus::NotFound;
            r.message = "RetroHub could not find the game this patch is for. Add the original game to your library first, then try again.";
        }
        return r;
    }
    for (const Cand& c : lib) {   // IPS / VCDIFF: the player chooses
        if (info.source_size ? !size_ok(c) : (ips_ext && c.size < ips_ext)) continue;
        r.choices.push_back(c);
    }
    r.choices = ranked(r.choices);
    if (r.choices.empty()) {
        r.status = FindStatus::NotFound;
        r.message = "None of your games fit this patch. Add the original game to your library first, then try again.";
    } else r.status = FindStatus::Choose;
    return r;
}

bool prepare_source(const std::string& base_path, const std::string& prep, const std::string& tmp_path, std::string* err,
                    const std::function<bool(double)>& progress) {
    std::ifstream in(P(base_path), std::ios::binary);
    std::error_code ec;
    uint64_t total = fs::file_size(P(base_path), ec);
    if (!in || ec) { if (err) *err = "Could not read the original game."; return false; }
    fs::create_directories(P(tmp_path).parent_path(), ec);
    std::ofstream out(P(tmp_path), std::ios::binary | std::ios::trunc);
    if (!out) { if (err) *err = "Could not write to the RetroHub cache folder."; return false; }
    std::vector<uint8_t> buf(1 << 20);
    char order = 0;
    if (prep == "nohdr") in.seekg(512);
    uint64_t done = 0;
    bool first = true;
    for (;;) {
        in.read((char*)buf.data(), (std::streamsize)buf.size());
        size_t n = (size_t)in.gcount();
        if (!n) break;
        if (first && prep == "z64" && n >= 4) order = n64_order(buf.data());
        first = false;
        if (prep == "z64" && (order == 'v' || order == 'n')) swap_chunk(buf.data(), n & ~(size_t)3, order);
        out.write((const char*)buf.data(), (std::streamsize)n);
        done += n;
        if (progress && total && !progress((double)done / (double)total)) { out.close(); fs::remove(P(tmp_path), ec); if (err) *err = "Cancelled."; return false; }
    }
    out.close();
    if (!out) { fs::remove(P(tmp_path), ec); if (err) *err = "Not enough disk space to prepare the game."; return false; }
    return true;
}

BuildResult build_patched(const std::string& base_path, const std::string& prep, const std::string& patch_path, const std::string& out_path,
                          const std::function<bool(double)>& progress) {
    BuildResult r;
    std::error_code ec;
    std::string src = base_path, tmp;
    const bool conv = prep == "z64" || prep == "nohdr";
    auto sub = [&](double a, double b) { return [&progress, a, b](double x) { return progress ? progress(a + (b - a) * x) : true; }; };
    if (conv) {
        tmp = out_path + ".src";
        std::string err;
        if (!prepare_source(base_path, prep, tmp, &err, sub(0, 0.2))) { r.message = err; return r; }
        src = tmp;
    }
    patch::Result pr = patch::apply_file(src, patch_path, out_path, 1ull << 31, sub(conv ? 0.2 : 0, 0.9));
    if (!tmp.empty()) fs::remove(P(tmp), ec);
    if (!pr.ok()) { r.message = pr.message.empty() ? "The patch could not be applied." : pr.message; return r; }
    std::string err;
    if (!patch::crc32_file(out_path, &r.out_crc, &err, sub(0.9, 1.0))) { fs::remove(P(out_path), ec); r.message = "Could not check the patched game."; return r; }
    r.out_size = fs::file_size(P(out_path), ec);
    r.ok = true;
    return r;
}

std::string patched_name(hub::System s, const std::string& base_path) {
    if (s == hub::System::N64) return "hack.z64";
    std::string e = lower(P(base_path).extension().u8string());
    if (e.empty() || e.size() > 6) e = ".rom";
    return "hack" + e;
}

std::string cache_folder(const std::string& key) {
    std::string o;
    for (unsigned char c : key) o.push_back((std::isalnum(c) || c == '.' || c == '_') ? (char)std::tolower(c) : '-');
    return o;
}

std::vector<Found> read_vault(const std::string& vault_dir) {
    std::vector<Found> v;
    std::error_code ec;
    for (fs::directory_iterator s(P(vault_dir), ec), se; !ec && s != se; s.increment(ec)) {
        std::error_code e1;
        if (!s->is_directory(e1)) continue;
        for (fs::directory_iterator g(s->path(), e1), ge; !e1 && g != ge; g.increment(e1)) {
            fs::path hj = g->path() / "patch" / "hack.json";
            std::string text;
            std::error_code e2;
            if (!fs::is_regular_file(hj, e2) || fs::file_size(hj, e2) > (1u << 16) || !read_all(hj.u8string(), &text)) continue;
            Found f;
            if (!decode(text, &f.def)) continue;
            f.dir = (g->path() / "patch").u8string();
            f.patch_path = (g->path() / "patch" / P(f.def.patch_file)).u8string();
            if (!fs::is_regular_file(P(f.patch_path), e2)) continue;
            v.push_back(std::move(f));
        }
    }
    return v;
}

hub::GameEntry make_entry(const HackDef& d, const std::string& patch_path, const hub::GameEntry& base) {
    hub::GameEntry e;
    e.key = d.key;
    e.system = base.system;
    e.title = d.title;
    e.serial = base.serial;
    e.path = base.path;
    e.size = d.out_size;
    e.catalog_id.clear();
    e.hack_patch = patch_path;
    e.hack_base = d.base_key;
    return e;
}

int append_hacks(const std::string& vault_dir, std::vector<hub::GameEntry>* out) {
    std::vector<Found> found = read_vault(vault_dir);
    if (found.empty()) return 0;
    std::set<std::string> have;
    for (const auto& e : *out) have.insert(e.key);
    std::stable_sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.def.title < b.def.title; });
    int added = 0;
    for (const Found& f : found) {
        if (have.count(f.def.key)) continue;
        size_t bi = out->size();
        for (size_t i = 0; i < out->size(); ++i)
            if ((*out)[i].key == f.def.base_key && (*out)[i].hack_patch.empty() && (*out)[i].issue.empty() && !(*out)[i].path.empty()) { bi = i; break; }
        if (bi == out->size()) continue;   // base game missing: not shown
        hub::GameEntry e = make_entry(f.def, f.patch_path, (*out)[bi]);
        size_t at = bi + 1;
        while (at < out->size() && !(*out)[at].hack_patch.empty() && (*out)[at].hack_base == f.def.base_key) ++at;
        out->insert(out->begin() + (long)at, e);
        have.insert(f.def.key);
        ++added;
    }
    return added;
}

bool install_hack(const std::string& game_dir, const HackDef& d, const std::string& patch_src, std::string* err) {
    std::error_code ec;
    fs::path dir = P(game_dir) / "patch";
    fs::create_directories(dir, ec);
    if (ec) { if (err) *err = "Could not create the hack's folder."; return false; }
    fs::copy_file(P(patch_src), dir / P(d.patch_file), fs::copy_options::overwrite_existing, ec);
    if (ec) { if (err) *err = "Could not copy the patch file."; return false; }
    fs::path tmp = dir / "hack.json.tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << encode(d);
        if (!f) { if (err) *err = "Could not write hack.json."; return false; }
    }
    fs::rename(tmp, dir / "hack.json", ec);
    if (ec) { fs::remove(dir / "hack.json", ec); ec.clear(); fs::rename(tmp, dir / "hack.json", ec); }
    if (ec) { if (err) *err = "Could not write hack.json."; return false; }
    return true;
}

bool remove_hack_files(const std::string& game_dir) {
    std::error_code ec;
    fs::path dir = P(game_dir) / "patch";
    if (!fs::is_regular_file(dir / "hack.json", ec)) return false;
    fs::remove_all(dir, ec);
    return !ec;
}

std::string load_def_next_to(const std::string& patch_path, HackDef* out) {
    std::string text;
    fs::path hj = P(patch_path).parent_path() / "hack.json";
    if (!read_all(hj.u8string(), &text)) return "The hack's settings file is missing.";
    if (!decode(text, out)) return "The hack's settings file is damaged.";
    return std::string();
}

std::string ribbon_for(const HackDef& d) { return d.kind == "translation" ? "TRANSLATION" : "HACK"; }

}  // namespace hacks
}  // namespace hubui
