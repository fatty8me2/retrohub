#pragma once
/* Nintendo Switch / Switch 2 game identification (System::Switch, played through Lichii: src/hub/lichii.h). UI-free, no decryption:
 * RetroHub never has or needs the player's keys to list a game. See docs/EMULATORS.md (Switch).
 *
 * Formats (Lichii build 88 file filter: nro nso nca xci xcz nsp nsz nup kip):
 *   .nsp / .nsz   PFS0 container. Title id from the ticket entry name "<rights id 32 hex>.tik" (rights id = title id + key generation),
 *                 else the "[0100XXXXXXXXXXXX]" tag of the file name. Update / DLC packages (title id low 12 bits != 0x000 / type) are not games.
 *   .xci / .xcz   cartridge image ("HEAD" magic at 0x100). Title id from the file name tag only (the header holds no plaintext title id).
 *   .nro          homebrew ("NRO0" magic at 0x10). Title from the embedded NACP when the ASET section has one, else the file name.
 *   loose dump    a folder holding exefs/main + exefs/main.npdm (+ romfs/) -- how Switch 2 games are dumped today. The game file is
 *                 exefs/main (Lichii loads it as a deconstructed ROM directory). Title id = the NPDM ACI0 program id (plaintext).
 *                 Switch 2 = the NPDM's ACID signature area is 0x300 bytes instead of 0x200 (Lichii program_metadata.cpp).
 *
 * Key: "switch:<title id, 16 lowercase hex>"; no title id -> "switch:file-<sanitized name>". serial = title id upper case ("" when none). */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace hub::nx {

struct Ident {
    uint64_t title_id = 0;    // 0 = unknown
    std::string title;        // from NACP (nro) or the file / folder name with [..] (..) tags stripped; "" = caller falls back to the file name
    bool nx2 = false;         // Switch 2 program (NPDM ACID layout)
    bool loose = false;       // exefs/main dump
    bool update_or_dlc = false;   // not a playable base game: the scanner skips it
    std::string format;       // "nsp" "nsz" "xci" "xcz" "nro" "exefs"
};

/* Pure parsers over a prefix of the file (unit-tested with synthetic headers). Each returns false when the magic is not there. */
bool parse_pfs0(const uint8_t* d, size_t n, Ident* out);          // NSP / NSZ: entry table + string table
bool parse_xci(const uint8_t* d, size_t n, Ident* out);           // "HEAD" at 0x100
bool parse_nro(const uint8_t* d, size_t n, uint64_t file_size, Ident* out);
bool parse_npdm(const uint8_t* d, size_t n, Ident* out);          // "META" magic; ACI0 program id; ACID size -> nx2
/* "[0100ABCD12340000]" anywhere in a name -> title id, 0 when absent. */
uint64_t title_id_from_name(const std::string& name);
/* "Super Game [0100ABCD12340000][v0] (USA)" -> "Super Game". */
std::string clean_title(const std::string& name);
/* Base game = application title id with the low 12 bits zero (0x800 = update, 0x1000+ = DLC). Switch 2 program ids (04xx...) follow the same rule. */
bool is_base_title(uint64_t title_id);

/* File front ends (read only the header bytes they need). path is UTF-8. For a loose dump pass the exefs/main file. */
bool identify_file(const std::string& path, Ident* out);
std::string key_for(uint64_t title_id, const std::string& sanitized_name);
uint64_t title_id_from_key(const std::string& key);   // "switch:0100abcd12340000" -> id, else 0

}  // namespace hub::nx
