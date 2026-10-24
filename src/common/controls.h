#pragma once
/* Controller profiles: one model shared by n64run, retrorun and the generated Dolphin / Azahar / Cemu configs.
 *
 * Pure logic (no SDL, no Windows): see controls_sdl.h for the runtime half (pad list, "press a button" capture,
 * gamecontrollerdb). docs/CONTROLS.md has the file formats.
 *
 * A Profile maps a system's logical controls ("a", "z", "dpad_up", ...) to SDL GameController sources:
 *   expr   := term ('|' term)*                     (any term pressed = control pressed)
 *   term   := name [ '+' | '-' ] [ '@' threshold ]
 *   name   := a b x y back guide start leftstick rightstick leftshoulder rightshoulder dpup dpdown dpleft dpright
 *             misc1 paddle1..paddle4 touchpad                       (buttons)
 *             leftx lefty rightx righty lefttrigger righttrigger    (axes)
 * An axis term fires when the axis passes the threshold (default Profile::axis_threshold) in the given direction
 * ('+' = value > thr, '-' = value < -thr, none = value > thr, for triggers). Analog controls (sticks) name two axes
 * and carry a deadzone (radial, rescaled) and a sensitivity (output scale, 1 = unchanged).
 * The built-in default profiles reproduce the old hardcoded mappings exactly (tests prove it). */

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "library.h"

