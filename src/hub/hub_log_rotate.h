#pragma once
/* Pure helpers for the hub log (hub_log.cpp): file naming, the rotation step and privacy scrubbing. No state, no globals, so the
 * unit test drives them against a temp directory. Windows only (wide paths, MoveFileExW). */
#include <string>
#include <cstddef>
#include <cstdint>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace hublog {

constexpr unsigned long long kMaxBytes = 1ull << 20;   // rotate once hub.log reaches 1 MB
constexpr int kKeepFiles = 3;                          // hub.log, hub.1.log, hub.2.log

inline std::wstring file_name(const std::wstring& dir, int index) {
    return dir + (index == 0 ? L"hub.log" : L"hub." + std::to_wstring(index) + L".log");
}

inline bool needs_rotation(unsigned long long current_size, unsigned long long incoming, unsigned long long max_bytes = kMaxBytes) {
    return current_size > 0 && current_size + incoming > max_bytes;
}

/* hub.1.log -> hub.2.log, hub.log -> hub.1.log; the oldest file is deleted. Call with no handle open on hub.log. Never throws. */
inline void rotate(const std::wstring& dir, int keep = kKeepFiles) {
    if (keep < 1) return;
    DeleteFileW(file_name(dir, keep - 1).c_str());
    for (int i = keep - 2; i >= 0; --i)
        MoveFileExW(file_name(dir, i).c_str(), file_name(dir, i + 1).c_str(), MOVEFILE_REPLACE_EXISTING);
}

inline char lower_ascii(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* Replaces every occurrence of profile_dir (case-insensitive, either slash direction) with "%USERPROFILE%", and every dotted-quad
 * (IPv4-looking) number with "<ip>". profile_dir is UTF-8 and may be empty. */
inline std::string scrub(const std::string& in, const std::string& profile_dir) {
    std::string prof = profile_dir;
    while (!prof.empty() && (prof.back() == '\\' || prof.back() == '/')) prof.pop_back();
    for (char& c : prof) if (c == '/') c = '\\';
    std::string out;
    out.reserve(in.size());
    const size_t n = in.size();
    for (size_t i = 0; i < n;) {
        if (!prof.empty() && i + prof.size() <= n) {
            bool eq = true;
            for (size_t k = 0; k < prof.size() && eq; ++k) {
                char a = in[i + k] == '/' ? '\\' : in[i + k];
                eq = lower_ascii(a) == lower_ascii(prof[k]);
            }
            if (eq) {
                const size_t e = i + prof.size();
                const bool boundary = e >= n || in[e] == '\\' || in[e] == '/' || in[e] == '"' || in[e] == '\'' || in[e] == ' ' || in[e] == ',' || in[e] == ';' || in[e] == ')' || in[e] == '\n';
                if (boundary) { out += "%USERPROFILE%"; i = e; continue; }
            }
        }
        // dotted quad: d{1,3}.d{1,3}.d{1,3}.d{1,3}, not glued to more digits or dots
        if (in[i] >= '0' && in[i] <= '9' && (i == 0 || !((in[i - 1] >= '0' && in[i - 1] <= '9') || in[i - 1] == '.'))) {
            size_t p = i;
            int parts = 0;
            bool ok = true;
            while (parts < 4 && ok) {
                size_t s = p;
                while (p < n && in[p] >= '0' && in[p] <= '9' && p - s < 4) ++p;
                if (p == s || p - s > 3) { ok = false; break; }
                ++parts;
                if (parts < 4) { if (p < n && in[p] == '.') ++p; else ok = false; }
            }
            if (ok && parts == 4 && !(p < n && ((in[p] >= '0' && in[p] <= '9') || (in[p] == '.' && p + 1 < n && in[p + 1] >= '0' && in[p + 1] <= '9')))) {
                out += "<ip>";
                i = p;
                continue;
            }
        }
        out += in[i++];
    }
    return out;
}

}  // namespace hublog
