#pragma once
/* Pure header parsers for the Nintendo 3DS (Azahar) and Wii U (Cemu) libraries.
 * No file I/O of their own: callers pass the bytes (or a read callback), so the same code
 * serves plain files, zip entries and unit tests. */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hub {

/* ---------------- Nintendo 3DS ---------------- */

struct N3dsInfo {
    std::string container;     // "ncsd" (.3ds/.cci), "ncch" (.cxi), "cia", "3dsx"
    uint64_t title_id = 0;     // 0 when unknown (3dsx)
    std::string product_code;  // NCCH product code, e.g. "CTR-P-EKJA" ("" for cia/3dsx)
    std::string serial;        // 4-char game code ("EKJA") for ncsd/ncch; 8 hex of the title id low word for cia; "" for 3dsx
    bool decrypted = false;    // NCCH "NoCrypto" flag set: Azahar can load it without any AES keys
    bool is_application = false;  // title id high word 0x00040000 (a game, not an update/DLC/system title)
};

/* Bytes of a .3ds/.cci file needed by parse_3ds_header (partition 0 normally starts at 0x4000). */
constexpr size_t kN3dsHeaderBytes = 0x4400;

/* NCSD image (.3ds/.cci), bare NCCH (.cxi), CIA (needs ~0x4000 bytes: header+cert+ticket+tmd) or 3DSX.
 * Detects the container from the magic. false when nothing recognized or the buffer is too short. */
bool parse_3ds_header(const uint8_t* d, size_t n, N3dsInfo* out);

/* Stable identity used for saves and library state: "3ds:<title id, 16 lowercase hex>" or "" when no title id. */
std::string n3ds_key(const N3dsInfo& i);

/* ---------------- Wii U ---------------- */

struct WiiuInfo {
    std::string container;     // "wua", "wud", "wux", "rpx"
    uint64_t title_id = 0;     // 0 when unknown (wud/wux without decrypting)
    std::string product_code;  // "WUP-P-ALZE" for wud/wux
    std::string serial;        // 16 hex digits (title id) when known, else the 4-char code, else ""
};

using ReadAtFn = std::function<bool(uint64_t offset, void* buf, size_t n)>;

/* .wua = ZArchive. Reads the footer and the root directory only (a few KB even for a 5 GB file) and
 * returns the base game's title id (high word 00050000), else the first title directory found. */
bool parse_wua(const ReadAtFn& read, uint64_t file_size, WiiuInfo* out);
/* Title id from a loadiine folder's meta/meta.xml (<title_id ...>0005000010101d00</title_id>). */
bool parse_meta_xml_title_id(const std::string& xml, uint64_t* title_id);
/* .wud starts with the disc id text ("WUP-P-ALZE"). */
bool parse_wud_header(const uint8_t* d, size_t n, WiiuInfo* out);
/* .wux: 0x20-byte header, sector index table, deduplicated sectors; reads sector 0 for the disc id. */
bool parse_wux(const ReadAtFn& read, uint64_t file_size, WiiuInfo* out);

std::string hex16(uint64_t v);  // 16 lowercase hex digits
std::string wiiu_key(const WiiuInfo& i);  // "wiiu:<title id hex16>" or "wiiu:wup-<code>" ("" when neither)

/* Zipped 3DS images: Azahar cannot open .zip (it reads .3ds/.cci/.cia/.cxi/.3dsx and its own
 * compressed .zcci/.zcia/.zcxi/.z3dsx). The library lists them and shows "needs extraction". */
bool is_3ds_extension(const std::string& lower_ext_with_dot);       // .3ds .cci .cia .cxi .3dsx and the z-variants
bool is_3ds_zip_entry(const std::string& lower_ext_with_dot);       // .3ds .cci .cia .cxi .3dsx

} // namespace hub
