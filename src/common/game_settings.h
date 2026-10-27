#pragma once
/* Per-game settings, stored in the save vault so they follow the user to every PC.
 *
 * Model: a fixed table of options (options()); every option has a string value and a scope:
 *   Visual : purely cosmetic, never leaves this PC (resolution, filtering, layout, fullscreen...).
 *   Sync   : changes emulation results. The HOST's value travels in the launch spec (`"gs":{...}`) and guests apply
 *            it for that session (sync_fragment() / extract_sync() / online_effective()).
 *   Locked : changes emulation results and is not negotiable online: forced to its default for everyone in an
 *            online session (the GLideN64 framebuffer-emulation options).
 * Layers, lowest to highest: option default -> system defaults -> per-game override (Store::resolve()).
 *
 * File: <vault>\_settings\game_settings.json   (Store::load(vault_dir) / save(vault_dir)).
 * Pure logic: no UI, no process handling. The emitters at the bottom turn resolved values into runner arguments:
 *   n64run   : --gfx Key=t:value (GLideN64 config, t = i|b|s)  + --fullscreen
 *   retrorun : --opt key=value (core options), --filter, --integer-scale, --fx, --bezel, --fullscreen
 * Dolphin / Azahar / Cemu config writers live in src/hub/emulators.{h,cpp} (dolphin_gfx_apply etc.). */
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "library.h"

namespace hub {
namespace gs {

enum class Scope { Visual, Sync, Locked };
enum class Kind { Bool, Choice, Text };

/* System bit for Option::systems. */
unsigned sys_bit(System s);

struct Choice { std::string value, label; };

struct Option {
    std::string key;      // "n64.res"; common keys have no prefix ("fullscreen")
    std::string label;    // UI text
    std::string group;    // UI section: "Video", "Color", "Emulation", ...
    unsigned systems = 0; // sys_bit() mask of systems it applies to
    Kind kind = Kind::Choice;
    Scope scope = Scope::Visual;
    std::string def;      // default value ("true"/"false" for Bool)
    std::vector<Choice> choices;   // Kind::Choice only
};

using Values = std::map<std::string, std::string>;

const std::vector<Option>& options();
const Option* find_option(const std::string& key);
std::vector<const Option*> options_for(System s);   // UI: what to show for this system
/* Valid value (Bool -> "true"/"false", Choice must be in the list, Text is trimmed, no control chars); false if not. */
bool normalize(const Option& o, const std::string& value, std::string* out);

/* Option defaults for a system (every applicable option present). */
Values defaults_for(System s);

class Store {
public:
    /* Unknown keys / invalid values are rejected (false). */
    bool set_system(System s, const std::string& key, const std::string& value);
    bool set_game(const std::string& game_key, System s, const std::string& key, const std::string& value);
    void clear_system_key(System s, const std::string& key);
    void clear_game_key(const std::string& game_key, const std::string& key);   // game value goes back to the system default
    void reset_game(const std::string& game_key);   // "reset to defaults" for one game (drops its overrides)
    void reset_system(System s);                    // drops the system defaults (games keep their own overrides)
    void reset_all();
    bool has_game_overrides(const std::string& game_key) const;

    /* Option defaults <- system defaults <- game overrides; only options that apply to `s`. */
    Values resolve(System s, const std::string& game_key) const;
    /* Where a value comes from, for the UI: 0 = option default, 1 = system default, 2 = per-game override. */
    int source(System s, const std::string& game_key, const std::string& key) const;

    std::string to_json() const;
    bool from_json(const std::string& text);   // replaces the contents; false (and unchanged) on malformed input
    /* Merge another PC's file: other's entries win per key (callers decide which is newer). */
    void merge_from(const Store& other);

    bool load(const std::string& vault_dir);   // missing file = empty store, true
    bool save(const std::string& vault_dir) const;
    static std::string file_path(const std::string& vault_dir);

private:
    std::map<std::string, Values> sys_;    // system_short -> overrides
    std::map<std::string, Values> game_;   // game key -> overrides
};

/* ---- online ---------------------------------------------------------------------------------------------- */
/* Only the Sync options of `resolved` (what the host puts in the launch spec). */
Values sync_subset(const Values& resolved);
/* ",\"gs\":{...}" (leading comma) for hubui::launch_json's extra_json parameter. Empty if there is nothing to sync. */
std::string sync_fragment(const Values& resolved);
/* The "gs" object of a launch message (or a bare object); empty if absent/malformed. Invalid entries dropped. */
Values extract_sync(const std::string& launch_json);
/* Values to use in an online session. Locked -> default for everyone. Sync -> the host's value; a guest takes it
 * from `host_sync` (absent key / older host => the option default, so both sides still agree); the host passes its
 * own sync_subset(). Visual values are kept as they are. */
Values online_effective(System s, const Values& local_resolved, const Values& host_sync);

/* ---- runner emitters ------------------------------------------------------------------------------------- */
/* n64run: ["--gfx","UseNativeResolutionFactor=i:2", ...]; AspectRatio is always emitted (4:3 default), other keys only when non-default.
 * Fullscreen is not included (the hub passes --fullscreen / --width / --height itself). */
std::vector<std::string> n64_args(const Values& v);
/* n64run: (section key, typed value "i:2"), for tests and the --gfx parser. */
std::vector<std::pair<std::string, std::string>> n64_gliden_params(const Values& v);
/* retrorun: --opt k=v ..., --filter nearest|linear, --integer-scale 0|1 (not --fullscreen: the hub decides). */
std::vector<std::string> retrorun_args(System s, const Values& v);
bool is_fullscreen(const Values& v);
/* GLideN64 config keys that n64run must ignore in an online session (Locked options). */
bool n64_key_locked_online(const std::string& gliden_key);

/* Parse "Key=t:value" (n64run --gfx). t = i (int), b (bool 0/1), s (string). */
struct GfxParam { std::string key; char type = 's'; std::string sval; int ival = 0; };
bool parse_gfx_arg(const std::string& arg, GfxParam* out);

}  // namespace gs
}  // namespace hub
