#include "lichii.h"
/* Lichii config / keys / firmware / profile (lichii.h). Pure text transforms + small file helpers; unit-tested by test_switch.cpp. */

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

#include "logic.h"
#include "miniz.h"
#include "winutil.h"

namespace hubemu {

namespace fs = std::filesystem;

static fs::path P(const std::string& s) { return fs::u8path(s); }
static std::string S(const fs::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); }
static std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    return (a.back() == '\\' || a.back() == '/') ? a + b : a + "\\" + b;
}
static std::string read_text(const std::string& p) {
    std::ifstream f(P(p), std::ios::binary);
    return f ? std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()) : std::string();
}
static bool write_bytes(const std::string& p, const std::string& t) {
    std::error_code ec;
    fs::create_directories(P(p).parent_path(), ec);
    std::ofstream f(P(p), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(t.data(), (std::streamsize)t.size());
    return (bool)f;
}
static bool is_file(const std::string& p) { std::error_code ec; return fs::is_regular_file(P(p), ec); }
static bool is_dir(const std::string& p) { std::error_code ec; return fs::is_directory(P(p), ec); }
static uintmax_t file_size(const std::string& p) { std::error_code ec; auto n = fs::file_size(P(p), ec); return ec ? 0 : n; }
static std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
static bool ends_with(const std::string& s, const char* suf) {
    size_t n = std::strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

std::string lichii_user_dir(const std::string& install_dir) { return join(install_dir, "user"); }

static std::string keys_dir(const std::string& install) { return join(lichii_user_dir(install), "keys"); }
static std::string registered_dir(const std::string& install) { return join(lichii_user_dir(install), "nand\\system\\Contents\\registered"); }
static std::string profiles_path(const std::string& install) {
    return join(lichii_user_dir(install), "nand\\system\\save\\8000000000000010\\su\\avators\\profiles.dat");
}
static std::string config_dir(const std::string& install) { return join(lichii_user_dir(install), "config"); }

/* ------------------------------------------------------------------ setup status */

static bool dir_has_nca(const std::string& dir) {
    std::error_code ec;
    for (fs::directory_iterator it(P(dir), ec), end; !ec && it != end; it.increment(ec)) {
        if (ends_with(lower(it->path().filename().string()), ".nca")) return true;
    }
    return false;
}

NxSetup lichii_setup_status(const std::string& install) {
    NxSetup s;
    s.prod_keys = file_size(join(keys_dir(install), "prod.keys")) > 0;
    s.title_keys = file_size(join(keys_dir(install), "title.keys")) > 0;
    s.firmware = dir_has_nca(registered_dir(install));
    return s;
}

bool lichii_needs_keys(const std::string& game_path) {
    std::string n = lower(S(P(game_path).filename()));
    for (const char* e : {".nsp", ".nsz", ".xci", ".xcz"})
        if (ends_with(n, e)) return true;
    return false;   // .nro, loose exefs\main (no extension), anything else
}

/* ------------------------------------------------------------------ finding keys / firmware elsewhere on this PC */

static bool valid_keys_text(const std::string& t) {   // a "header_key = ..." line
    size_t p = 0;
    while (p < t.size()) {
        size_t e = t.find_first_of("\r\n", p);
        std::string line = t.substr(p, e == std::string::npos ? std::string::npos : e - p);
        size_t a = line.find_first_not_of(" \t");
        if (a != std::string::npos && line.compare(a, 10, "header_key") == 0) {
            size_t q = line.find_first_not_of(" \t", a + 10);
            if (q != std::string::npos && line[q] == '=') return true;
        }
        if (e == std::string::npos) break;
        p = e + 1;
    }
    return false;
}

std::vector<NxSource> lichii_find_sources(const std::string& appdata, const std::vector<std::string>& extra_roots) {
    std::vector<NxSource> out;
    auto add = [&](const std::string& label, const std::string& keys, const std::string& fw) {
        NxSource s;
        s.label = label;
        if (!keys.empty() && file_size(join(keys, "prod.keys")) > 0) s.keys_dir = keys;
        if (!fw.empty() && dir_has_nca(fw)) s.firmware_dir = fw;
        if (!s.keys_dir.empty() || !s.firmware_dir.empty()) out.push_back(s);
    };
    if (!appdata.empty()) {
        static const char* const names[][2] = {{"Eden", "eden"}, {"yuzu", "yuzu"}, {"Citron", "citron"}, {"Suyu", "suyu"}, {"Sudachi", "sudachi"}};
        for (const auto& n : names) {
            std::string base = join(appdata, n[1]);
            add(n[0], join(base, "keys"), join(base, "nand\\system\\Contents\\registered"));
        }
        std::string ry = join(appdata, "Ryujinx");
        add("Ryujinx", join(ry, "system"), join(ry, "bis\\system\\Contents\\registered"));
    }
    for (const std::string& r : extra_roots) {
        if (r.empty()) continue;
        std::string u = join(r, "user");
        std::string leaf = S(P(r).filename());
        add("Portable " + (leaf.empty() ? r : leaf), join(u, "keys"), join(u, "nand\\system\\Contents\\registered"));
    }
    return out;
}

/* ------------------------------------------------------------------ import */

static bool same_file(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    return fs::equivalent(a, b, ec) && !ec;
}

static std::string copy_keys_file(const fs::path& from, const std::string& to) {
    if (same_file(from, P(to))) return "";
    std::error_code ec;
    fs::create_directories(P(to).parent_path(), ec);
    const fs::path tmp = P(to + ".retrohub-new");   // copied beside it first, then swapped in: a failed copy never leaves a half-written key file
    fs::copy_file(from, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) { std::error_code e2; fs::remove(tmp, e2); return "could not copy " + S(from.filename()) + ": " + ec.message(); }
    fs::rename(tmp, P(to), ec);
    if (ec) { std::error_code e2; fs::remove(tmp, e2); return "could not replace " + S(P(to).filename()) + ": " + ec.message(); }
    return "";
}

std::string lichii_import_keys(const std::string& install, const std::string& from) {
    if (install.empty() || from.empty()) return "no keys file chosen";
    fs::path prod, title;
    std::error_code ec;
    if (fs::is_directory(P(from), ec)) {
        for (const char* rel : {"prod.keys", "keys\\prod.keys", "system\\prod.keys"})
            if (prod.empty() && is_file(join(from, rel))) prod = P(join(from, rel));
        for (const char* rel : {"title.keys", "keys\\title.keys", "system\\title.keys"})
            if (title.empty() && is_file(join(from, rel))) title = P(join(from, rel));
        if (prod.empty()) return "that folder has no prod.keys (dump it from your own Switch with Lockpick_RCM)";
    } else if (fs::is_regular_file(P(from), ec)) {
        std::string n = lower(S(P(from).filename()));
        if (n == "title.keys") title = P(from);
        else if (ends_with(n, ".keys")) prod = P(from);
        else return "pick a prod.keys file (or the folder that holds it)";
    } else {
        return "that file or folder does not exist";
    }
    if (!prod.empty()) {
        if (file_size(S(prod)) == 0) return "prod.keys is empty";
        if (file_size(S(prod)) > (4u << 20)) return "that file is too big to be a prod.keys";
        if (!valid_keys_text(read_text(S(prod)))) return "that prod.keys is not valid: it has no header_key line (use the prod.keys dumped from your own Switch with Lockpick_RCM)";
        std::string e = copy_keys_file(prod, join(keys_dir(install), "prod.keys"));
        if (!e.empty()) return e;
    }
    if (!title.empty() && file_size(S(title)) > 0) {
        std::string e = copy_keys_file(title, join(keys_dir(install), "title.keys"));
        if (!e.empty()) return e;
    }
    return "";
}

/* Entry names from the zip: relative, no "..", no drive or root. */
static bool safe_zip_name(const std::string& n) {
    if (n.empty() || n[0] == '/' || n[0] == '\\' || n.find(':') != std::string::npos) return false;
    size_t p = 0;
    while (p <= n.size()) {
        size_t q = n.find_first_of("/\\", p);
        std::string part = n.substr(p, q == std::string::npos ? std::string::npos : q - p);
        if (part == "..") return false;
        if (q == std::string::npos) break;
        p = q + 1;
    }
    return true;
}

/* Only the .nca entries (and Ryujinx's "<name>.nca/00") are unpacked. */
static std::string unzip_nca(const std::string& archive, const std::string& dest) {
    mz_zip_archive za;
    std::memset(&za, 0, sizeof za);
    FILE* zf = _wfopen(P(archive).wstring().c_str(), L"rb");
    if (!zf) return "cannot open the firmware zip";
    if (!mz_zip_reader_init_cfile(&za, zf, 0, 0)) { std::fclose(zf); return "that is not a valid zip"; }
    std::string err;
    mz_uint n = mz_zip_reader_get_num_files(&za);
    uint64_t total = 0;
    if (n > 20000) { mz_zip_reader_end(&za); std::fclose(zf); return "that zip has too many files to be a Switch firmware"; }
    for (mz_uint i = 0; i < n && err.empty(); i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&za, i, &st)) { err = "damaged zip entry"; break; }
        std::string name = st.m_filename;
        if (!safe_zip_name(name)) { err = "unsafe path in the zip: " + name; break; }
        if (st.m_is_directory || name.back() == '/' || name.back() == '\\') continue;
        { const std::string ln = lower(name); if (!ends_with(ln, ".nca") && !ends_with(ln, ".nca/00") && !ends_with(ln, ".nca\\00")) continue; }
        total += st.m_uncomp_size;
        if (st.m_uncomp_size > (1ull << 30) || total > (4ull << 30)) { err = "that zip is far bigger than a Switch firmware"; break; }
        fs::path target = P(dest) / fs::u8path(name);
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        FILE* out = _wfopen(target.wstring().c_str(), L"wb");
        if (!out) { err = "cannot write " + name; break; }
        bool r = mz_zip_reader_extract_to_cfile(&za, i, out, 0) != 0;
        bool flushed = std::fflush(out) == 0;
        std::fclose(out);
        if (!r || !flushed) err = "cannot extract " + name;
    }
    mz_zip_reader_end(&za);
    std::fclose(zf);
    return err;
}

