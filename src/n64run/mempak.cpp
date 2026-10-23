#include "mempak.h"

#include <algorithm>
#include <cctype>

#include "save_sync.h"

namespace n64run {

bool mempak_enabled(bool online, bool share_host_save) {
    return !online || share_host_save;
}

bool rumble_enabled(bool online, bool profile_rumble) { return !online && profile_rumble; }

bool decode_rumble_command(const unsigned char* cmd, bool* on) {
    if (cmd[2] != 0x03 || cmd[3] != 0xC0) return false;   // JCMD_PAK_WRITE to 0xC000 (address high byte | crc in cmd[4])
    *on = cmd[5] != 0;
    return true;
}

bool is_mempak_file(const std::string& name) {
    if (!valid_save_name(name)) return false;
    std::string ext = name.size() >= 4 ? name.substr(name.size() - 4) : std::string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return ext == ".mpk";
}

}  // namespace n64run
