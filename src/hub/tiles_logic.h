#pragma once
/* Pure text helpers of the tile / game-page code, moved out of tiles.cpp so they can be tested (hub_tiles_logic_test).
 * tiles.cpp keeps the global size_text() / initials() (declared in app.h) as one-line wrappers. */
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>

namespace hubtiles {

inline std::string size_text(uint64_t b) {
    char s[32];
    if (b < 1024) std::snprintf(s, sizeof s, "%llu B", (unsigned long long)b);
    else if (b < 1024 * 1024) std::snprintf(s, sizeof s, "%.1f KB", b / 1024.0);
    else std::snprintf(s, sizeof s, "%.1f MB", b / 1048576.0);
    return s;
}

inline std::string initials(const std::string& t) {
    std::string o;
    bool start = true;
    for (unsigned char c : t) {
        if (c == ' ' || c == '-' || c == ':' || c == '.') { start = true; continue; }
        if (start && std::isalnum(c)) {
            o += (char)std::toupper(c);
            if (o.size() >= 3) break;
        }
        start = false;
    }
    return o;
}

}  // namespace hubtiles
