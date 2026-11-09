#pragma once
/* Game Boy cartridge header helpers (pure, header-only; used by the hub and retrorun).
 * Why: the Gambatte core keeps a cartridge's real-time clock on wall time, so two PCs in an online room drift apart.
 * Online play never uses Gambatte for a game with a clock (docs/CORES.md). */
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace gbcart {

/* Cartridge type byte (header offset 0x147) of a cart that has a real-time clock:
 * 0x0F / 0x10 = MBC3+TIMER (Pokemon Gold/Silver/Crystal), 0xFE = HuC3. */
inline bool gb_cart_has_clock(uint8_t cart_type) { return cart_type == 0x0F || cart_type == 0x10 || cart_type == 0xFE; }

/* Which Game Boy core a non-link online room runs: SameBoy replaces Gambatte for a game with a clock; everything else keeps the setting. */
inline std::string online_gb_core(const std::string& setting, bool has_clock) {
    return (setting == "gambatte" && has_clock) ? std::string("sameboy") : setting;
}

/* Reads the cartridge type from a plain .gb / .gbc / .sgb file. Returns false when the file is not one of those or has no readable header
 * (archives, missing file): callers then keep the setting. `path` is UTF-8. */
inline bool read_cart_type(const std::wstring& wpath, uint8_t* out) {
    auto ends = [&](const wchar_t* ext) {
        size_t n = 0; while (ext[n]) n++;
        if (wpath.size() < n) return false;
        for (size_t i = 0; i < n; i++) {
            wchar_t c = wpath[wpath.size() - n + i];
            if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
            if (c != ext[i]) return false;
        }
        return true;
    };
    if (!ends(L".gb") && !ends(L".gbc") && !ends(L".sgb")) return false;
    FILE* f = nullptr;
    if (_wfopen_s(&f, wpath.c_str(), L"rb") != 0 || !f) return false;
    unsigned char hdr[0x150];
    size_t got = fread(hdr, 1, sizeof hdr, f);
    fclose(f);
    if (got < sizeof hdr) return false;
    *out = hdr[0x147];
    return true;
}

}  // namespace gbcart
