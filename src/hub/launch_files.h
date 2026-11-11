#pragma once
/* Small file helpers for the online launch (probe, pid and status files under the data dir). Paths are UTF-8; every call goes
 * through a wide path, so an install or profile folder with non-ASCII characters works (narrow fopen/ifstream/remove would use the
 * ANSI code page and miss the file). */
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace hublaunch {

inline std::filesystem::path u8p(const std::string& utf8) { return std::filesystem::u8path(utf8); }

inline void remove_file_utf8(const std::string& utf8) {
    std::error_code ec;
    std::filesystem::remove(u8p(utf8), ec);
}
inline std::string read_file_utf8(const std::string& utf8) {
    std::ifstream f(u8p(utf8), std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
inline bool write_file_utf8(const std::string& utf8, const std::string& data) {
    std::ofstream f(u8p(utf8), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), (std::streamsize)data.size());
    return (bool)f;
}
/* Size of a file in bytes (0 when it cannot be read). */
inline unsigned long long file_size_utf8(const std::string& utf8) {
    std::error_code ec;
    const auto n = std::filesystem::file_size(u8p(utf8), ec);
    return ec ? 0ull : (unsigned long long)n;
}

}  // namespace hublaunch
