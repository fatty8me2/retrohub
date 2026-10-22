#include "emu_formats.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace hub {

namespace {

uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
uint64_t le64(const uint8_t* p) { return uint64_t(le32(p)) | (uint64_t(le32(p + 4)) << 32); }
uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]); }
uint64_t be64(const uint8_t* p) { return (uint64_t(be32(p)) << 32) | uint64_t(be32(p + 4)); }

bool is_alnum(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

std::string upper_hex8(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%08X", v);
    return b;
}

/* NCCH header at p (0x200 bytes starting at the signature). */
bool parse_ncch(const uint8_t* p, N3dsInfo* o) {
    if (std::memcmp(p + 0x100, "NCCH", 4) != 0) return false;
    o->title_id = le64(p + 0x118);   // program id
    if (o->title_id == 0) o->title_id = le64(p + 0x108);   // partition id
    char code[17] = {0};
    std::memcpy(code, p + 0x150, 16);
    o->product_code = code;
    o->serial.clear();
    if (o->product_code.size() >= 10 && o->product_code.compare(0, 4, "CTR-") == 0) {
        std::string s = o->product_code.substr(6, 4);
        bool ok = s.size() == 4;
        for (char c : s) if (!is_alnum(c)) ok = false;
        if (ok) o->serial = s;
    }
    o->decrypted = (p[0x188 + 7] & 0x04) != 0;   // NCCH flags[7] bit 2: NoCrypto
    o->is_application = (o->title_id >> 32) == 0x00040000u;
    return true;
}

} // namespace

std::string hex16(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
    return b;
}

bool parse_3ds_header(const uint8_t* d, size_t n, N3dsInfo* out) {
    if (!d || !out) return false;
    N3dsInfo o;
    if (n >= 0x200 && std::memcmp(d + 0x100, "NCSD", 4) == 0) {
        /* partition table at 0x120: 8 x {offset, length} in 0x200-byte media units; partition 0 = executable */
        uint32_t off = le32(d + 0x120), len = le32(d + 0x124);
        uint64_t at = uint64_t(off) * 0x200;
        if (len == 0 || at + 0x200 > n) return false;
        if (!parse_ncch(d + at, &o)) return false;
        o.container = "ncsd";
        *out = o;
        return true;
    }
    if (n >= 0x200 && std::memcmp(d + 0x100, "NCCH", 4) == 0) {
        if (!parse_ncch(d, &o)) return false;
        o.container = "ncch";
        *out = o;
        return true;
    }
    if (n >= 4 && std::memcmp(d, "3DSX", 4) == 0) {
        o.container = "3dsx";
        o.decrypted = true;
        *out = o;
        return true;
    }
    /* CIA: u32 header size (0x2020), u16 type, u16 version, u32 cert / ticket / tmd sizes, each section 0x40-aligned. */
    if (n >= 0x20 && le32(d) == 0x2020) {
        uint32_t cert = le32(d + 0x08), tik = le32(d + 0x0C), tmd = le32(d + 0x10);
        auto al = [](uint64_t v) { return (v + 0x3F) & ~uint64_t(0x3F); };
        uint64_t tmd_at = al(0x2020) + al(cert) + al(tik);
        if (cert > 0x10000 || tik > 0x10000 || tmd < 0x100 || tmd_at + 0x200 > n) return false;
        uint32_t sig = be32(d + tmd_at);
        uint64_t sig_len;
        switch (sig) {
        case 0x010000: case 0x010003: sig_len = 4 + 0x200 + 0x3C; break;   // RSA-4096
        case 0x010001: case 0x010004: sig_len = 4 + 0x100 + 0x3C; break;   // RSA-2048
        case 0x010002: case 0x010005: sig_len = 4 + 0x3C + 0x40; break;    // ECDSA
        default: return false;
        }
        uint64_t hdr = tmd_at + sig_len;   // TMD header: title id at +0x4C (big endian)
        if (hdr + 0x54 > n) return false;   // reads 8 bytes at hdr + 0x4C
        o.title_id = be64(d + hdr + 0x4C);
        o.container = "cia";
        o.serial = upper_hex8(uint32_t(o.title_id & 0xFFFFFFFFu));
        o.is_application = (o.title_id >> 32) == 0x00040000u;
        o.decrypted = false;   // content crypto is per-content (tmd flags); Azahar installs/handles CIAs itself
        *out = o;
        return true;
    }
    return false;
}

std::string n3ds_key(const N3dsInfo& i) {
    return i.title_id ? "3ds:" + hex16(i.title_id) : std::string();
}

bool is_3ds_extension(const std::string& e) {
    static const char* const x[] = {".3ds", ".cci", ".cia", ".cxi", ".3dsx", ".zcci", ".zcia", ".zcxi", ".z3dsx"};
    for (const char* s : x) if (e == s) return true;
    return false;
}
bool is_3ds_zip_entry(const std::string& e) {
    static const char* const x[] = {".3ds", ".cci", ".cia", ".cxi", ".3dsx"};
    for (const char* s : x) if (e == s) return true;
    return false;
}

/* ---------------- Wii U ---------------- */

