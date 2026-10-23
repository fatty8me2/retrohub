#include "controls_sdl.h"

#include <SDL.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>

#include "games.h"

namespace hubctl {

static_assert(SDL_CONTROLLER_BUTTON_A == 0 && SDL_CONTROLLER_BUTTON_DPAD_RIGHT == 14 && SDL_CONTROLLER_BUTTON_MAX >= 15, "button order");
static_assert(SDL_CONTROLLER_AXIS_TRIGGERRIGHT == 5, "axis order");

static void ensure_init() {
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER)) SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);
}

int load_gamecontroller_db(const std::string& path) {
    ensure_init();
    return SDL_GameControllerAddMappingsFromFile(path.c_str());
}

int load_bundled_db() {
    namespace fs = std::filesystem;
    fs::path ex = fs::u8path(hub::exe_dir());
    for (const fs::path& p : {ex / "controls" / "gamecontrollerdb.txt", ex / ".." / ".." / "assets" / "controls" / "gamecontrollerdb.txt"}) {
        std::error_code ec;
        if (fs::exists(p, ec)) return load_gamecontroller_db(p.u8string());
    }
    return -1;
}

static std::string guid_str(int idx) {
    char b[64] = {0};
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(idx), b, sizeof b);
    return b;
}

std::string pad_key(SDL_GameController* c) {
    SDL_Joystick* j = SDL_GameControllerGetJoystick(c);
    char b[64] = {0};
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(j), b, sizeof b);
    std::string k = b;
    const char* sn = SDL_JoystickGetSerial(j);
    if (sn && *sn) k += std::string("#") + sn;
    return k;
}

std::vector<PadInfo> list_pads(const std::vector<std::string>& order) {
    ensure_init();
    SDL_JoystickUpdate();
    std::vector<PadInfo> v;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i)) continue;
        PadInfo p;
        p.sdl_index = i;
        const char* n = SDL_GameControllerNameForIndex(i);
        p.name = n ? n : "Controller";
        p.guid = guid_str(i);
        SDL_GameControllerType t = SDL_GameControllerTypeForIndex(i);
        p.xinput = t == SDL_CONTROLLER_TYPE_XBOX360 || t == SDL_CONTROLLER_TYPE_XBOXONE;
        // The serial is only available on an open device; open briefly.
        if (SDL_GameController* c = SDL_GameControllerOpen(i)) { p.key = pad_key(c); SDL_GameControllerClose(c); }
        if (p.key.empty()) p.key = p.guid;
        v.push_back(p);
    }
    std::vector<std::string> keys;
    for (auto& p : v) keys.push_back(p.key);
    keys = uniquify_keys(keys);   // two identical controllers must not share a key
    for (size_t i = 0; i < v.size(); ++i) v[i].key = keys[i];
    std::vector<PadInfo> out;
    for (int idx : order_pads(order, keys)) { v[idx].port = (int)out.size(); out.push_back(v[idx]); }
    return out;
}

std::vector<SDL_GameController*> open_pads(const std::vector<std::string>& order, std::vector<std::string>* names) {
    ensure_init();
    std::vector<SDL_GameController*> all, out;
    std::vector<std::string> keys;
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i))
            if (SDL_GameController* c = SDL_GameControllerOpen(i)) { all.push_back(c); keys.push_back(pad_key(c)); }
    keys = uniquify_keys(keys);
    for (int idx : order_pads(order, keys)) {
        out.push_back(all[idx]);
        if (names) names->push_back(SDL_GameControllerName(all[idx]) ? SDL_GameControllerName(all[idx]) : "");
    }
    return out;
}

void read_state(SDL_GameController* c, PadState* s) {
    for (int i = 0; i < kNumButtons; i++)
        s->b[i] = i < SDL_CONTROLLER_BUTTON_MAX && SDL_GameControllerGetButton(c, (SDL_GameControllerButton)i) != 0;
    for (int i = 0; i < kNumAxes; i++) s->a[i] = SDL_GameControllerGetAxis(c, (SDL_GameControllerAxis)i);
}

Capture::~Capture() { cancel(); }

void Capture::cancel() {
    for (auto* c : pads_) SDL_GameControllerClose(c);
    pads_.clear();
    rest_.clear();
}

bool Capture::start(const std::string& key) {
    cancel();
    ensure_init();
    SDL_JoystickUpdate();
    std::vector<SDL_GameController*> opened;
    std::vector<std::string> keys;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i)) continue;
        SDL_GameController* c = SDL_GameControllerOpen(i);
        if (!c) continue;
        opened.push_back(c);
        keys.push_back(pad_key(c));
    }
    keys = uniquify_keys(keys);
    for (size_t i = 0; i < opened.size(); ++i) {
        if (!key.empty() && keys[i] != key) { SDL_GameControllerClose(opened[i]); continue; }
        pads_.push_back(opened[i]);
    }
    for (auto* c : pads_) { SDL_GameControllerUpdate(); PadState s; read_state(c, &s); rest_.push_back(s); }
    return !pads_.empty();
}

std::string Capture::poll() {
    if (pads_.empty()) return "";
    SDL_GameControllerUpdate();
    for (size_t k = 0; k < pads_.size(); k++) {
        PadState s;
        read_state(pads_[k], &s);
        for (int i = 0; i < kNumButtons; i++) if (s.b[i] && !rest_[k].b[i]) { std::string t = button_name(i); cancel(); return t; }
        for (int i = 0; i < kNumAxes; i++) {
            int d = s.a[i] - rest_[k].a[i];
            if (std::abs(d) > 20000) {
                std::string t = axis_name(i);
                if (i < 4) t += d > 0 ? "+" : "-";
                cancel();
                return t;
            }
        }
    }
    return "";
}

}  // namespace hubctl
