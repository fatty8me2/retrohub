#include "games.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

namespace hub {
namespace {

namespace fs = std::filesystem;

constexpr size_t kMinRomSize = 1024u * 1024u;       // 1 MB
constexpr size_t kMaxRomSize = 64u * 1024u * 1024u; // 64 MB

/* ---- UTF-8 <-> UTF-16 helpers (paths may contain non-ASCII) ---- */

std::wstring to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

fs::path utf8_path(const std::string& s) { return fs::path(to_wide(s)); }
std::string path_utf8(const fs::path& p) { return to_utf8(p.native()); }

std::string ascii_lower(std::string s) {
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    }
    return s;
}

/* ---- MD5 via Windows BCrypt ---- */

std::string md5_bcrypt(const uint8_t* data, size_t len) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0)))
        return std::string();

    std::string result;
    BCRYPT_HASH_HANDLE h = nullptr;
    do {
        DWORD obj_len = 0, hash_len = 0, got = 0;
        if (!BCRYPT_SUCCESS(BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&obj_len), sizeof(obj_len), &got, 0))) break;
        if (!BCRYPT_SUCCESS(BCryptGetProperty(alg, BCRYPT_HASH_LENGTH,
                reinterpret_cast<PUCHAR>(&hash_len), sizeof(hash_len), &got, 0))) break;

        std::vector<UCHAR> obj(obj_len), hash(hash_len);
        if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, obj.data(), obj_len, nullptr, 0, 0))) break;

        size_t off = 0;
        bool ok = true;
        while (off < len) {
            ULONG chunk = (ULONG)std::min<size_t>(len - off, size_t(1) << 30);
            if (!BCRYPT_SUCCESS(BCryptHashData(h, const_cast<PUCHAR>(data + off), chunk, 0))) {
                ok = false;
                break;
            }
            off += chunk;
        }
        if (!ok) break;
        if (!BCRYPT_SUCCESS(BCryptFinishHash(h, hash.data(), hash_len, 0))) break;

        static const char kHex[] = "0123456789ABCDEF";
        for (UCHAR b : hash) {
            result.push_back(kHex[b >> 4]);
            result.push_back(kHex[b & 0x0F]);
        }
    } while (false);

    if (h) BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    return result;
}

/* ---- ROM byte order ---- */

uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

/* Converts the image in place to z64 (big-endian) order. */
bool normalize_to_z64(std::vector<uint8_t>& img) {
    if (img.size() < 4) return false;
    switch (be32(img.data())) {
    case 0x80371240u:  // z64: already big-endian
        return true;
    case 0x37804012u:  // v64: byteswapped, swap each pair of bytes
        for (size_t i = 0; i + 1 < img.size(); i += 2) std::swap(img[i], img[i + 1]);
        return true;
    case 0x40123780u:  // n64: little-endian, reverse each 4-byte word
        for (size_t i = 0; i + 3 < img.size(); i += 4) {
            std::swap(img[i], img[i + 3]);
            std::swap(img[i + 1], img[i + 2]);
        }
        return true;
    default:
        return false;
    }
}

std::string header_name_of(const std::vector<uint8_t>& img) {
    if (img.size() < 0x34) return std::string();
    std::string s(reinterpret_cast<const char*>(img.data() + 0x20), 0x14);
    for (auto& c : s) {
        if (c == '\0') c = ' ';
    }
    size_t b = 0, e = s.size();
    while (b < e && s[b] == ' ') ++b;
    while (e > b && s[e - 1] == ' ') --e;
    return s.substr(b, e - b);
}

bool has_rom_extension(const fs::path& p) {
    std::string ext = ascii_lower(path_utf8(p.extension()));
    return ext == ".z64" || ext == ".v64" || ext == ".n64";
}

} // namespace

/* ---- catalog ---- */

