/* The game identity for the join fingerprint (joinfp.h): the same game gives the same id on every PC, a different dump or a hack gives another one.
 *
 *  Cartridges (NES, SNES, N64, GB, GBC, GBA, NDS): the No-Intro style CRC + size of the ROM data the core gets (hub::verify::hash_stream): an iNES or SNES copier
 *     header is skipped and an N64 .v64/.n64 is read as .z64. A .zip is opened and its biggest entry hashed, so a zipped and an unpacked copy match.
 *  Discs (PS1, PS2 and anything else): the boot serial (SYSTEM.CNF, else the hub's --setup-tag) + the data track's identity. A .cue / .m3u is followed to its first data
 *     file. A .chd is identified by the SHA-1s in its header (exact, nothing sampled). Any other image is identified by its size + 64 blocks of 64 KiB spread over it
 *     (about 4 MiB read, never the whole multi-GB file): a patch that only touches bytes between the samples and keeps the size and serial is NOT noticed.
 * 0 = the identity could not be worked out (unreadable file, an unsupported index such as .gdi); joinfp::check treats 0 as equal only to 0. */
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "disc_id.h"
#include "joinfp.h"
#include "library.h"
#include "miniz.h"
#include "verify.h"

namespace joinid {

namespace fs = std::filesystem;

constexpr uint64_t kWholeFileMax = 64ull << 20;   // cartridge images are read whole up to here

inline std::string lower_ext(const std::string& path) {
    size_t dot = path.find_last_of('.');
    std::string e;
    if (dot != std::string::npos) for (size_t i = dot + 1; i < path.size(); i++) e += (char)((path[i] >= 'A' && path[i] <= 'Z') ? path[i] + 32 : path[i]);
    return e;
}

inline FILE* open_rb(const std::string& path) { return _wfopen(fs::u8path(path).wstring().c_str(), L"rb"); }

inline bool read_all(const std::string& path, uint64_t cap, std::vector<uint8_t>* out) {
    FILE* f = open_rb(path);
    if (!f) return false;
    _fseeki64(f, 0, SEEK_END);
    long long n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    bool ok = n > 0 && (uint64_t)n <= cap;
    if (ok) { out->resize((size_t)n); ok = fread(out->data(), 1, (size_t)n, f) == (size_t)n; }
    fclose(f);
    return ok;
}

inline uint32_t from_crc_size(uint32_t crc, uint64_t size) {
    uint64_t h = joinfp::fnv(0xCBF29CE484222325ull, &crc, sizeof crc);
    h = joinfp::fnv(h, &size, sizeof size);
    return joinfp::fold(h) | 1u;   // never 0
}

/* Cartridge data in memory (an unpacked ROM or a zip's entry). */
inline uint32_t cart_from_bytes(hub::System s, const uint8_t* d, size_t n) {
    if (n == 0) return 0;
    hub::verify::FileHash fh;
    if (hub::verify::supported(s)) {
        size_t pos = 0;
        if (!hub::verify::hash_stream(s, n, [&](uint8_t* b, size_t want) { size_t k = std::min(want, n - pos); std::copy(d + pos, d + pos + k, b); pos += k; return k; }, &fh)) return 0;
        return from_crc_size(fh.crc, fh.size);
    }
    return from_crc_size(hub::verify::crc32_update(0, d, n), n);   // another cartridge system: the plain CRC
}

/* The bytes of a game file: the file itself, or the biggest entry of a zip. */
inline uint32_t cart_from_file(hub::System s, const std::string& path) {
    std::vector<uint8_t> raw;
    if (!read_all(path, kWholeFileMax, &raw)) return 0;
    if (lower_ext(path) == "zip") {
        mz_zip_archive z;
        memset(&z, 0, sizeof z);
        if (!mz_zip_reader_init_mem(&z, raw.data(), raw.size(), 0)) return 0;
        int best = -1; mz_uint64 best_sz = 0;
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&z); i++) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&z, i, &st) || st.m_is_directory) continue;
            if (best < 0 || st.m_uncomp_size > best_sz) { best = (int)i; best_sz = st.m_uncomp_size; }
        }
        uint32_t id = 0;
        if (best >= 0 && best_sz <= kWholeFileMax) {
            size_t sz = 0;
            void* p = mz_zip_reader_extract_to_heap(&z, (mz_uint)best, &sz, 0);
            if (p) { id = cart_from_bytes(s, (const uint8_t*)p, sz); mz_free(p); }
        }
        mz_zip_reader_end(&z);
        return id;
    }
    return cart_from_bytes(s, raw.data(), raw.size());
}

