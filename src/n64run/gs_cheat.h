#pragma once
/* GameShark RAM-write codes for headless QA (n64run --gs-code). Supports 80/81 (8/16-bit constant write),
 * 88/89 are treated like 80/81, and D0/D1 (apply next code if equal) / D2/D3 (if not equal). Boot/enable
 * codes (F0/F1/DE...) are ignored: an emulator-side writer needs none. Pure logic, unit tested. */
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace n64run {

struct GsLine { uint8_t op; uint32_t addr; uint16_t val; };

/* "80ADDR 00VV+81ADDR VVVV" (lines separated by '+', ',' or newline). False on malformed text. */
inline bool gs_parse(const std::string& text, std::vector<GsLine>* out) {
    std::string t = text;
    for (char& c : t) if (c == '+' || c == ',' || c == '\n' || c == '\r') c = ';';
    std::stringstream ss(t); std::string line;
    while (std::getline(ss, line, ';')) {
        size_t a = line.find_first_not_of(' '); if (a == std::string::npos) continue;
        line = line.substr(a);
        unsigned long hi, lo; char* e;
        hi = std::strtoul(line.c_str(), &e, 16);
        if (e - line.c_str() != 8) return false;
        while (*e == ' ') e++;
        lo = std::strtoul(e, &e, 16);
        if (lo > 0xFFFF) return false;
        out->push_back({(uint8_t)(hi >> 24), (uint32_t)(hi & 0xFFFFFF), (uint16_t)lo});
    }
    return !out->empty();
}

/* ram: RDRAM as host-endian 32-bit words (mupen64plus layout): N64 byte at physical p is ram[p ^ 3]. */
inline uint8_t gs_rd8(const uint8_t* ram, uint32_t a) { return ram[a ^ 3u]; }
inline uint16_t gs_rd16(const uint8_t* ram, uint32_t a) { return (uint16_t)((ram[a ^ 3u] << 8) | ram[(a + 1) ^ 3u]); }

inline void gs_apply(uint8_t* ram, uint32_t ram_size, const std::vector<GsLine>& codes) {
    bool skip = false;
    for (const GsLine& c : codes) {
        if (skip) { skip = false; continue; }
        uint32_t a = c.addr & 0x7FFFFF;
        if (a + 2 > ram_size) continue;
        switch (c.op) {
            case 0x80: case 0x88: ram[a ^ 3u] = (uint8_t)c.val; break;
            case 0x81: case 0x89: if (a & 1) break; ram[a ^ 3u] = (uint8_t)(c.val >> 8); ram[(a + 1) ^ 3u] = (uint8_t)c.val; break;
            case 0xD0: skip = gs_rd8(ram, a) != (uint8_t)c.val; break;
            case 0xD1: skip = gs_rd16(ram, a) != c.val; break;
            case 0xD2: skip = gs_rd8(ram, a) == (uint8_t)c.val; break;
            case 0xD3: skip = gs_rd16(ram, a) == c.val; break;
            default: break;
        }
    }
}

}  // namespace n64run
