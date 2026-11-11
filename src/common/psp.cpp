#include "psp.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "miniz.h"

namespace hub { namespace psp {

namespace fs = std::filesystem;
static std::string lower(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c + 32); return s; }
static std::string upper(std::string s) { for (char& c : s) if (c >= 'a' && c <= 'z') c = char(c - 32); return s; }
static uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
static uint32_t le16(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8); }

bool parse_sfo(const uint8_t* d, size_t n, Sfo* out) {
    *out = Sfo();
    if (n < 20 || std::memcmp(d, "\0PSF", 4) != 0) return false;
    uint32_t kt = le32(d + 8), dt = le32(d + 12), cnt = le32(d + 16);
    if (cnt > 512 || kt >= n || dt >= n) return false;
    for (uint32_t i = 0; i < cnt; ++i) {
        size_t at = 20 + size_t(i) * 16;
        if (at + 16 > n) break;
        const uint8_t* e = d + at;
        uint32_t ko = le16(e), fmt = le16(e + 2), len = le32(e + 4), doff = le32(e + 12);
        if (size_t(kt) + ko >= n || size_t(dt) + doff > n) continue;
        std::string key((const char*)d + kt + ko, strnlen((const char*)d + kt + ko, n - kt - ko));
        size_t avail = n - dt - doff;
        if (fmt == 0x0404) {
            if (avail >= 4) out->num[key] = le32(d + dt + doff);
        } else {
            size_t l = std::min<size_t>(len, avail);
            out->str[key] = std::string((const char*)d + dt + doff, strnlen((const char*)d + dt + doff, l));
        }
    }
    out->ok = true;
    return true;
}

std::string normalize_disc_id(const std::string& s) {
    std::string letters, digits;
    size_t i = 0;
    for (; i < s.size() && letters.size() < 4; ++i) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return "";
        letters += c;
    }
    if (letters.size() != 4) return "";
    if (i < s.size() && (s[i] == '-' || s[i] == '_')) ++i;
    for (; i < s.size() && digits.size() < 5; ++i) {
        if (s[i] < '0' || s[i] > '9') return "";
        digits += s[i];
    }
    if (digits.size() != 5 || i != s.size()) return "";
    return upper(letters) + digits;
}

static void from_sfo(const Sfo& sfo, Ident* out) {
    out->disc_id = normalize_disc_id(sfo.get("DISC_ID"));
    out->title = sfo.get("TITLE");
    out->category = sfo.get("CATEGORY");
}

/* ---- ISO9660 ---- */
static bool dir_find(const disc::SectorFn& rd, uint32_t lba, uint32_t size, const std::string& want, uint32_t* o_lba, uint32_t* o_size) {
    if (size == 0 || size > (1u << 20)) return false;
    uint8_t s[2048];
    for (uint32_t off = 0; off < size; off += 2048) {
        if (!rd(lba + off / 2048, s)) return false;
        uint32_t pos = 0;
        while (pos < 2048) {
            uint32_t len = s[pos];
            if (len == 0) break;
            if (pos + len > 2048 || len < 34) break;
            uint32_t nl = s[pos + 32];
            if (33 + nl <= len) {
                std::string name((const char*)s + pos + 33, nl);
                size_t semi = name.find(';');
                if (semi != std::string::npos) name.resize(semi);
                if (lower(name) == lower(want)) { *o_lba = le32(s + pos + 2); *o_size = le32(s + pos + 10); return true; }
            }
            pos += len;
        }
    }
    return false;
}

bool identify_reader(const disc::SectorFn& rd, Ident* out) {
    uint8_t s[2048];
    if (!rd || !rd(16, s) || s[0] != 1 || std::memcmp(s + 1, "CD001", 5) != 0) return false;
    uint32_t lba = le32(s + 158), size = le32(s + 166), a, b;
    if (!dir_find(rd, lba, size, "PSP_GAME", &a, &b)) return false;
    uint32_t flba, fsize;
    if (!dir_find(rd, a, b, "PARAM.SFO", &flba, &fsize) || fsize < 20 || fsize > (1u << 16)) return false;
    std::vector<uint8_t> buf;
    for (uint32_t o = 0; o < fsize; o += 2048) {
        if (!rd(flba + o / 2048, s)) return false;
        buf.insert(buf.end(), s, s + std::min<uint32_t>(2048, fsize - o));
    }
    Sfo sfo;
    if (!parse_sfo(buf.data(), buf.size(), &sfo)) return false;
    *out = Ident();
    from_sfo(sfo, out);
    return true;
}

