#pragma once
/* Shared PlayStation disc reading (PS1 and PS2): a sector reader over .iso, raw .bin (2352), .cso and .chd (CD and DVD, via libchdr),
 * the ISO9660 SYSTEM.CNF reader, and the boot serial ("BOOT2 = cdrom0:\SLUS_209.46;1" PS2, "BOOT = cdrom:\SCUS_941.63;1" PS1).
 * No hub dependencies. See docs/PS2.md. */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace hub { namespace disc {

/* "SLUS_209.46" / "slus-20946" / "SLUS20946" -> "SLUS-20946"; "" when it is not a serial. */
std::string normalize_serial(const std::string& s);
/* First serial-looking token in a file name ("GTA SA [SLUS-20946].chd"), "" if none. */
std::string serial_from_filename(const std::string& name);

enum class Cnf { None, Ps2, Ps1 };
/* SYSTEM.CNF text: BOOT2 = PS2, BOOT = PS1. *serial gets the normalized serial of the boot file. */
Cnf parse_system_cnf(const std::string& text, std::string* serial);

/* Reads one 2048-byte user-data sector. */
using SectorFn = std::function<bool(uint32_t lba, uint8_t* out2048)>;
/* ISO9660: the root directory's SYSTEM.CNF. */
bool read_system_cnf(const SectorFn& read, std::string* text);
/* Sector reader over a buffer holding the start of a 2048-byte-sector image. */
SectorFn buffer_reader(std::shared_ptr<std::string> data);

/* A disc image on disk: iso, raw (2352-byte sectors, .bin), cso, chd (CD: 2448-byte frames, DVD: 2048-byte units). */
class Disc {
  public:
    Disc();
    ~Disc();
    bool open(const std::string& path_utf8);                       // by extension; false if unreadable or not that format
    /* A CHD stored (not compressed) inside another file, e.g. a "stored" zip entry: bytes [base, base+size) of the file. */
    bool open_chd_slice(const std::string& path_utf8, uint64_t base, uint64_t size);
    SectorFn reader();
  private:
    struct Impl;
    static bool chd_finish(Impl& d);
    std::shared_ptr<Impl> p_;
};

/* First FILE "name" of a .cue sheet, "" if none. */
std::string cue_first_file(const std::string& cue_text);

struct Info {
    Cnf kind = Cnf::None;
    std::string serial;   // normalized boot serial ("" if SYSTEM.CNF named none)
};
/* .iso .bin .cso .chd, or a .cue (its first FILE). false when the image or its SYSTEM.CNF cannot be read. */
bool probe_file(const std::string& path_utf8, Info* out);
bool probe_buffer(std::shared_ptr<std::string> data, Info* out);

}} // namespace hub::disc