/* <name>.nca files (and Ryujinx's folders named <name>.nca holding a file "00") below dir, keyed by lower-case file name. */
static void collect_nca(const fs::path& dir, std::map<std::string, fs::path>* out, int depth) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        std::string name = p.filename().string();
        bool nca = ends_with(lower(name), ".nca");
        std::error_code e2;
        if (fs::is_directory(p, e2)) {
            if (nca) {
                fs::path inner = p / "00";
                if (fs::is_regular_file(inner, e2)) out->emplace(lower(name), inner);
            } else if (depth < 8) {
                collect_nca(p, out, depth + 1);
            }
        } else if (nca && fs::is_regular_file(p, e2)) {
            out->emplace(lower(name), p);
        }
    }
}

std::string lichii_import_firmware(const std::string& install, const std::string& from) {
    if (install.empty() || from.empty()) return "no firmware chosen";
    std::error_code ec;
    const std::string reg = registered_dir(install);
    const std::string tmp = join(lichii_user_dir(install), "nand\\system\\Contents\\.retrohub-fw-new");
    const std::string unz = join(lichii_user_dir(install), "nand\\system\\Contents\\.retrohub-fw-unzip");
    fs::path src = P(from);
    if (!fs::exists(src, ec)) return "that file or folder does not exist";
    {   // never wipe the folder we are copying from
        fs::path a = fs::weakly_canonical(src, ec), b = fs::weakly_canonical(P(reg), ec);
        std::string as = lower(a.generic_string()), bs = lower(b.generic_string());
        if (!as.empty() && as.compare(0, bs.size(), bs) == 0 && (as.size() == bs.size() || as[bs.size()] == '/'))
            return "that is already Lichii's firmware folder";
    }
    fs::remove_all(P(tmp), ec);
    fs::remove_all(P(unz), ec);
    fs::path scan = src;
    bool zipped = false;
    if (fs::is_regular_file(src, ec)) {
        if (!ends_with(lower(S(src)), ".zip")) return "pick the firmware folder or its .zip";
        std::string e = unzip_nca(from, unz);
        if (!e.empty()) { fs::remove_all(P(unz), ec); return e; }
        scan = P(unz);
        zipped = true;
    }
    std::map<std::string, fs::path> files;
    collect_nca(scan, &files, 0);
    if (files.empty()) {
        if (zipped) fs::remove_all(P(unz), ec);
        return "no firmware files (.nca) found there";
    }
    fs::create_directories(P(tmp), ec);
    std::string err;
    fs::remove_all(P(tmp), ec);
    fs::create_directories(P(tmp), ec);
    for (const auto& kv : files) {
        std::string leaf = kv.second.filename() == "00" ? kv.second.parent_path().filename().string() : kv.second.filename().string();
        fs::copy_file(kv.second, P(tmp) / fs::u8path(leaf), fs::copy_options::overwrite_existing, ec);
        if (ec) { err = "could not copy " + leaf + ": " + ec.message(); break; }
    }
    if (zipped) fs::remove_all(P(unz), ec);
    if (!err.empty()) { fs::remove_all(P(tmp), ec); return err; }
    /* Swap: the old firmware is moved aside first and only deleted once the new folder is in place; any failure puts it back. */
    const std::string old = join(lichii_user_dir(install), "nand\\system\\Contents\\.retrohub-fw-old");
    fs::remove_all(P(old), ec);
    fs::create_directories(P(reg).parent_path(), ec);
    const bool had_old = fs::exists(P(reg), ec);
    if (had_old) {
        fs::rename(P(reg), P(old), ec);
        if (ec) { std::error_code e2; fs::remove_all(P(tmp), e2); return "could not replace the firmware (is Lichii still running?): " + ec.message(); }
    }
    fs::rename(P(tmp), P(reg), ec);
    if (ec) {
        std::error_code e2;
        if (had_old) fs::rename(P(old), P(reg), e2);
        fs::remove_all(P(tmp), e2);
        return "could not install the firmware files: " + ec.message();
    }
    fs::remove_all(P(old), ec);
    return "";
}

