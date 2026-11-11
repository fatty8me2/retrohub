#pragma once
/* Pure text helpers of the tile / game-page code, moved out of tiles.cpp so they can be tested (hub_tiles_logic_test).
 * tiles.cpp keeps the global size_text() / initials() (declared in app.h) as one-line wrappers. */
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace hubtiles {

/* "812 bytes", "1 byte", "310 KB", "1.5 MB", "23.3 GB", "1.2 TB": binary multiples, one decimal below 100 of a unit, none above, never ".0". */
inline std::string size_text(uint64_t b) {
    char s[48];
    if (b < 1024) {
        if (b == 1) std::snprintf(s, sizeof s, "1 byte");
        else std::snprintf(s, sizeof s, "%llu bytes", (unsigned long long)b);
        return s;
    }
    static const char* const unit[] = {"bytes", "KB", "MB", "GB", "TB"};
    double v = (double)b;
    int u = 0;
    while (u < 4 && v >= 1024.0) { v /= 1024.0; ++u; }
    auto round_for_display = [](double x) { return x < 100.0 ? std::floor(x * 10.0 + 0.5) / 10.0 : std::floor(x + 0.5); };
    double r = round_for_display(v);
    if (r >= 1024.0 && u < 4) { v /= 1024.0; ++u; r = round_for_display(v); }   // 1023.97 KB reads as 1 MB
    if (std::fabs(r - std::floor(r)) < 1e-9) std::snprintf(s, sizeof s, "%.0f %s", r, unit[u]);
    else std::snprintf(s, sizeof s, "%.1f %s", r, unit[u]);
    return s;
}

/* Decodes one UTF-8 sequence at s[i] (never reads past n). Invalid, truncated, overlong or surrogate input gives U+FFFD and consumes one byte. */
inline uint32_t utf8_next(const std::string& s, size_t& i) {
    const size_t n = s.size();
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { ++i; return c; }
    int len = (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
    if (!len || i + len > n) { ++i; return 0xFFFD; }
    uint32_t cp = c & (0xFF >> (len + 1));
    for (int k = 1; k < len; ++k) {
        unsigned char d = (unsigned char)s[i + k];
        if ((d & 0xC0) != 0x80) { ++i; return 0xFFFD; }
        cp = (cp << 6) | (d & 0x3F);
    }
    if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) { ++i; return 0xFFFD; }
    i += len;
    return cp;
}

inline void utf8_append(std::string& o, uint32_t cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
}

/* Letter or digit that may start a word. ASCII alnum, Latin-1 letters, and any other script's letters; punctuation, symbols and emoji blocks are not. */
inline bool initial_candidate(uint32_t cp) {
    if (cp < 0x80) return std::isalnum((int)cp) != 0;
    if (cp < 0x100) return (cp >= 0xC0 && cp != 0xD7 && cp != 0xF7) || cp == 0xAA || cp == 0xB5 || cp == 0xBA;
    if (cp == 0xFFFD) return false;
    if ((cp >= 0x2000 && cp <= 0x2BFF) || (cp >= 0x3000 && cp <= 0x303F) || (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xFF00 && cp <= 0xFF0F) ||
        (cp >= 0xE000 && cp <= 0xF8FF) || cp >= 0x1F000) return false;
    return true;
}

/* Upper-cases ASCII and Latin-1 lower-case letters only; other scripts are left as they are. */
inline uint32_t initial_upper(uint32_t cp) {
    if (cp >= 'a' && cp <= 'z') return cp - 32;
    if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) return cp - 32;
    return cp;
}

/* The tile font (Segoe UI loaded with the default glyph range) draws U+0020..U+00FF only; anything else would be a blank box. */
inline bool initial_drawable(uint32_t cp) { return cp >= 0x20 && cp <= 0xFF; }

inline std::string initials(const std::string& t) {
    std::string o;
    size_t count = 0;
    bool start = true;
    for (size_t i = 0; i < t.size() && count < 3;) {
        uint32_t cp = utf8_next(t, i);
        if (cp == ' ' || cp == '-' || cp == ':' || cp == '.' || cp == 0xA0 || cp == 0x3000) { start = true; continue; }
        if (start && initial_candidate(cp)) {
            uint32_t u = initial_upper(cp);
            utf8_append(o, initial_drawable(u) ? u : (uint32_t)'?');
            ++count;
        }
        start = false;
    }
    if (o.empty()) {   // a non-empty title never gives a blank tile: use its first real character (a symbol or emoji reads as "?")
        for (size_t i = 0; i < t.size();) {
            uint32_t cp = utf8_next(t, i);
            if (cp < 0x80 || cp == 0xA0 || cp == 0x3000 || cp == 0xFFFD) continue;   // spaces, ASCII punctuation, broken bytes
            uint32_t u = initial_upper(cp);
            utf8_append(o, initial_drawable(u) ? u : (uint32_t)'?');
            break;
        }
    }
    if (o.empty() && !t.empty()) o = "?";   // no letters or digits at all (punctuation, spaces): a "?" tile, never a blank one
    return o;
}

}  // namespace hubtiles