bool parse_wua(const ReadAtFn& read, uint64_t file_size, WiiuInfo* out) {
    if (!read || !out || file_size < 144 + 16) return false;
    uint8_t f[144];
    if (!read(file_size - 144, f, sizeof f)) return false;
    /* footer: 6 x {u64 offset, u64 size} (data, offset records, names, file tree, meta dir, meta data),
     * sha256[32], u64 total size, u32 version, u32 magic -- all big endian */
    if (be32(f + 140) != 0x169f52d6u || be32(f + 136) != 0x61bf3a01u) return false;
    if (be64(f + 128) != file_size) return false;
    uint64_t names_off = be64(f + 32), names_sz = be64(f + 40);
    uint64_t tree_off = be64(f + 48), tree_sz = be64(f + 56);
    if (tree_sz < 16 || tree_sz > (1ull << 31) || tree_off > file_size || tree_sz > file_size - tree_off ||
        names_off > file_size || names_sz > file_size - names_off) return false;
    uint8_t node[16];
    if (!read(tree_off, node, 16)) return false;
    if (node[0] & 0x80) return false;   // root must be a directory
    uint32_t first = be32(node + 4), count = be32(node + 8);
    if (count == 0 || first >= tree_sz / 16 || count > tree_sz / 16 - first) return false;
    if (count > 4096) count = 4096;
    std::vector<uint8_t> kids(size_t(count) * 16);
    if (!read(tree_off + uint64_t(first) * 16, kids.data(), kids.size())) return false;

    uint64_t best = 0;
    bool have = false;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* k = kids.data() + size_t(i) * 16;
        if (k[0] & 0x80) continue;   // a file at the root
        uint32_t name_at = be32(k) & 0x7FFFFFFFu;
        if (name_at >= names_sz) continue;
        uint8_t nb[2 + 64] = {0};
        size_t want = std::min<uint64_t>(sizeof nb, names_sz - name_at);
        if (!read(names_off + name_at, nb, want)) continue;
        size_t len = nb[0], skip = 1;
        if (len & 0x80) { len = ((len & 0x7F) << 8) | nb[1]; skip = 2; }
        if (len < 16 || skip + len > want) continue;
        uint64_t id = 0;
        bool ok = true;
        for (int c = 0; c < 16 && ok; ++c) {
            char h = char(nb[skip + c]);
            int v = (h >= '0' && h <= '9') ? h - '0' : (h >= 'a' && h <= 'f') ? h - 'a' + 10 : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
            if (v < 0) ok = false; else id = (id << 4) | uint64_t(v);
        }
        if (!ok) continue;
        bool base = (id >> 32) == 0x00050000u;
        if (!have || (base && (best >> 32) != 0x00050000u)) { best = id; have = true; }
    }
    if (!have) return false;
    WiiuInfo o;
    o.container = "wua";
    o.title_id = best;
    o.serial = hex16(best);
    for (char& c : o.serial) if (c >= 'a' && c <= 'f') c = char(c - 32);   // serials are upper-case
    *out = o;
    return true;
}

bool parse_meta_xml_title_id(const std::string& xml, uint64_t* title_id) {
    size_t p = xml.find("<title_id");
    if (p == std::string::npos) return false;
    p = xml.find('>', p);
    if (p == std::string::npos) return false;
    ++p;
    while (p < xml.size() && (xml[p] == ' ' || xml[p] == '\r' || xml[p] == '\n' || xml[p] == '\t')) ++p;
    uint64_t v = 0;
    int digits = 0;
    for (; p < xml.size(); ++p, ++digits) {
        char h = xml[p];
        int d = (h >= '0' && h <= '9') ? h - '0' : (h >= 'a' && h <= 'f') ? h - 'a' + 10 : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
        if (d < 0) break;
        v = (v << 4) | uint64_t(d);
    }
    if (digits != 16 || v == 0) return false;
    *title_id = v;
    return true;
}

static bool wud_id(const uint8_t* d, size_t n, WiiuInfo* o) {
    if (n < 10 || std::memcmp(d, "WUP-", 4) != 0) return false;
    char code[11] = {0};
    std::memcpy(code, d, 10);
    for (int i = 0; i < 10; ++i)
        if (!(is_alnum(code[i]) || code[i] == '-')) return false;
    o->product_code = code;
    o->serial = std::string(code + 6, 4);
    return true;
}

bool parse_wud_header(const uint8_t* d, size_t n, WiiuInfo* out) {
    if (!d || !out) return false;
    WiiuInfo o;
    if (!wud_id(d, n, &o)) return false;
    o.container = "wud";
    *out = o;
    return true;
}

bool parse_wux(const ReadAtFn& read, uint64_t file_size, WiiuInfo* out) {
    if (!read || !out || file_size < 0x40) return false;
    uint8_t h[0x20];
    if (!read(0, h, sizeof h)) return false;
    if (std::memcmp(h, "WUX0", 4) != 0 || le32(h + 4) != 0x1099d02e) return false;
    uint32_t sector = le32(h + 8);
    uint64_t uncompressed = le64(h + 12);
    if (sector < 0x100 || sector > 0x100000 || uncompressed == 0 || uncompressed > (1ull << 50)) return false;
    uint64_t sectors = (uncompressed + sector - 1) / sector;
    uint64_t data_at = (0x20 + sectors * 4 + sector - 1) / sector * sector;
    uint8_t idx[4];
    if (!read(0x20, idx, 4)) return false;
    uint64_t at = data_at + uint64_t(le32(idx)) * sector;
    uint8_t id[16];
    if (at + sizeof id > file_size || !read(at, id, sizeof id)) return false;
    WiiuInfo o;
    if (!wud_id(id, sizeof id, &o)) return false;
    o.container = "wux";
    *out = o;
    return true;
}

std::string wiiu_key(const WiiuInfo& i) {
    if (i.title_id) return "wiiu:" + hex16(i.title_id);
    if (i.product_code.size() >= 10) {
        std::string c = i.product_code;
        for (char& ch : c) if (ch >= 'A' && ch <= 'Z') ch = char(ch + 32);
        return "wiiu:" + c;
    }
    return std::string();
}

} // namespace hub