/* ------------------------------------------------------------------ profile */

/* uuid text = {uuid[1]:016X}{uuid[0]:016X}; the file stores uuid[0] then uuid[1] as little-endian u64 (random value, fixed once for RetroHub). */
const char* const kLichiiUserUuid = "5A3C9E1B7D2F4086C41E8B05A97D3F62";

static uint64_t hex64(const std::string& s) { return std::strtoull(s.c_str(), nullptr, 16); }
static void put64(std::string& b, size_t at, uint64_t v) { for (int i = 0; i < 8; ++i) b[at + i] = (char)(uint8_t)(v >> (8 * i)); }
static uint64_t get64(const std::string& b, size_t at) { uint64_t v = 0; for (int i = 7; i >= 0; --i) v = (v << 8) | (uint8_t)b[at + i]; return v; }

static const size_t kProfHeader = 0x10, kUserSize = 0xC8, kMaxUsers = 8, kProfSize = 0x650;

static std::string uuid_text(uint64_t u0, uint64_t u1) {
    char b[40];
    std::snprintf(b, sizeof b, "%016llX%016llX", (unsigned long long)u1, (unsigned long long)u0);
    return b;
}

std::string lichii_ensure_profile(const std::string& install) {
    const std::string path = profiles_path(install);
    std::string cur = read_text(path);
    if (cur.size() >= kProfHeader + kUserSize) {
        for (size_t i = 0; i < kMaxUsers && kProfHeader + (i + 1) * kUserSize <= cur.size(); ++i) {
            uint64_t u0 = get64(cur, kProfHeader + i * kUserSize), u1 = get64(cur, kProfHeader + i * kUserSize + 8);
            if (u0 || u1) return uuid_text(u0, u1);   // an existing user list is never altered
        }
    }
    std::string raw(kProfSize, '\0');
    const std::string u(kLichiiUserUuid);
    uint64_t hi = hex64(u.substr(0, 16)), lo = hex64(u.substr(16, 16));
    size_t at = kProfHeader;
    put64(raw, at, lo);
    put64(raw, at + 8, hi);
    put64(raw, at + 16, lo);   // uuid2 = uuid
    put64(raw, at + 24, hi);
    put64(raw, at + 32, 0);    // timestamp
    std::memcpy(&raw[at + 40], "Player", 6);
    if (!write_bytes(path, raw)) return "";
    return kLichiiUserUuid;
}

