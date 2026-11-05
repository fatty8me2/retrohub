#pragma once
/* "Verify my games" (Plan 4 Wave 3, L3): is a cartridge dump one of the known-good dumps in the No-Intro lists?
 *
 *   KnownDb    data/known_dumps.bin (tools/gen_known_dumps.py): CRC32 + size + title per entry for NES, SNES, N64, GB, GBC, GBA, NDS.
 *              Derived from libretro-database (CC-BY-SA-4.0), see docs/VERIFY.md.
 *   hash_*     No-Intro style hashing: N64 is converted to z64 byte order, an iNES header and a SNES copier header are skipped.
 *   HashCache  results by path + size + mtime (so a second run reads nothing).
 *   Runner     background thread; hashes only when the player opens "Verify my games", never during a normal scan.
 * Disc systems (PS1, PS2, GC, Wii U, 3DS, PSP) cannot be hashed cheaply: supported() is false and the UI says "not checked". */
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "library.h"

namespace hub::verify {

bool supported(System s);   // NES, SNES, N64, GB, GBC, GBA, NDS

uint32_t crc32_update(uint32_t crc, const uint8_t* d, size_t n);

/* ---- known-good list ---- */
class KnownDb {
public:
    /* text = the decompressed list: "@<system>\t<count>" section lines, "<CRC32 hex>\t<size>\t<title>" rows, '#' comments. */
    bool load_text(const std::string& text);
    /* whole file: one "RHKD1 ..." header line, then a zlib stream of that text. */
    bool load_blob(const std::string& bytes);
    bool load_file(const std::string& path);
    /* The title of the known dump with this CRC and size, or nullptr. GB and GBC also look in each other's list (dual-mode carts). */
    const std::string* find(System s, uint32_t crc, uint64_t size) const;
    size_t count(System s) const;
    size_t total() const;
    bool loaded() const { return total() > 0; }
    const std::string& header() const { return header_; }   // the "RHKD1 ..." line (license, source version)
private:
    const std::string* find_in(System s, uint32_t crc, uint64_t size) const;
    std::map<System, std::unordered_map<uint64_t, std::string>> m_;
    std::string header_;
};

/* The shared list, loaded on first use from <data dir>/known_dumps.bin (default: next to the exe in data\). */
void set_data_dir(const std::string& dir);
const KnownDb& known_db();

/* ---- hashing ---- */
struct FileHash {
    uint32_t crc = 0;
    uint64_t size = 0;   // bytes hashed: after z64 conversion and header removal (an N64 .v64 keeps its size; an iNES file loses 16)
};
/* read(buf, n) returns the bytes read (0 at the end). `total` is the stream's full length. false when stop is set or total == 0. */
using ReadFn = std::function<size_t(uint8_t*, size_t)>;
bool hash_stream(System s, uint64_t total, const ReadFn& read, FileHash* out, const std::atomic<bool>* stop = nullptr);
bool hash_file(System s, const std::string& path, FileHash* out, const std::atomic<bool>* stop = nullptr);

/* ---- cache ---- */
class HashCache {
public:
    bool get(const std::string& path, System s, uint64_t size, long long mtime, FileHash* out) const;
    void put(const std::string& path, System s, uint64_t size, long long mtime, const FileHash& h);
    bool load(const std::string& file);
    bool save(const std::string& file) const;
    size_t size() const { return m_.size(); }
private:
    struct Rec { std::string sys; uint64_t size = 0; long long mtime = 0; FileHash h; };
    std::unordered_map<std::string, Rec> m_;   // lowercase path
};

/* ---- background check ---- */
enum class Verdict { Good, Unknown, Unreadable };
struct Result {
    Verdict v = Verdict::Unknown;
    std::string title;   // the known dump's title when Good
    uint32_t crc = 0;
};
struct Item {
    std::string key, path;
    System system = System::Unknown;
};
/* Verdict for one hash: Good when the list has it. */
Result judge(const KnownDb& db, System s, const FileHash& h);

class Runner {
public:
    ~Runner() { stop_and_join(); }
    /* Items whose system is not supported() are skipped. cache_file "" = no persistent cache. The db must outlive the run.
     * cache_only: answer from the cache only (path + size + mtime unchanged) and never read a ROM: files the cache does not know get no result. */
    void start(std::vector<Item> items, const KnownDb* db, const std::string& cache_file, bool cache_only = false);
    void cancel() { cancel_ = true; }
    void stop_and_join() { cancel_ = true; if (th_.joinable()) th_.join(); }
    bool running() const { return running_; }
    int done() const { return done_; }
    int total() const { return total_; }
    bool get(const std::string& key, Result* out) const;
    std::map<std::string, Result> snapshot() const;
private:
    void run(std::vector<Item> items, const KnownDb* db, std::string cache_file, bool cache_only);
    std::thread th_;
    std::atomic<bool> running_{false}, cancel_{false};
    std::atomic<int> done_{0}, total_{0};
    mutable std::mutex mu_;
    std::map<std::string, Result> res_;
};

}  // namespace hub::verify