/* A CHD v5 header holds the SHA-1 of the raw data and of the whole content (offsets 64..103): the exact identity of the disc, however it was compressed. */
inline bool chd_digest(const uint8_t* h, size_t n, uint64_t* out) {
    if (n < 124 || memcmp(h, "MComprHD", 8) != 0) return false;
    const uint32_t ver = ((uint32_t)h[12] << 24) | ((uint32_t)h[13] << 16) | ((uint32_t)h[14] << 8) | h[15];
    if (ver != 5) return false;
    bool any = false;
    for (int i = 64; i < 104; i++) if (h[i]) any = true;
    if (!any) return false;
    *out = joinfp::fnv(0xCBF29CE484222325ull, h + 64, 40);
    return true;
}

/* size + 64 blocks of 64 KiB evenly spread (the first and the last included). Not exhaustive: see the note at the top. */
inline uint64_t sampled_hash(const std::string& path) {
    FILE* f = open_rb(path);
    if (!f) return 0;
    _fseeki64(f, 0, SEEK_END);
    long long size = _ftelli64(f);
    if (size <= 0) { fclose(f); return 0; }
    uint64_t h = joinfp::fnv(0xCBF29CE484222325ull, &size, sizeof size);
    std::vector<uint8_t> buf(65536);
    for (int i = 0; i < 64; i++) {
        long long off = size > (long long)buf.size() ? (size - (long long)buf.size()) / 63 * i : 0;
        _fseeki64(f, off, SEEK_SET);
        size_t got = fread(buf.data(), 1, buf.size(), f);
        h = joinfp::fnv(h, buf.data(), got);
        if (size <= (long long)buf.size()) break;
    }
    fclose(f);
    return h;
}

/* The data file behind a disc index (.cue first FILE; .m3u first entry, followed once more when that is a .cue); the path itself otherwise. */
inline std::string data_file(const std::string& path, int depth = 0) {
    const std::string e = lower_ext(path);
    if (e == "gdi" || e == "ccd" || e == "toc") return "";   // text sheets that differ between PCs without the game differing
    if (e != "cue" && e != "m3u") return path;
    std::vector<uint8_t> t;
    if (!read_all(path, 1 << 20, &t)) return "";
    std::string text(t.begin(), t.end()), name;
    if (e == "cue") name = hub::disc::cue_first_file(text);
    else {
        size_t pos = 0;
        while (pos < text.size() && name.empty()) {
            size_t end = text.find_first_of("\r\n", pos);
            std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? text.size() : end + 1;
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
            if (!line.empty() && line[0] != '#') name = line;
        }
    }
    if (name.empty()) return "";
    fs::path p = fs::u8path(name);
    if (p.is_relative()) p = fs::u8path(path).parent_path() / p;
    std::string r = p.u8string();
    return (e == "m3u" && depth == 0) ? data_file(r, 1) : r;
}

/* Disc: serial + data identity. `serial_hint` = the hub's --setup-tag (used when the disc has no readable SYSTEM.CNF). */
inline uint32_t disc_id(const std::string& path, const std::string& serial_hint) {
    std::string serial;
    hub::disc::Info info;
    if (hub::disc::probe_file(path, &info)) serial = info.serial;
    if (serial.empty()) serial = serial_hint;
    const std::string data = data_file(path);
    uint64_t ident = 0;
    if (!data.empty()) {
        FILE* f = open_rb(data);
        if (f) {
            uint8_t hdr[124];
            size_t got = fread(hdr, 1, sizeof hdr, f);
            fclose(f);
            if (!(lower_ext(data) == "chd" && chd_digest(hdr, got, &ident))) ident = sampled_hash(data);
        }
    }
    if (serial.empty() && !ident) return 0;
    uint64_t h = joinfp::fnv(0xCBF29CE484222325ull, serial.data(), serial.size());
    h = joinfp::fnv(h ^ 0xFF, &ident, sizeof ident);
    return joinfp::fold(h) | 1u;
}

/* `system` = retrorun's --system text ("gba", "ps1" ...). */
inline uint32_t of_game(const std::string& system, const std::string& path, const std::string& serial_hint) {
    if (path.empty()) return 0;
    const hub::System s = hub::system_from_short(system);
    if (s == hub::System::PS1 || s == hub::System::PS2) return disc_id(path, serial_hint);
    const std::string e = lower_ext(path);
    if (e == "cue" || e == "m3u" || e == "chd" || e == "iso" || e == "gdi" || e == "ccd" || e == "toc" || e == "cso") return disc_id(path, serial_hint);
    return cart_from_file(s, path);
}

}  // namespace joinid