std::string lichii_save_rel(const std::string& uuid, uint64_t tid) {
    char b[24];
    std::snprintf(b, sizeof b, "%016llX", (unsigned long long)tid);
    return "nand/user/save/0000000000000000/" + uuid + "/" + b;
}

/* ------------------------------------------------------------------ qt-config.ini */

/* Sets [section] key=value and its "key\default=false" twin; other keys and sections are kept. SimpleIni / Qt syntax (no spaces around "="). */
static std::string ini_put(const std::string& text, const std::string& section, const std::string& key, const std::string& value) {
    std::vector<std::string> lines;
    {
        size_t p = 0;
        while (p < text.size()) {
            size_t e = text.find('\n', p);
            std::string l = text.substr(p, e == std::string::npos ? std::string::npos : e - p);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            lines.push_back(l);
            if (e == std::string::npos) break;
            p = e + 1;
        }
    }
    auto sec_name = [](const std::string& l, std::string* n) {
        size_t a = l.find_first_not_of(" \t");
        if (a == std::string::npos || l[a] != '[') return false;
        size_t b = l.find(']', a);
        if (b == std::string::npos) return false;
        *n = l.substr(a + 1, b - a - 1);
        return true;
    };
    auto is_key = [&](const std::string& l) {
        size_t a = l.find_first_not_of(" \t");
        if (a == std::string::npos) return false;
        std::string s = l.substr(a);
        return s.compare(0, key.size() + 1, key + "=") == 0 || s.compare(0, key.size() + 9, key + "\\default=") == 0;
    };
    int start = -1, end = (int)lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string n;
        if (sec_name(lines[i], &n)) {
            if (start >= 0) { end = (int)i; break; }
            if (n == section) start = (int)i;
        }
    }
    std::vector<std::string> pair = {key + "\\default=false", key + "=" + value};
    if (start < 0) {
        while (!lines.empty() && lines.back().empty()) lines.pop_back();
        if (!lines.empty()) lines.push_back("");
        lines.push_back("[" + section + "]");
        lines.insert(lines.end(), pair.begin(), pair.end());
    } else {
        int first = -1;
        std::vector<std::string> kept;
        for (int i = start + 1; i < end; ++i) {
            if (is_key(lines[i])) { if (first < 0) first = (int)kept.size(); continue; }
            kept.push_back(lines[i]);
        }
        if (first < 0) {
            first = (int)kept.size();
            while (first > 0 && kept[first - 1].empty()) --first;   // before trailing blank lines
        }
        kept.insert(kept.begin() + first, pair.begin(), pair.end());
        std::vector<std::string> o(lines.begin(), lines.begin() + start + 1);
        o.insert(o.end(), kept.begin(), kept.end());
        o.insert(o.end(), lines.begin() + end, lines.end());
        lines.swap(o);
    }
    std::string o;
    for (const auto& l : lines) o += l + "\n";
    return o;
}

std::string lichii_ini_defaults(const std::string& in) {
    std::string t = in;
    t = ini_put(t, "UI", "firstStart", "false");
    t = ini_put(t, "UI", "check_for_updates", "false");
    t = ini_put(t, "UI", "confirmStop", "2");
    t = ini_put(t, "UI", "showConsole", "false");
    t = ini_put(t, "UI", "pauseWhenInBackground", "false");
    t = ini_put(t, "UI", "select_user_on_boot", "false");
    t = ini_put(t, "UI", "enable_discord_presence", "false");
    t = ini_put(t, "UI", "singleWindowMode", "true");
    t = ini_put(t, "Renderer", "fullscreen_mode", "0");
    return t;
}

static bool all_digits(const std::string& s) { return !s.empty() && s.size() < 4 && s.find_first_not_of("0123456789") == std::string::npos; }

