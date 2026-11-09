#pragma once
/* "Does this game file look incomplete?" for Nintendo Switch / Switch 2 dumps. UI-free, no keys, header reads only: it never hashes or reads a
 * whole file. Used by the game's page, the Library health card and the Play note (src/hub/verify_ui.cpp).
 *
 * Two independent checks, worst verdict wins:
 *   1. Structure (check_structure): the container's own header says how long the file must be (PFS0 entry table for .nsp / .nsz, the XCI
 *      header's valid-data end, the NUP superblock's file size). A file shorter than that was cut short (an interrupted copy or download).
 *      .xcz is skipped (compressed: the header size does not apply). NUP: Lichii's loader (core/file_sys/nup_container.cpp) refuses a NUP
 *      whose superblock size differs from the real size, and nx.cpp already refuses it too, so a cut-short NUP never reaches the library; the
 *      check is repeated here only so the answer is the same wherever this module is used. The per-entry extents of a NUP are NOT checked
 *      (they sit in a TOC that can be tens of MB).
 *   2. Known sizes (kKnownSizes): a complete dump of a given title is at least some size. A file of the right structure but smaller than that
 *      is an incomplete or older-version dump. Example: the Ocarina of Time (Switch 2) .nup that renders the Water Temple without walls.
 *
 * Add a game by adding one row to kKnownSizes in dump_check.cpp. */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hub::dumpcheck {

enum class Verdict { Ok, LooksIncomplete, Truncated };

struct Result {
    Verdict v = Verdict::Ok;
    uint64_t have = 0;       // size of the file on disk
    uint64_t expected = 0;   // what a complete copy needs (Truncated: the header's size; LooksIncomplete: the typical complete size); 0 when Ok
    std::string reason;      // plain words for the player; "" when Ok
    std::string area_note;   // LooksIncomplete: what the player may see ("" = none known)
};

struct KnownSize {
    uint64_t title_id;               // Switch program id (nx::Ident::title_id / the library key "switch:<16 hex>")
    const char* name;
    const char* ext;                 // the container the numbers are for (lower case, with the dot)
    uint64_t complete_at_least;      // anything smaller is incomplete or an older version
    uint64_t typical_complete;       // what a good copy measures (shown to the player)
    const char* area_note;           // what is wrong in a short copy
    const char* source;              // where the numbers came from
};
const KnownSize* known_sizes(size_t* count);

/* Table check only. ext is lower case with the dot (".nup"); a title or container not in the table is Ok. */
Result check_size(uint64_t title_id, const std::string& ext, uint64_t file_size);

/* Reads len bytes at off (fewer at the end of the file); false on an I/O error. Same shape as nx::ReadFn. */
using ReadFn = std::function<bool(uint64_t off, size_t len, std::vector<uint8_t>* out)>;

/* Structure check over header reads only. Short, odd and malformed headers are Ok ("can't tell"), never a crash. */
Result check_structure(const ReadFn& read, uint64_t file_size, const std::string& ext);

/* Both checks on a real file (path is UTF-8). Cheap: a few small reads. Missing or unreadable file = Ok (the library handles that). */
Result check_file(const std::string& path, uint64_t title_id);

/* "23.30 GB" (binary gigabytes, the unit file explorers show). */
std::string format_gb(uint64_t bytes);

/* The warning sentence(s) for the game page and the Play note; "" for Ok. */
std::string warning_text(const Result& r);
/* One short line for the Library health card list. */
std::string short_line(const Result& r);

/* ---- "Don't warn about this file again": remembered per path AND size, so a new dump of the same path warns again ---- */
class DismissList {
public:
    bool has(const std::string& path, uint64_t size) const;
    void add(const std::string& path, uint64_t size);
    std::string serialize() const;           // one "<size>\t<path>" per line
    void parse(const std::string& text);     // ignores bad lines
    size_t size() const { return m_.size(); }
private:
    std::map<std::string, uint64_t> m_;      // lower-cased path -> size
};

/* ---- cache: one background check per file, keyed by path + size + modified time ---- */
class Cache {
public:
    Cache();
    /* Non-blocking. false = not known yet (a worker thread was started if none is running). true = *out holds the last answer. The file's
     * size / time are re-read at most every 30 s, on the worker, and the check only re-runs when one of them changed. */
    bool get(const std::string& path, uint64_t title_id, Result* out);
    /* Test aid: wait for idle workers. */
    void wait_idle();
    int runs() const;   // how many times check_file really ran (tests)
    struct State;   // public so the worker function can name it
private:
    std::shared_ptr<State> s_;
};

}  // namespace hub::dumpcheck
