#include "logic.h"

#include <windows.h>

namespace {
std::wstring to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n > 0 ? (size_t)n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
bool exists(const std::string& p) { return GetFileAttributesW(to_wide(p).c_str()) != INVALID_FILE_ATTRIBUTES; }
}  // namespace

namespace hubui {

bool smash_migrate_legacy_save(const std::string& legacy_path, const std::string& save_path) {
    if (!smash_should_migrate_save(exists(save_path), exists(legacy_path))) return false;
    return CopyFileW(to_wide(legacy_path).c_str(), to_wide(save_path).c_str(), TRUE) != FALSE;
}

}  // namespace hubui