namespace hubctl {

constexpr int kNumButtons = 21;   // SDL_CONTROLLER_BUTTON_* order
constexpr int kNumAxes = 6;       // leftx lefty rightx righty lefttrigger righttrigger
constexpr int kDefaultAxisThreshold = 16384;

/* System keys: "n64", "retro" (GBA/GB/GBC/DS libretro cores), "ps1" (PlayStation, DualShock), "gc", "wiiu", "3ds". */
const std::vector<std::string>& systems();
bool is_system(const std::string& s);
std::string system_key(hub::System s);   // "" when RetroHub has no profile for it

struct ControlInfo {
    std::string name;    // key in Profile::buttons / Profile::analogs
    std::string label;   // what the UI shows ("A", "Z trigger", "Left stick")
    bool analog;
};
const std::vector<ControlInfo>& controls_for(const std::string& system);

struct Analog {
    std::string x, y;          // axis names ("leftx" / "lefty"); empty for settings-only entries
    double deadzone = 0.08;    // 0..0.95, radial
    double sensitivity = 1.0;  // output scale
    bool operator==(const Analog& o) const { return x == o.x && y == o.y && deadzone == o.deadzone && sensitivity == o.sensitivity; }
};

struct Profile {
    std::string name;     // "default-n64", "n64-custom", "game:<id>" ...
    std::string system;
    std::map<std::string, std::string> buttons;   // control -> expr
    std::map<std::string, Analog> analogs;
    int axis_threshold = kDefaultAxisThreshold;
    std::map<std::string, std::string> keys;      // keyboard "pad": control -> key expr ("x|space"), see key_controls_for()
    bool rumble = false;                          // n64 only: attach a Rumble Pak instead of a Controller Pak (offline)
    std::vector<std::string> port_order;          // filled by resolve(): pad keys, first = port 1
    /* Equality ignores name and port_order (what the emulator configs are generated from). */
    bool same_mapping(const Profile& o) const;
};

Profile default_profile(const std::string& system);   // name "default-<system>"
uint64_t mapping_hash(const Profile& p);               // FNV-1a over the canonical mapping (not name / port_order)
std::string hash_hex(uint64_t h);

/* ---- keyboard "pad" ----
 * The keyboard is a pad of its own: Profile::keys maps the same control names (plus n64 stick_up/down/left/right) to key
 * expressions, `key ('|' key)*`, key = a-z 0-9 up down left right return kp_enter space tab backspace lshift rshift lctrl
 * rctrl lalt ralt f1..f12 kp0..kp9 comma period slash semicolon quote lbracket rbracket minus equals backslash grave
 * insert delete home end pageup pagedown. It drives port 1 (n64run, retrorun) and Dolphin's GCPad1. Systems without a
 * keyboard pad (wiiu, 3ds: Cemu / Azahar take one source per control) have no key controls. The defaults are the old
 * fixed bindings. Key codes are SDL scancodes, so the runners index SDL_GetKeyboardState with them directly. */
const std::vector<ControlInfo>& key_controls_for(const std::string& system);   // empty = no keyboard pad
bool has_keyboard(const std::string& system);
int key_code(const std::string& name);                 // SDL scancode, -1 if unknown
const char* key_name(int code);                        // "" if unknown
std::string key_label(int code);                       // "Enter", "Left arrow", "X"
bool parse_keys(const std::string& expr, std::vector<int>* codes);
bool valid_keys(const std::string& expr);
std::string describe_keys(const std::string& expr);    // "X / Enter", "Unbound"

constexpr int kKeyStateSize = 512;
class KeyMapper {
  public:
    explicit KeyMapper(const Profile& p);
    int id(const std::string& control) const;                  // -1 if not bound / not a key control
    bool down(int id, const uint8_t* state) const;             // state = SDL_GetKeyboardState (kKeyStateSize entries)
  private:
    std::vector<std::vector<int>> codes_;
    std::map<std::string, int> ids_;
};

/* ---- source expressions ---- */
struct Term {
    bool axis = false;
    int idx = 0;       // button index or axis index
    int sign = 0;      // axes: +1, -1, 0 (= positive, triggers)
    int threshold = 0; // resolved (never 0 for axes)
};
bool parse_expr(const std::string& expr, int default_threshold, std::vector<Term>* out, std::string* err = nullptr);
bool valid_expr(const std::string& expr);
std::string term_name(const Term& t);                  // "a", "leftx-", "lefttrigger"
/* Human text for the Controllers page: "A", "Left stick left", "LT", "X / B / Y"; "Unbound" for a bad expression. */
std::string describe_term(const Term& t);
std::string describe_expr(const std::string& expr);
int button_index(const std::string& name);             // -1 if unknown
int axis_index(const std::string& name);
const char* button_name(int idx);
const char* axis_name(int idx);

/* ---- evaluation (the runners' per-frame work) ---- */
struct PadState {
    bool b[kNumButtons] = {};
    int a[kNumAxes] = {};   // SDL range -32768..32767
};

class Mapper {
  public:
    explicit Mapper(const Profile& p);
    int id(const std::string& control) const;   // -1 if the profile has no such control
    bool down(int id, const PadState& s) const;
    /* Radial deadzone + rescale, then sensitivity: x,y in SDL orientation (down = +y), roughly -1..1.
     * With deadzone 0.08 / sensitivity 1 this is exactly what n64run always did. */
    void stick(int id, const PadState& s, float* x, float* y) const;
  private:
    struct Ctl { std::vector<Term> terms; int ax = -1, ay = -1; float dead = 0.08f, sens = 1.0f; };
    std::vector<Ctl> ctl_;
    std::map<std::string, int> ids_;
};

/* ---- storage ---- */
struct Store {
    std::map<std::string, Profile> profiles;               // user profiles only (the defaults are built in)
    std::map<std::string, std::string> system_default;     // system -> profile name
    std::map<std::string, std::string> game_override;      // game id -> profile name (profile.system says which system)
    std::vector<std::string> port_order;                   // pad keys, first = port 1
    int64_t stamp = 0;                                     // unix seconds of the last edit (newest wins on sync)
};

Profile resolve(const Store& s, const std::string& system, const std::string& game_id = "");
/* The profile an edit should change: for game_id "" the system default, otherwise that game's override. A built-in
 * default is copied first ("<system>-custom" / "game:<id>"), so the original is never modified. */
Profile& edit_profile(Store& s, const std::string& system, const std::string& game_id = "");
/* Controls page calls: */
bool set_mapping(Store& s, const std::string& system, const std::string& game_id, const std::string& control,
                 const std::string& expr, std::string* err = nullptr);
bool set_analog(Store& s, const std::string& system, const std::string& game_id, const std::string& control,
                double deadzone, double sensitivity, std::string* err = nullptr);
bool set_key(Store& s, const std::string& system, const std::string& game_id, const std::string& control,
             const std::string& keys, std::string* err = nullptr);
bool set_rumble(Store& s, const std::string& system, const std::string& game_id, bool on, std::string* err = nullptr);   // n64 only
void reset(Store& s, const std::string& system, const std::string& game_id = "");   // back to the built-in default / the system default
void set_port_order(Store& s, const std::vector<std::string>& pad_keys);

/* "Assign players" (Controllers page): press A on the controller for Player 1, then Player 2, and so on. Pure state,
 * fed with pad keys by the UI (rising edge of A on a connected pad). A pad that already has a port is ignored, and the
 * assignment ends by itself after kPorts presses. The UI ends it on Esc / Done or after a timeout without a press; what
 * was assigned so far is kept, the rest of the pads follow in their current order. */
class PortAssign {
  public:
    static constexpr int kPorts = 4;
    void start(double now);                          // begins an empty assignment
    bool active() const { return active_; }
    bool finished() const { return !active_ || (int)keys_.size() >= kPorts; }
    /* A press by this pad. Returns true if it took the next port. */
    bool press(const std::string& pad_key, double now);
    /* Ends it (Esc / Done / timeout); the assigned ports stay. */
    void end();
    /* True when no press came for `secs` while active. */
    bool timed_out(double now, double secs) const { return active_ && now - last_ >= secs; }
    int next_port() const { return (int)keys_.size() + 1; }   // 1-based port Player N presses for
    const std::vector<std::string>& assigned() const { return keys_; }   // assigned[i] = pad key for port i+1
    /* Pads unplugged during the mode: drop every assigned key not in `present` (ports shift up). */
    void sync(const std::vector<std::string>& present);
    /* Full port order to store: the assigned pads, then the others in `all_keys` order. */
    std::vector<std::string> order(const std::vector<std::string>& all_keys) const;
  private:
    bool active_ = false;
    double last_ = 0;
    std::vector<std::string> keys_;
};

/* Makes keys unique: a repeat of an earlier key gets "#dup<N>" (N = 2,3,..) appended; the first keeps its key, so
 * a single pad's stored key never changes. Two identical controllers (same guid, no serial) get distinct keys. */
std::vector<std::string> uniquify_keys(std::vector<std::string> keys);

/* Pad key = guid, plus "#serial" when SDL reports one. order_pads returns indices into `pads` in port order: listed keys
 * first (exact key, else same guid), then the rest in their original order. Empty order = unchanged. */
std::vector<int> order_pads(const std::vector<std::string>& port_order, const std::vector<std::string>& pad_keys);

std::string to_json(const Store& s);
bool from_json(const std::string& json, Store* out);
std::string profile_to_json(const Profile& p);
bool profile_from_json(const std::string& json, Profile* out);

/* userdata/controls.json and (when vault_dir is not empty) <vault_dir>/controls.json: load takes the newer stamp,
 * save writes both. Both dirs may have a trailing slash. */
bool load_store(const std::string& userdata_dir, const std::string& vault_dir, Store* out);
bool save_store(const Store& s, const std::string& userdata_dir, const std::string& vault_dir);
/* Resolved profile as a standalone file for the runners (--controls FILE). */
bool write_profile_file(const std::string& path, const Profile& p);
bool read_profile_file(const std::string& path, Profile* out);

}  // namespace hubctl