std::string lichii_ini_launch(const std::string& in, const hub::gs::Values& v, bool fullscreen, const std::vector<std::string>* skip_keys) {
    auto skipped = [&](const char* sec, const char* key) { if (!skip_keys) return false; const std::string id = std::string(sec) + "." + key; for (const auto& k : *skip_keys) if (k == id) return true; return false; };
    auto put = [&](std::string& t, const char* sec, const char* key, const std::string& val) { if (!skipped(sec, key)) t = ini_put(t, sec, key, val); };
    auto get = [&](const char* k, const char* def) { auto i = v.find(k); return i == v.end() || i->second.empty() ? std::string(def) : i->second; };
    std::string res = get("switch.res", "3"), fg = get("switch.framegen", "off"), vs = get("switch.vsync", "fifo");
    if (!all_digits(res)) res = "3";
    /* Frame pacing (Lichii FramePacingMode {Target_Auto 0, Target_30 1, Target_60 2, Target_90 3, Target_120 4}): the swapchain waits
     * so frames are presented at that steady rate. A game that can't hold 60 but runs above 30 (Ocarina of Time: ~40) presents
     * unevenly on a 60/120 Hz screen (each frame 3 or 4 refreshes): micro-stutter. The target counts PRESENTED frames, generated ones
     * included, so "smooth60" (steady 30 real frames + FSR3 frame generation 2x) is Target_60 + frame_gen 2x; Target_30 with frame
     * generation halves the game to 15 fps (measured 2026-11-06, Ocarina of Time: 15 fps with long stalls vs a steady 30 + 30 at
     * Target_60). It overrides switch.framegen. "lichii" leaves Lichii's own setting alone. */
    const std::string pc = get("switch.pacing", "lichii");
    if (pc == "smooth60") fg = "2";
    std::string t = in;
    put(t, "Renderer", "resolution_setup", res);
    if (pc == "auto" || pc == "30" || pc == "60" || pc == "smooth60")
        put(t, "Renderer", "frame_pacing_mode", pc == "auto" ? "0" : pc == "30" ? "1" : "2");
    if (all_digits(fg) && fg != "0" && fg != "1") {
        put(t, "Renderer", "frame_gen", "true");
        put(t, "Renderer", "frame_gen_type", "1");
        put(t, "Renderer", "frame_gen_multiplier", fg);
    } else {
        put(t, "Renderer", "frame_gen", "false");
    }
    put(t, "Renderer", "use_vsync", vs == "mailbox" ? "1" : vs == "off" ? "0" : "2");
    const std::string as = get("switch.async_shaders", "on");
    if (as == "on" || as == "off") put(t, "Renderer", "use_asynchronous_shaders", as == "on" ? "true" : "false");   // "lichii": untouched
    const std::string ga = get("switch.gpu_accuracy", "low");   // Lichii GpuAccuracy {Low 0, High 1}
    if (ga == "low" || ga == "high") put(t, "Renderer", "gpu_accuracy", ga == "low" ? "0" : "1");   // "lichii": untouched
    put(t, "UI", "fullscreen", fullscreen ? "true" : "false");
    put(t, "System", "current_user", "0");
    t = ini_put(t, "UI", "check_for_updates", "false");   // last (after fullscreen, so [UI] keeps its order), on every launch and never skipped: Lichii's own update check must never run (RetroHub is the only updater), even if the player switched it back on
    return t;
}

std::string fritchii_ini_pin(const std::string& in) {
    std::string t = in;
    t = ini_put(t, "Renderer", "frizti_settings_revision", std::to_string(kFriztiPinRevision));
    t = ini_put(t, "Renderer", "oot_fps_asked", "true");
    // Fritchii's recipe is recorded with EDS2 + dynamic vertex input: other values make "Preparing shaders" skip almost every pipeline. Its own
    // one-time repair of them (tuned-settings revision 5, v5.5) is held off by the revision pin above, so RetroHub pins the two keys itself.
    t = ini_put(t, "Renderer", "dyna_state", "2");
    t = ini_put(t, "Renderer", "vertex_input_dynamic_state", "true");
    t = ini_put(t, "UI", "fritchii_whats_new_seen", kFritchiiWhatsNewSeen);   // no What's new window after the game
    return t;
}

/* ------------------------------------------------------------------ controls */

static std::string num(double d) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", d);
    return b;
}

static bool hex32(const std::string& g) {
    if (g.size() != 32) return false;
    for (char ch : g) if (!std::isxdigit((unsigned char)ch)) return false;
    return true;
}

std::string lichii_zero_guid_crc(const std::string& guid) {
    if (!hex32(guid)) return guid;
    std::string g = guid;
    for (char& ch : g) ch = (char)std::tolower((unsigned char)ch);
    g.replace(4, 4, "0000");   // bytes 2-3: SDL's name crc, which Lichii's GetGUID clears
    return g;
}

std::string lichii_xinput_guid(unsigned vendor, unsigned product, unsigned version, unsigned subtype) {
    char b[48];
    std::snprintf(b, sizeof b, "03000000%02x%02x0000%02x%02x0000%02x%02x%02x%02x", vendor & 0xff, (vendor >> 8) & 0xff, product & 0xff, (product >> 8) & 0xff,
                  version & 0xff, (version >> 8) & 0xff, 0x78u, subtype & 0xff);
    return b;
}

bool lichii_guid_is_xinput(const std::string& guid) { return hex32(guid) && guid.compare(28, 2, "78") == 0; }

