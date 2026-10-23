#include "logic.h"
#include "winutil.h"

#include <windows.h>

namespace {
using winutil::to_wide;
bool exists(const std::string& p) { return GetFileAttributesW(to_wide(p).c_str()) != INVALID_FILE_ATTRIBUTES; }
}  // namespace

namespace hubui {

bool smash_migrate_legacy_save(const std::string& legacy_path, const std::string& save_path) {
    if (!smash_should_migrate_save(exists(save_path), exists(legacy_path))) return false;
    return CopyFileW(to_wide(legacy_path).c_str(), to_wide(save_path).c_str(), TRUE) != FALSE;
}

}  // namespace hubui
