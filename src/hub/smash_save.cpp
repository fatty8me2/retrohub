#include "logic.h"
#include "winutil.h"
#include "plat/plat.h"

#if RH_WIN
#include <windows.h>
#else
#include <filesystem>
#endif

namespace {
#if RH_WIN
using winutil::to_wide;
bool exists(const std::string& p) { return GetFileAttributesW(to_wide(p).c_str()) != INVALID_FILE_ATTRIBUTES; }
#else
bool exists(const std::string& p) { std::error_code ec; return std::filesystem::exists(std::filesystem::u8path(p), ec); }
#endif
}  // namespace

namespace hubui {

bool smash_migrate_legacy_save(const std::string& legacy_path, const std::string& save_path) {
    if (!smash_should_migrate_save(exists(save_path), exists(legacy_path))) return false;
#if RH_WIN
    return CopyFileW(to_wide(legacy_path).c_str(), to_wide(save_path).c_str(), TRUE) != FALSE;
#else
    std::error_code ec;   // CopyFileW(..., bFailIfExists = TRUE): never overwrites
    return std::filesystem::copy_file(std::filesystem::u8path(legacy_path), std::filesystem::u8path(save_path), std::filesystem::copy_options::none, ec) && !ec;
#endif
}

}  // namespace hubui
