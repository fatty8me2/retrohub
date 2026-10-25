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

enum class Format { GameShark, ActionReplay, CodeBreaker, GameGenie, Raw };
const char* format_name(Format f);                       // "gameshark" | "ar" | "codebreaker" | "gamegenie" | "raw"
bool parse_format(const std::string& s, Format* out);    // case-insensitive; also "actionreplay", "cb", "gg", "gs"

/* Which emulator family consumes the codes. */
enum class Target { N64, GBA, GB, DS, PS1 };
/* "n64" / "gba" / "gb" / "gbc" / "nds" / "ps1" (case-insensitive) -> Target. False for other systems. */
bool target_for_system(const std::string& system, Target* out);

struct Cheat {
    std::string id;
    std::string desc;
    Format format = Format::Raw;
    std::vector<std::string> codes;
    bool enabled = false;
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

/* ---- N64 (mupen64plus core cheat API) ---- */
struct N64Code {
    uint32_t address;   // full GameShark word, type byte included (m64p_cheat_code.address)
    int value;          // low 16 bits
};
/* Convert one cheat. Supported types: 50 (repeater), 80/81 (8/16-bit write), 88/89 (write while the GS button is held),
 * A0/A1 (uncached write), D0..D3 (conditional), F0/F1 (write at boot). Anything else makes the whole cheat unusable
 * (skipping one line of a conditional would change what the rest means): returns false and explains in *err. */
bool n64_codes(const Cheat& c, std::vector<N64Code>* out, std::string* err);
/* True when a cheat contains an 88/89 code (only fires while the GameShark button is held; the runners do not map it). */
bool n64_needs_gs_button(const std::vector<N64Code>& codes);

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
