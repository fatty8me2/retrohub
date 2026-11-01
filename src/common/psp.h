#pragma once
/* PlayStation Portable game identification (pure, no hub dependencies): PARAM.SFO -> DISC_ID / TITLE / CATEGORY.
 * Sources: an ISO9660 image (PSP_GAME/PARAM.SFO) as .iso, .cso (CISO v1) or .chd (read through disc_id.h), and the
 * PARAM.SFO embedded in an EBOOT .pbp. A PS1 "PSone Classic" eboot is a .pbp too: it stays PlayStation 1 (category "ME" or a
 * DATA.PSAR starting "PSISOIMG"/"PSTITLEIMG"). See docs/EMULATORS.md (PSP). */

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "disc_id.h"

namespace hub { namespace psp {

/* PARAM.SFO: key -> text value (UTF-8 formats) and key -> number (int32 format). */
struct Sfo {
    bool ok = false;
    std::map<std::string, std::string> str;
    std::map<std::string, uint32_t> num;
    std::string get(const std::string& key) const { auto i = str.find(key); return i == str.end() ? std::string() : i->second; }
};
bool parse_sfo(const uint8_t* data, size_t n, Sfo* out);

/* "ULUS10041", "NPUZ00001", "ulus-10041" -> "ULUS10041" (4 letters + 5 digits, a '-' or '_' between is dropped); "" when it is not a disc id. */
std::string normalize_disc_id(const std::string& s);

struct Ident {
    std::string disc_id;    // "ULUS10041" ("" for homebrew without a DISC_ID)
    std::string title;      // TITLE of the SFO
    std::string category;   // "UG" UMD game, "MG" memory stick game / homebrew, "EG" PSN, "ME" PS1 eboot ...
    bool ps1 = false;       // a PlayStation 1 eboot: not ours
};

/* .iso .cso .ciso .chd .pbp. False when the file is not a PSP game (a PS1 eboot is false with ident->ps1 = true). */
bool identify_file(const std::string& path_utf8, Ident* out);
/* Identity from the start of a file held in memory (archive members): an ISO prefix, a CSO prefix, or a PBP prefix. */
bool identify_buffer(std::shared_ptr<std::string> data, Ident* out);
/* The PARAM.SFO of a PBP (offsets from its header) and the PS1-vs-PSP decision. `psar_head` = first bytes of DATA.PSAR when known. */
bool identify_pbp(const uint8_t* head, size_t n, const uint8_t* psar_head, size_t psar_n, Ident* out);
/* ISO9660 reader: PSP_GAME/PARAM.SFO of the image behind `read`. */
bool identify_reader(const disc::SectorFn& read, Ident* out);
/* Sector reader over the start of a CSO held in memory (blocks beyond the buffer fail to read). Empty function when not a CISO. */
disc::SectorFn cso_buffer_reader(std::shared_ptr<std::string> data);

/* Library key: "psp:<disc id lower>" or "psp:file-<stem>". */
std::string key_for(const std::string& disc_id, const std::string& file_stem_key);
/* "psp:ulus10041" -> "ULUS10041"; "" for file keys and anything else. */
std::string disc_id_from_key(const std::string& key);
/* Names of the SAVEDATA folders (e.g. "ULUS10041DATA00", "ULUS10041_SYSTEM") that belong to the game: the folder name starts with the disc id. */
bool savedata_folder_matches(const std::string& folder, const std::string& disc_id);

}} // namespace hub::psp
