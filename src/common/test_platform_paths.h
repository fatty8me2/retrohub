#pragma once
/* Test helper: the tests spell their fixture paths the Windows way ("C:\\Users\\a\\Documents"). wp() leaves them untouched on Windows and
 * maps them to the platform form elsewhere: the drive prefix is dropped and '\\' becomes '/' ("C:\\U\\Desktop" -> "/U/Desktop"), so the same
 * expectation checks the same logic on both platforms. */
#include "plat/plat.h"

#include <string>

inline std::string wp(const std::string& s) {
#if RH_WIN
    return s;
#else
    std::string r = s;
    if (r.size() >= 2 && ((r[0] >= 'A' && r[0] <= 'Z') || (r[0] >= 'a' && r[0] <= 'z')) && r[1] == ':') r.erase(0, 2);
    for (char& c : r) if (c == '\\') c = '/';
    return r;
#endif
}

#if !RH_WIN
#include <cstdlib>
#include <filesystem>
#include <vector>

/* macOS: `tar --format 7zip -cf <out> -C <stage> <members...>` (bsdtar writes 7z; Windows tests do the same through System32\tar.exe). */
inline void tar7z(const std::filesystem::path& out, const std::filesystem::path& stage, const std::vector<std::string>& members) {
    auto q = [](const std::string& s) {
        std::string r = "'";
        for (char c : s) { if (c == '\'') r += "'\\''"; else r += c; }
        return r + "'";
    };
    std::string cmd = "/usr/bin/tar --format 7zip -cf " + q(out.string()) + " -C " + q(stage.string());
    for (const std::string& m : members) cmd += " " + q(m);
    cmd += " >/dev/null 2>&1";
    (void)std::system(cmd.c_str());
}
#endif
