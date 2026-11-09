#pragma once
/* "Does this game file look cut short?" for Nintendo Switch / Switch 2 dumps. UI-free, no keys, header reads only: it never hashes or reads a
 * whole file. Used only by the game's page (About tab, src/hub/verify_ui.cpp).
 *
 * Structure check only (check_structure): the container's own header says how long the file must be (PFS0 entry table for .nsp / .nsz, the XCI
 * header's valid-data end, the NUP superblock's file size). A file shorter than that was cut short (an interrupted copy or download).
 * That is proof, so it is the only thing that raises a warning. There is deliberately NO "typical size of this game" rule: file sizes differ
 * between versions and dumpers, and a file whose header matches its real size is complete.
 * .xcz is skipped (compressed: the header size does not apply). NUP: Lichii's loader (core/file_sys/nup_container.cpp) refuses a NUP
 * whose superblock size differs from the real size, and nx.cpp already refuses it too, so a cut-short NUP never reaches the library; the
 * check is repeated here only so the answer is the same wherever this module is used. The per-entry extents of a NUP are NOT checked
 * (they sit in a TOC that can be tens of MB). */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hub::dumpcheck {

enum class Verdict { Ok, Truncated };

struct Result {
    Verdict v = Verdict::Ok;
    uint64_t have = 0;       // size of the file on disk
    uint64_t expected = 0;   // Truncated: the size the header says a complete copy has; 0 when Ok
    std::string reason;      // plain words for the player; "" when Ok
};

/* Reads len bytes at off (fewer at the end of the file); false on an I/O error. Same shape as nx::ReadFn. */
using ReadFn = std::function<bool(uint64_t off, size_t len, std::vector<uint8_t>* out)>;

/* Structure check over header reads only. Short, odd and malformed headers are Ok ("can't tell"), never a crash. */
Result check_structure(const ReadFn& read, uint64_t file_size, const std::string& ext);

/* The structure check on a real file (path is UTF-8). Cheap: a few small reads. Missing or unreadable file = Ok (the library handles that). */
Result check_file(const std::string& path, uint64_t title_id);   // title_id is not used by the check; kept so callers and the cache key stay the same

/* "23.30 GB" (binary gigabytes, the unit file explorers show). */
std::string format_gb(uint64_t bytes);

/* The warning sentence(s) for the game page; "" for Ok. */
std::string warning_text(const Result& r);
/* ---- cache: one background check per file, keyed by path + size + modified time ---- */
class Cache {
public:
    explicit Cache(int recheck_ms = 30000);   // recheck_ms: how often size / time are re-read (tests pass 0)
    /* Non-blocking. false = not known yet (a worker thread was started if none is running). true = *out holds the last answer. The file's
     * size / time are re-read at most every recheck_ms (default 30 s), on the worker, and the check only re-runs when one of them changed. */
    bool get(const std::string& path, uint64_t title_id, Result* out);
    /* Test aid: wait for idle workers. */
    void wait_idle();
    int runs() const;   // how many times check_file really ran (tests)
    struct State;   // public so the worker function can name it
private:
    std::shared_ptr<State> s_;
};

}  // namespace hub::dumpcheck
