#include "mempak.h"

#include <algorithm>
#include <cctype>

#include "save_sync.h"

namespace n64run {

bool mempak_enabled(bool online, bool share_host_save) {
    return !online || share_host_save;
}

bool is_mempak_file(const std::string& name) {
    if (!valid_save_name(name)) return false;
    std::string ext = name.size() >= 4 ? name.substr(name.size() - 4) : std::string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return ext == ".mpk";
}

}  // namespace n64run
