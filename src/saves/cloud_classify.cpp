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
std::string ext_of(const std::string& name) {
    size_t i = name.rfind('.');
    return i == std::string::npos ? std::string() : name.substr(i + 1);
}
}  // namespace

/* Exact mirror of classifyPath() in the Worker (server/cloudflare/src/saves_pool.js); golden table docs/cloud_classify_cases.txt. */
FileClass classify_path(const std::string& rel_in_game, int64_t size) {
    std::string p = lower(rel_in_game);
    size_t slash = p.find('/');
    std::string seg0 = slash == std::string::npos ? p : p.substr(0, slash);
    std::string name = p.substr(p.rfind('/') == std::string::npos ? 0 : p.rfind('/') + 1);
    if (seg0 == "_account" || name == "meta.json") return FileClass::Settings;
    if (p == "cover.png") return FileClass::Settings;   // the user's custom cover at the game root (mirror of saves_pool.js)
    std::string ext = ext_of(name);
    if (seg0 == "states") {
        static const char* shots[] = {"png", "jpg", "jpeg", "bmp", "tga", "webp"};
        for (const char* e : shots) if (ext == e) return FileClass::Shot;
        return FileClass::State;
    }
    static const char* cards[] = {"mcd", "mcr", "gci", "gcp", "vmp", "vmc", "ps2", "mc2", "card", "raw"};
    for (const char* e : cards) if (ext == e) return FileClass::Card;
    if (seg0 == "battery") return FileClass::Battery;
    return size > kProtectedCardMax ? FileClass::Card : FileClass::Battery;
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