static unsigned le16(const std::string& g, size_t at) { return (unsigned)(std::stoul(g.substr(at, 2), nullptr, 16) | (std::stoul(g.substr(at + 2, 2), nullptr, 16) << 8)); }

std::string lichii_guid_to_xinput(const std::string& guid) {
    if (!hex32(guid) || lichii_guid_is_xinput(guid)) return "";
    const std::string g = lichii_zero_guid_crc(guid);
    if (le16(g, 8) != 0x045e) return "";   // Microsoft pads only
    return lichii_xinput_guid(0x045e, le16(g, 16), le16(g, 24), 1);
}

SdlBinds lichii_xinput_binds() {
    SdlBinds s;
    auto b = [&](int i, int idx) { s.button[i].type = 'b'; s.button[i].idx = idx; };
    auto h = [&](int i, int mask) { s.button[i].type = 'h'; s.button[i].idx = 0; s.button[i].hat = mask; };
    auto a = [&](int i, int idx) { s.axis[i].type = 'a'; s.axis[i].idx = idx; };
    b(0, 0); b(1, 1); b(2, 2); b(3, 3);          // A B X Y
    b(4, 6); b(5, 10); b(6, 7);                  // Back, Guide, Start
    b(7, 8); b(8, 9); b(9, 4); b(10, 5);         // L3 R3 LB RB
    h(11, 1); h(12, 4); h(13, 8); h(14, 2);      // d-pad up down left right: hat 0
    a(0, 0); a(1, 1); a(2, 3); a(3, 4); a(4, 2); a(5, 5);   // LX LY RX RY LT RT
    s.valid = true;
    return s;
}

bool lichii_guid_is_hidapi(const std::string& guid) { return hex32(guid) && std::tolower((unsigned char)guid[28]) == '6' && std::tolower((unsigned char)guid[29]) == '8'; }

SdlBinds lichii_sdl3_binds(const std::string& guid, const SdlBinds& in) {
    if (!in.valid || !lichii_guid_is_hidapi(guid)) return in;
    SdlBinds s = in;
    auto h = [&](int i, int mask) { s.button[i].type = 'h'; s.button[i].idx = 0; s.button[i].hat = mask; };
    h(11, 1); h(12, 4); h(13, 8); h(14, 2);   // d-pad up down left right: hat 0 (SDL_HAT_UP / DOWN / LEFT / RIGHT)
    for (int i = 15; i < 21; ++i) s.button[i] = SdlBinds::B();   // misc1, paddles, touchpad: not at SDL2's indices in SDL3
    for (int i = 0; i < 6; ++i) { s.axis[i].type = 'a'; s.axis[i].idx = i; }   // LX LY RX RY LT RT
    return s;
}

bool lichii_ini_player0(const std::string& ini, std::string* guid, int* port) {
    const std::string key = "player_0_button_a=\"";
    size_t p = ini.find(key);
    if (p == std::string::npos) return false;
    p += key.size();
    size_t e = ini.find_first_of("\"\r\n", p);
    const std::string v = ini.substr(p, e == std::string::npos ? std::string::npos : e - p);   // engine:sdl,port:0,guid:...,button:1
    size_t g = v.find("guid:");
    if (g == std::string::npos) return false;
    size_t ge = v.find(',', g);
    *guid = v.substr(g + 5, ge == std::string::npos ? std::string::npos : ge - g - 5);
    *port = 0;
    size_t q = v.find("port:");
    if (q != std::string::npos) *port = std::atoi(v.c_str() + q + 5);
    return true;
}

XinputId lichii_xinput_id(int slot) {
    XinputId r;
    if (slot < 0 || slot > 3) return r;
    struct CapsEx { BYTE type, sub; WORD flags; BYTE gamepad[12]; BYTE vibration[4]; WORD vendor, product, version, unk1; DWORD unk2; };
    typedef DWORD(WINAPI * GetCaps)(DWORD, DWORD, void*);
    typedef DWORD(WINAPI * GetCapsEx)(DWORD, DWORD, DWORD, CapsEx*);
    static HMODULE dll = LoadLibraryW(L"xinput1_4.dll");   // the one SDL3 loads first
    if (!dll) return r;
    GetCaps caps = (GetCaps)(void*)GetProcAddress(dll, "XInputGetCapabilities");
    GetCapsEx ex = (GetCapsEx)(void*)GetProcAddress(dll, (LPCSTR)(ULONG_PTR)108);   // _XInputGetCapabilitiesEx: also returns vid / pid / version
    if (!caps) return r;
    BYTE buf[32] = {};
    if (caps((DWORD)slot, 1 /*XINPUT_FLAG_GAMEPAD*/, buf) != ERROR_SUCCESS) return r;
    const unsigned sub = buf[1];
    if (sub == 0) return r;   // XINPUT_DEVSUBTYPE_UNKNOWN: SDL ignores it
    r.present = true;
    r.subtype = sub;
    r.vendor = 0x045e; r.product = 0x028e; r.version = 0;   // SDL3's fallback when CapsEx is missing
    CapsEx e = {};
    if (ex && ex(1, (DWORD)slot, 0, &e) == ERROR_SUCCESS) {
        r.vendor = e.vendor; r.product = e.product; r.version = e.version;
        if (r.product == 0 && (e.flags & 0x0002 /*XINPUT_CAPS_WIRELESS*/)) { r.vendor = 0x045e; r.product = 0x028e; }   // wireless 360 pad
    }
    return r;
}