const std::vector<GameInfo>& catalog() {
    static const std::vector<GameInfo> games = {
        {"smash64", "Super Smash Bros. (Smash Classic)", Engine::Native, 4, {},
         "games/smashClassic/BattleShip.exe"},
        {"mp1", "Mario Party", Engine::Mupen64Plus, 4,
         {"8BC2712139FBF0C56C8EA835802C52DC"}, ""},
        {"mp2", "Mario Party 2", Engine::Mupen64Plus, 4,
         {"04840612A35ECE222AFDB2DFBF926409"}, ""},
        {"mp3", "Mario Party 3", Engine::Mupen64Plus, 4,
         {"76A8BBC81BC2060EC99C9645867237CC"}, ""},
        /* Smash Remix (fan ROM hack, 50+ fighters): no native port, so it runs
         * through the emulator path. MD5 of the 2.0.0 release .z64. */
        {"remix", "Smash Remix 2.0", Engine::Mupen64Plus, 4,
         {"43571020FA5DDB138BBD7F64A8C9C0D4"}, ""},
    };
    return games;
}

const GameInfo* find_game(const std::string& id) {
    for (const auto& g : catalog()) {
        if (g.id == id) return &g;
    }
    return nullptr;
}

/* ---- ROMs ---- */

bool load_rom(const std::string& path, std::vector<uint8_t>* z64_image) {
    if (!z64_image) return false;
    fs::path p = utf8_path(path);

    std::error_code ec;
    uintmax_t size = fs::file_size(p, ec);
    if (ec || size < kMinRomSize || size > kMaxRomSize) return false;

    std::ifstream f(p, std::ios::binary);
    if (!f) return false;

    std::vector<uint8_t> img((size_t)size);
    f.read(reinterpret_cast<char*>(img.data()), (std::streamsize)img.size());
    if (f.gcount() != (std::streamsize)img.size()) return false;

    if (!normalize_to_z64(img)) return false;
    *z64_image = std::move(img);
    return true;
}

std::string md5_hex(const uint8_t* data, size_t len) {
    return md5_bcrypt(data, len);
}

bool identify_rom(const std::string& path, RomFile* out) {
    if (!out) return false;
    std::vector<uint8_t> img;
    if (!load_rom(path, &img)) return false;

    RomFile r;
    r.path = path;
    r.md5 = md5_hex(img.data(), img.size());
    r.header_name = header_name_of(img);
    for (const auto& g : catalog()) {
        for (const auto& m : g.rom_md5s) {
            if (m == r.md5) {
                r.game_id = g.id;
                break;
            }
        }
        if (!r.game_id.empty()) break;
    }
    *out = std::move(r);
    return true;
}

std::vector<RomFile> scan_roms(const std::string& folder) {
    std::vector<RomFile> out;
    std::error_code ec;
    fs::recursive_directory_iterator it(utf8_path(folder),
        fs::directory_options::skip_permission_denied, ec);
    if (ec) return out;
    const fs::recursive_directory_iterator end;

    while (it != end) {
        std::error_code fec;
        const fs::path p = it->path();
        if (has_rom_extension(p) && it->is_regular_file(fec) && !fec) {
            RomFile r;
            if (identify_rom(path_utf8(p), &r)) out.push_back(std::move(r));
        }
        it.increment(ec);
        if (ec) break;  // directory iteration became unreliable; keep what we have
    }
    return out;
}

/* ---- paths ---- */

std::string exe_dir() {
    static const std::string cached = [] {
        std::wstring buf(MAX_PATH, L'\0');
        for (;;) {
            DWORD n = GetModuleFileNameW(nullptr, &buf[0], (DWORD)buf.size());
            if (n == 0) return std::string();
            if (n < buf.size()) {
                buf.resize(n);
                break;
            }
            buf.resize(buf.size() * 2);
        }
        size_t pos = buf.find_last_of(L"\\/");
        if (pos == std::wstring::npos) return std::string();
        buf.resize(pos + 1);
        std::string s = to_utf8(buf);
        if (!s.empty() && s.back() != '\\') s.push_back('\\');
        return s;
    }();
    return cached;
}

std::string data_dir() {
    std::string dir = exe_dir() + "userdata\\";
    std::error_code ec;
    fs::create_directories(utf8_path(dir), ec);
    return dir;
}

} // namespace hub