/* ---- PBP ---- */
bool identify_pbp(const uint8_t* h, size_t n, const uint8_t* psar, size_t psar_n, Ident* out) {
    if (n < 0x28 || std::memcmp(h, "\0PBP", 4) != 0) return false;
    uint32_t sfo = le32(h + 8), next = le32(h + 12);
    if (next <= sfo || next - sfo > (1u << 16) || sfo < 0x28 || size_t(next) > n) return false;
    Sfo s;
    if (!parse_sfo(h + sfo, next - sfo, &s)) return false;
    *out = Ident();
    from_sfo(s, out);
    bool ps1 = upper(out->category) == "ME";
    if (psar && psar_n >= 8 && (std::memcmp(psar, "PSISOIMG", 8) == 0 || (psar_n >= 10 && std::memcmp(psar, "PSTITLEIMG", 10) == 0))) ps1 = true;
    out->ps1 = ps1;
    return !ps1;
}

static bool pbp_file(const std::string& path, Ident* out) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> head(0x12000);   // header + PARAM.SFO + the small icon (the SFO sits first)
    f.read((char*)head.data(), (std::streamsize)head.size());
    head.resize((size_t)f.gcount());
    if (head.size() < 0x28) return false;
    uint32_t next = le32(head.data() + 12), psar_off = le32(head.data() + 36);
    if (size_t(next) > head.size()) {   // SFO beyond the first block: read exactly up to it
        if (next > (1u << 22)) return false;
        head.resize(next);
        f.clear(); f.seekg(0); f.read((char*)head.data(), (std::streamsize)head.size());
        if ((size_t)f.gcount() != head.size()) return false;
    }
    uint8_t psar[16] = {};
    size_t pn = 0;
    f.clear(); f.seekg(psar_off);
    if (f) { f.read((char*)psar, sizeof psar); pn = (size_t)f.gcount(); }
    return identify_pbp(head.data(), head.size(), psar, pn, out);
}

/* ---- CSO in memory ---- */
disc::SectorFn cso_buffer_reader(std::shared_ptr<std::string> data) {
    if (!data || data->size() < 24 || std::memcmp(data->data(), "CISO", 4) != 0) return disc::SectorFn();
    const uint8_t* d = (const uint8_t*)data->data();
    uint64_t total = uint64_t(le32(d + 8)) | (uint64_t(le32(d + 12)) << 32);
    uint32_t block = le32(d + 16);
    int align = d[21];
    if (block < 2048 || block > (1u << 20) || (block & (block - 1)) || total == 0) return disc::SectorFn();
    return [data, total, block, align](uint32_t lba, uint8_t* out) -> bool {
        const uint8_t* b = (const uint8_t*)data->data();
        uint64_t off = uint64_t(lba) * 2048;
        if (off + 2048 > total) return false;
        uint64_t bi = off / block;
        if ((bi + 2) * 4 + 24 > data->size()) return false;
        uint32_t e0 = le32(b + 24 + bi * 4), e1 = le32(b + 24 + (bi + 1) * 4);
        uint64_t p0 = uint64_t(e0 & 0x7FFFFFFFu) << align, p1 = uint64_t(e1 & 0x7FFFFFFFu) << align;
        if (p1 <= p0 || p1 > data->size() || p1 - p0 > (2u << 20)) return false;
        std::vector<uint8_t> blk(block, 0);
        if (e0 & 0x80000000u) std::memcpy(blk.data(), b + p0, std::min<size_t>(size_t(p1 - p0), block));
        else if (tinfl_decompress_mem_to_mem(blk.data(), block, b + p0, size_t(p1 - p0), 0) == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) return false;
        std::memcpy(out, blk.data() + (off % block), 2048);
        return true;
    };
}

bool identify_buffer(std::shared_ptr<std::string> data, Ident* out) {
    if (!data || data->size() < 0x28) return false;
    if (std::memcmp(data->data(), "\0PBP", 4) == 0) return identify_pbp((const uint8_t*)data->data(), data->size(), nullptr, 0, out);
    if (std::memcmp(data->data(), "CISO", 4) == 0) return identify_reader(cso_buffer_reader(data), out);
    return identify_reader(disc::buffer_reader(data), out);
}

bool identify_file(const std::string& path, Ident* out) {
    *out = Ident();
    std::string ext = lower(fs::u8path(path).extension().u8string());
    if (ext == ".pbp") return pbp_file(path, out);
    if (ext != ".iso" && ext != ".cso" && ext != ".ciso" && ext != ".chd") return false;
    disc::Disc d;
    if (!d.open(path)) return false;
    return identify_reader(d.reader(), out);
}

std::string key_for(const std::string& id, const std::string& stem) {
    return id.empty() ? "psp:file-" + stem : "psp:" + lower(id);
}

std::string disc_id_from_key(const std::string& key) {
    if (key.compare(0, 4, "psp:") != 0) return "";
    return normalize_disc_id(upper(key.substr(4)));
}

bool savedata_folder_matches(const std::string& folder, const std::string& id) {
    return id.size() == 9 && upper(folder).compare(0, 9, id) == 0;
}

}} // namespace hub::psp