bool lichii_xinput_pad(const XinputId ids[4], int slot, std::string* guid, int* port) {
    if (slot < 0 || slot > 3 || !ids[slot].present) return false;
    *guid = ids[slot].guid();
    *port = 0;
    for (int s = 0; s < slot; ++s) if (ids[s].present && ids[s].guid() == *guid) ++*port;
    return true;
}

static std::string sdl_prefix(const std::string& guid, int port) {
    return "engine:sdl,port:" + std::to_string(port < 0 ? 0 : port) + ",guid:" + (guid.empty() ? std::string("0") : lichii_zero_guid_crc(guid));
}

static std::string bind_value(const SdlBinds::B& b, const std::string& p) {
    if (b.type == 'b' && b.idx >= 0) return p + ",button:" + std::to_string(b.idx);
    if (b.type == 'h' && b.idx >= 0) {
        const char* d = (b.hat & 1) ? "up" : (b.hat & 2) ? "right" : (b.hat & 4) ? "down" : (b.hat & 8) ? "left" : nullptr;
        if (d) return p + ",hat:" + std::to_string(b.idx) + ",direction:" + d;
    }
    if (b.type == 'a' && b.idx >= 0) return p + ",axis:" + std::to_string(b.idx) + ",threshold:0.5,invert:+";
    return "[empty]";
}

static std::string term_value(const hubctl::Term& t, const std::string& guid, int port, const SdlBinds* binds) {
    std::string p = sdl_prefix(guid, port);
    if (binds && binds->valid) {   // the pad's raw joystick numbers (what Lichii's SDL engine reads)
        if (!t.axis) return t.idx >= 0 && t.idx < 21 ? bind_value(binds->button[t.idx], p) : std::string("[empty]");
        if (t.idx < 0 || t.idx >= 6) return "[empty]";
        const SdlBinds::B& b = binds->axis[t.idx];
        if (b.type != 'a') return bind_value(b, p);   // e.g. a pad whose triggers are plain buttons
        const bool trig = t.idx >= 4;
        double th = trig ? 0.5 : (t.threshold > 0 ? t.threshold / 32767.0 : 0.5);
        if (th > 0.95) th = 0.95;
        return p + ",axis:" + std::to_string(b.idx) + ",threshold:" + num(std::floor(th * 100 + 0.5) / 100) + ",invert:" + (!trig && t.sign < 0 ? "-" : "+");
    }
    if (!t.axis) return t.idx < 0 || t.idx >= hubctl::kNumButtons ? std::string("[empty]") : p + ",button:" + std::to_string(t.idx);
    bool trig = t.idx >= 4;
    double th = trig ? 0.5 : (t.threshold > 0 ? t.threshold / 32767.0 : 0.5);
    if (th > 0.95) th = 0.95;
    return p + ",axis:" + std::to_string(t.idx) + ",threshold:" + num(std::floor(th * 100 + 0.5) / 100) + ",invert:" + (!trig && t.sign < 0 ? "-" : "+");
}

static std::string button_value(const hubctl::Profile& prof, const std::string& control, const std::string& guid, int port, const SdlBinds* binds) {
    auto it = prof.buttons.find(control);
    std::vector<hubctl::Term> terms;
    if (it != prof.buttons.end() && hubctl::parse_expr(it->second, prof.axis_threshold, &terms) && !terms.empty())
        return term_value(terms[0], guid, port, binds);
    return "[empty]";
}

static std::string stick_value(const hubctl::Profile& prof, const std::string& control, int dx, int dy, const std::string& guid, int port, const SdlBinds* binds) {
    int ax = dx, ay = dy;
    double dz = 0.15, range = 1.0;
    auto it = prof.analogs.find(control);
    if (it != prof.analogs.end()) {
        if (hubctl::axis_index(it->second.x) >= 0) ax = hubctl::axis_index(it->second.x);
        if (hubctl::axis_index(it->second.y) >= 0) ay = hubctl::axis_index(it->second.y);
        dz = std::min(0.95, std::max(0.0, it->second.deadzone));
        range = std::min(1.5, std::max(0.25, it->second.sensitivity));
    }
    if (binds && binds->valid) {   // GameController axis -> the pad's raw joystick axis
        if (ax >= 0 && ax < 6 && binds->axis[ax].type == 'a') ax = binds->axis[ax].idx;
        if (ay >= 0 && ay < 6 && binds->axis[ay].type == 'a') ay = binds->axis[ay].idx;
    }
    return sdl_prefix(guid, port) + ",axis_x:" + std::to_string(ax) + ",axis_y:" + std::to_string(ay) +
           ",offset_x:0,offset_y:0,invert_x:+,invert_y:+,deadzone:" + num(dz) + ",range:" + num(range);
}

