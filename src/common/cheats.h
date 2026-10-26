#pragma once
/* Shared cheat model: the hub writes a cheats file (JSON), the runners (n64run, retrorun) read it.
 *
 *   {"version":1,"cheats":[{"id":"...","desc":"Unlock all characters","format":"gameshark|ar|codebreaker|gamegenie|raw",
 *                           "codes":["8033B21E 0008", ...],"enabled":true}]}
 *
 * Runners get `--cheats <path>` and watch the file's mtime (about twice a second) to apply enable/disable
 * live. Online sessions never take live changes: the file is read once at launch and every peer must hold
 * the same list (see set_hash(), exchanged in the netplay handshake).
 *
 * Pure logic (no emulator, no network): parsing/validation per format and per target system, so it is unit tested. */

#include <cstdint>
#include <string>
#include <vector>

namespace cheats {

enum class Format { GameShark, ActionReplay, CodeBreaker, GameGenie, Raw, ActionReplayGC, Gecko };
const char* format_name(Format f);                       // "gameshark" | "ar" | "codebreaker" | "gamegenie" | "raw" | "ar_gc" | "gecko"
bool parse_format(const std::string& s, Format* out);    // case-insensitive; also "actionreplay", "cb", "gg", "gs"

/* Which emulator family consumes the codes. */
enum class Target { N64, GBA, GB, DS, PS1, GC, NES, SNES };
/* "n64" / "gba" / "gb" / "gbc" / "nds" / "ps1" / "gc" / "nes" / "snes" (case-insensitive) -> Target. False for other systems. */
bool target_for_system(const std::string& system, Target* out);

struct Cheat {
    std::string id;
    std::string desc;
    Format format = Format::Raw;
    std::vector<std::string> codes;
    bool enabled = false;
    int fire = 0;   // N64 GameShark-button codes (88/89): runner fires them once each time this counter rises
};

struct CheatSet {
    int version = 1;
    std::vector<Cheat> cheats;
};

/* ---- JSON ---- */
bool parse_json(const std::string& text, CheatSet* out, std::string* err);
std::string to_json(const CheatSet& set);
bool load_file(const std::string& path, CheatSet* out, std::string* err);
bool save_file(const std::string& path, const CheatSet& set, std::string* err);   // atomic (temp + rename)

/* ---- code validation ---- */
/* Normalize one code line for `target` in `fmt`: upper-case, single separators. False + *err when the line is not a
 * valid code of that format on that system. */
bool normalize_code(Target target, Format fmt, const std::string& line, std::string* out, std::string* err);

/* ---- NES / SNES (libretro retro_cheat_set through retrorun) ----
 * NES: Game Genie 6 or 8 letters from APZLGITYEOXUKSVN ("SXIOPO"), normalized upper-case; Raw "AAAA:VV" (address 0000-FFFF,
 *      value 00-FF) or "AAAA:CC:VV" (with compare byte). SNES: Game Genie "XXXX-XXXX" (hex digits DF4709156BC8A23E),
 *      Pro Action Replay / Raw 8 hex digits "AAAAAAVV" ("7E0DBE05"; a "AAAAAA:VV" pair is accepted). Pure helpers, unit tested. */
bool nes_game_genie_ok(const std::string& letters);   // 6 or 8 letters of the NES alphabet
bool snes_game_genie_ok(const std::string& code);     // XXXX-XXXX (dash optional on input)

/* ---- GameCube (Dolphin) ----
 * "gecko": 8+8 hex lines ("0445BF28 FFFFFFFF"). "ar_gc": Action Replay, either decrypted 8+8 hex or the encrypted
 * "XXXX-XXXX-XXXXX" form (Dolphin decrypts it itself). Both go to User\GameSettings\<GAMEID>.ini, see dolphin_cheats.h. */
bool gc_is_encrypted_ar(const std::string& line);

/* ---- N64 (mupen64plus core cheat API) ---- */
struct N64Code {
    uint32_t address;   // full GameShark word, type byte included (m64p_cheat_code.address)
    int value;          // low 16 bits
};
/* Convert one cheat. Codes may be joined with ';' '+' ',' or newlines. Supported types: 50 (repeater), 80/81 (8/16-bit
 * write), 88/89 (GameShark-button writes: runners fire them once, see n64_split_button), A0/A1 (uncached write),
 * D0..D3 (conditional), F0/F1 (write at boot). 00/DE/EE/FF are device-control words with no emulator effect and are
 * dropped (a cheat made only of those yields an empty list, still true). Anything else (D8/D9..., XXXX placeholders)
 * makes the whole cheat unusable (skipping one line of a conditional would change what the rest means): false + *err. */
bool n64_codes(const Cheat& c, std::vector<N64Code>* out, std::string* err);
/* True when a cheat contains an 88/89 code. */
bool n64_needs_gs_button(const std::vector<N64Code>& codes);
bool n64_is_button_cheat(const Cheat& c);
/* Split into the always-on list (core cheat engine) and the fire-once list (88/89 plus the conditional guarding each). */
void n64_split_button(const std::vector<N64Code>& codes, std::vector<N64Code>* core, std::vector<N64Code>* once);

/* ---- value cheats ----
 * ~6000 database cheats carry placeholder digits ("8001A0E7 XXXX", "01??DCC4", "800A0F34+000?") meaning "type a value".
 * A run of X / x / ? characters in a hex code is one value slot; every run of the cheat takes the same number
 * (a shorter run keeps the low digits). Hints in the description narrow it: "(00-63)", "(0-63 Hexidecimal)",
 * "01=Potion 02=Antidote" lists. The filled-in cheat (value_apply) is an ordinary cheat everything else handles. */
struct ValueOption { uint32_t value; std::string label; };
struct ValueSpec {
    bool valid = false;          // the cheat has a placeholder
    int digits = 0;              // hex digits of the widest slot (1..8)
    uint32_t min = 0, max = 0;   // inclusive, already clamped to the slot and the code type
    bool hinted = false;         // the description gave a range or an option list
    bool prefer_hex = false;     // the description talks in hex ("00-FF", "hexadecimal")
    std::vector<ValueOption> options;   // known meanings (dropdown) when the description lists >= 2
};
/* True when some code line has a placeholder run (and is otherwise hex). */
bool value_has_placeholder(const Cheat& c);
ValueSpec value_spec(const Cheat& c, Target target);
uint32_t value_clamp(const ValueSpec& s, uint32_t v);
/* Replaces every placeholder run with `v` (clamped) as upper-case hex. False when the cheat has no placeholder. */
bool value_apply(const Cheat& c, uint32_t v, Target target, Cheat* out);
/* "99" / "0x63" / "63h": text -> number. dec: plain digits are decimal. False when empty / not a number / overflow. */
bool value_parse(const std::string& text, bool dec, uint32_t* out);
std::string value_format(uint32_t v, bool dec, int digits);

/* ---- libretro (retro_cheat_set) ---- */
/* The string handed to retro_cheat_set for one cheat: normalized lines joined with '+'. False + *err when any line is
 * invalid for `target`. */
bool retro_code_string(Target target, const Cheat& c, std::string* out, std::string* err);

/* ---- netplay ---- */
/* Hash of the effective cheat list: enabled cheats only, normalized codes, order independent. An empty or all-disabled
 * set hashes to kEmptyHash. Peers must agree on it before the session starts. */
constexpr uint64_t kEmptyHash = 0xCBF29CE484222325ull;
uint64_t set_hash(const CheatSet& set, Target target);

/* Cheap change detector for the live file watch. */
class FileWatch {
  public:
    explicit FileWatch(std::string path = std::string()) : path_(std::move(path)) { stamp_ = stamp(); }
    /* True once per modification (mtime or size changed since the previous true/construction). */
    bool changed();
    const std::string& path() const { return path_; }
  private:
    std::string stamp() const;
    std::string path_, stamp_;
};

}  // namespace cheats
