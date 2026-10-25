/* File classes of the cloud saves v2 engine (docs/CLOUD_SAVES_V2.md section 2). Pure. */
#include "cloud_internal.h"

#include <algorithm>
#include <cctype>

namespace hub::saves {

namespace {
std::string lower(std::string s) {
    for (auto& c : s) { if (c == '\\') c = '/'; c = (char)tolower((unsigned char)c); }
    return s;
}
bool ends_with(const std::string& s, const char* suf) {
    size_t n = std::char_traits<char>::length(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}
bool image_ext(const std::string& s) {
    return ends_with(s, ".png") || ends_with(s, ".jpg") || ends_with(s, ".jpeg") || ends_with(s, ".bmp") || ends_with(s, ".webp");
}
bool card_ext(const std::string& s) {
    return ends_with(s, ".gci") || ends_with(s, ".gcs") || ends_with(s, ".mcd") || ends_with(s, ".mcr") || ends_with(s, ".mc") ||
           ends_with(s, ".ps2") || ends_with(s, ".mcc") || ends_with(s, ".card") || ends_with(s, ".vmp") || ends_with(s, ".vmc") ||
           ends_with(s, ".mpk") || ends_with(s, ".raw");
}
}  // namespace

FileClass classify_path(const std::string& rel_in_game, int64_t size) {
    std::string p = lower(rel_in_game);
    if (p == "meta.json" || p.rfind("_account/", 0) == 0) return FileClass::Settings;
    if (p.rfind("states/", 0) == 0 || p.rfind("state/", 0) == 0) return image_ext(p) ? FileClass::Shot : FileClass::State;
    if (p.rfind("cheat_backups/", 0) == 0) return FileClass::State;           // copies of battery files: never protected
    if (image_ext(p)) return FileClass::Shot;
    if (p.rfind("battery/", 0) == 0) {
        std::string name = p.substr(8);
        if (card_ext(name) || name.find("memcard") != std::string::npos || name.find("memory card") != std::string::npos) return FileClass::Card;
        if (name.rfind("card", 0) == 0 && size > 64 * 1024) return FileClass::Card;
        return FileClass::Battery;
    }
    if (card_ext(p)) return FileClass::Card;
    /* unknown location: small files behave like battery data, big ones like cards (limited protected share) */
    return size <= 64 * 1024 ? FileClass::Battery : FileClass::Card;
}

bool is_protected(FileClass c, int64_t size) {
    switch (c) {
        case FileClass::Settings:
        case FileClass::Battery: return true;
        case FileClass::Card: return size <= kProtectedCardMax;
        default: return false;
    }
}

const char* class_name(FileClass c) {
    switch (c) {
        case FileClass::Settings: return "settings";
        case FileClass::Battery: return "battery";
        case FileClass::Card: return "card";
        case FileClass::State: return "state";
        case FileClass::Shot: return "shot";
    }
    return "battery";
}
FileClass class_from_name(const std::string& s, FileClass def) {
    if (s == "settings") return FileClass::Settings;
    if (s == "battery") return FileClass::Battery;
    if (s == "card") return FileClass::Card;
    if (s == "state") return FileClass::State;
    if (s == "shot") return FileClass::Shot;
    return def;
}

}  // namespace hub::saves