std::string lichii_controls_ini(const std::string& in, const hubctl::Profile& prof, const std::string& guid, int port, const SdlBinds* binds) {
    struct M { const char* control; const char* key; };
    static const M map[] = {
        {"a", "button_a"}, {"b", "button_b"}, {"x", "button_x"}, {"y", "button_y"}, {"l3", "button_lstick"}, {"r3", "button_rstick"},
        {"l", "button_l"}, {"r", "button_r"}, {"zl", "button_zl"}, {"zr", "button_zr"}, {"plus", "button_plus"}, {"minus", "button_minus"},
        {"dpleft", "button_dleft"}, {"dpup", "button_dup"}, {"dpright", "button_dright"}, {"dpdown", "button_ddown"}, {"home", "button_home"},
    };
    std::string t = in;
    auto put = [&](const std::string& key, const std::string& value, bool quote) { t = ini_put(t, "Controls", key, quote ? "\"" + value + "\"" : value); };
    for (const M& m : map) put(std::string("player_0_") + m.key, button_value(prof, m.control, guid, port, binds), true);
    put("player_0_lstick", stick_value(prof, "left_stick", 0, 1, guid, port, binds), true);
    put("player_0_rstick", stick_value(prof, "right_stick", 2, 3, guid, port, binds), true);
    put("player_0_connected", "true", false);
    put("player_0_type", "0", false);
    for (int i = 1; i < 8; ++i) put("player_" + std::to_string(i) + "_connected", "false", false);
    return t;
}

/* ------------------------------------------------------------------ prepare + args */

std::string prepare_lichii(const std::string& install, const hubctl::Profile& prof, const std::string& guid, int port, const hub::gs::Values& v,
                           bool fullscreen, const SdlBinds* binds, const std::vector<std::string>* skip_keys) {
    std::error_code ec;
    fs::create_directories(P(lichii_user_dir(install)), ec);
    if (!is_dir(lichii_user_dir(install))) return "could not create Lichii's user folder";
    const std::string ini = join(config_dir(install), "qt-config.ini"), marker = join(config_dir(install), "retrohub-defaults.txt");
    std::string t = read_text(ini);
    const bool first = read_text(marker).empty();
    if (first) t = lichii_ini_defaults(t);
    t = lichii_ini_launch(t, v, fullscreen, skip_keys);
    std::string g = guid;
    int pt = port;
    const SdlBinds* b = binds;
    SdlBinds xb;
    if (g.empty()) {   // no pad now: keep whatever Lichii has, but turn a Microsoft pad's old RAWINPUT / WGI guid into the XInput one Lichii sees once the pad is switched on
        std::string og; int op = 0;
        const std::string x = lichii_ini_player0(t, &og, &op) ? lichii_guid_to_xinput(og) : std::string();
        if (!x.empty()) { g = x; pt = op; xb = lichii_xinput_binds(); b = &xb; }
    }
    if (!g.empty()) {
        t = lichii_controls_ini(t, prof, g, pt, b);
        if (lichii_guid_is_xinput(g)) t = ini_put(t, "Controls", "enable_raw_input", "false");   // an XInput guid only exists while Lichii's RAWINPUT is off
    }
    if (!write_bytes(ini, t)) return "could not write qt-config.ini";
    if (first) write_bytes(marker, "RetroHub wrote Lichii's defaults once (delete this file to write them again); video settings and controls are rewritten before every launch.\n");
    lichii_ensure_profile(install);
    return "";
}

bool lichii_pad_select(const std::vector<PadDesc>& by_port, const std::vector<PadDesc>& by_sdl, std::string* guid, int* port) {
    AzPad p;
    bool ok = azahar_pad_select(by_port, by_sdl, &p);
    *guid = p.guid;
    *port = p.port;
    return ok;
}

std::string lichii_args(const std::string& game, bool fullscreen) {
    return std::string(fullscreen ? "-f " : "") + "-u 0 -g " + winutil::quote_arg(game);
}

bool lichii_fast_reads(const hub::gs::Values& v) {
    auto it = v.find("switch.fast_reads");
    return it == v.end() || it->second != "off";   // missing / unknown counts as on
}

std::vector<std::pair<std::string, std::string>> lichii_env(const hub::gs::Values& v, uint64_t ram) {
    std::vector<std::pair<std::string, std::string>> env;
    if (lichii_fast_reads(v)) env.push_back({"EDEN_NX2_NO_REACTIVE", "1"});
    auto it = v.find("switch.preload_shaders");
    const std::string m = it == v.end() ? "recent" : it->second;   // the default since 1.40.4; an unknown value preloads nothing either
    const uint64_t kGiB = 1024ull * 1024 * 1024;
    // "auto" preloads everything only with 60 GiB+ of RAM (a "64 GB" PC). It used to be 30 GiB, but a cache of 43k pipelines needs ~19 GB of
    // driver memory, and after a Lichii update every pipeline is compiled again first: a 32 GB PC sat 27 minutes before the first frame on build 125.
    const bool all = m == "all" || (m == "auto" && ram >= 60 * kGiB);
    if (all) env.push_back({"EDEN_NX2_PRELOAD_ALL_PIPELINES", "1"});
    return env;
}

uint64_t physical_ram_bytes() {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    return GlobalMemoryStatusEx(&ms) ? (uint64_t)ms.ullTotalPhys : 0;
}

}  // namespace hubemu
