#pragma once
/* Host-save sharing for online N64 sessions (--share-host-save).
 *
 * The host (slot 0) sends its battery saves (.eep .sra .fla .mpk) to every other PC before the
 * emulator starts, so that every PC boots from the same EEPROM/SRAM/FlashRAM/mempak bytes.
 *
 * Protocol (all messages are Session datagrams, <= kMaxPayload bytes, loss/reorder tolerant):
 *   HDR  host -> all   {u8 type, u8 file, u8 nfiles, u8 namelen, name, u32 size, u64 hash}
 *   DATA host -> all   {u8 type, u8 file, u16 chunk, u32 check, bytes}  chunk = kSaveChunk bytes (last may be short);
 *                      check = low 32 bits of save_hash(bytes): a corrupt chunk is dropped and re-sent
 *   ACK  client -> host {u8 type, u8 slot, u8 file, u8 verified, u16 nchunks, bitmap of received chunks}
 *   GO   host -> all   {u8 type}   sent for kGoGraceMs once every client has verified every file
 *
 * The host re-sends HDR every 50 ms and the chunks any client still lacks (round-robin, bounded
 * per tick). A client that fails the hash clears its bitmap, which makes the host resend the file.
 * The hash is 64-bit FNV-1a over the bytes plus the size: it catches corruption, not tampering.
 *
 * Limits: a file <= 128 KiB (largest N64 save: FlashRAM), total <= 256 KiB, <= 8 files.
 * Only the four battery extensions are shared; anything else in the save dir is ignored. */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace n64run {

constexpr uint8_t kPktSaveHdr = 3;
constexpr uint8_t kPktSaveData = 4;
constexpr uint8_t kPktSaveAck = 5;
constexpr uint8_t kPktSaveGo = 6;

constexpr size_t kSaveChunk = 1000;
constexpr size_t kSaveMaxFile = 128 * 1024;
constexpr size_t kSaveMaxTotal = 256 * 1024;
constexpr size_t kSaveMaxFiles = 8;
constexpr uint32_t kHdrIntervalMs = 50;
constexpr uint32_t kGoGraceMs = 1000;
constexpr int kDataPerTick = 24;

struct SaveFile { std::string name; std::vector<uint8_t> data; };

uint64_t save_hash(const uint8_t* p, size_t n);
bool valid_save_name(const std::string& name);   // plain basename, battery extension, <= 64 chars
bool is_battery_ext(const std::string& name);

/* Reads eligible files from dir (sorted by name, bounded by the limits above). Files that are
 * too large or would exceed the total are listed in *skipped. */
bool load_save_dir(const std::string& dir, std::vector<SaveFile>* out, std::vector<std::string>* skipped);
bool write_save_file(const std::string& dir, const std::string& name, const std::vector<uint8_t>& data);

using SendFn = std::function<void(const std::vector<uint8_t>& datagram)>;

/* Host (slot 0). Feed received datagrams with on_packet(); call tick() regularly. */
class SaveSender {
  public:
    SaveSender(const std::vector<SaveFile>& files, int num_slots, uint32_t now_ms, uint32_t timeout_ms);
    void on_packet(int from, const uint8_t* d, size_t n);
    void tick(uint32_t now_ms, const SendFn& send);
    bool all_verified() const;     // every client verified every file
    bool finished() const;         // all_verified and the GO grace period elapsed
    bool failed() const { return !error_.empty(); }
    const std::string& error() const { return error_; }
    size_t bytes_sent() const { return bytes_sent_; }

  private:
    struct Peer { std::vector<bool> done; std::vector<std::vector<bool>> have; std::vector<uint16_t> nch; };
    std::vector<SaveFile> files_;
    std::vector<int> nchunks_;
    std::vector<Peer> peers_;   // index = slot (slot 0 unused)
    int num_slots_;
    uint32_t start_ms_, timeout_ms_;
    uint32_t last_hdr_ = 0, last_data_ = 0, last_go_ = 0, go_t0_ = 0, now_ = 0;
    bool started_ = false;
    size_t cursor_ = 0;
    size_t bytes_sent_ = 0;
    std::string error_;
};

/* Client (slot >= 1). */
class SaveReceiver {
  public:
    SaveReceiver(int my_slot, int num_slots, const std::string& dir);
    void on_packet(int from, const uint8_t* d, size_t n);
    void tick(uint32_t now_ms, const SendFn& send);
    bool finished() const;         // GO received and every file verified and written
    bool failed() const { return !error_.empty(); }
    const std::string& error() const { return error_; }
    std::vector<std::string> written() const;

  private:
    struct File {
        bool set = false, verified = false;
        std::string name; uint32_t size = 0; uint64_t hash = 0;
        size_t nch = 0, nhave = 0;
        std::vector<uint8_t> buf; std::vector<bool> have;
    };
    int slot_, num_slots_;
    std::string dir_;
    int nfiles_ = -1;
    std::vector<File> files_;
    bool go_ = false;
    uint32_t last_ack_ = 0;
    std::string error_;
};

}  // namespace n64run

namespace hubnet { class Session; }

namespace n64run {
/* Runs the protocol over a started, ready Session (save_sync_net.cpp). host_files is non-null
 * only on slot 0; clients write the verified files into client_dir. False + *err on failure. */
bool run_save_sync(hubnet::Session& s, int my_slot, int num_slots, const std::vector<SaveFile>* host_files,
                   const std::string& client_dir, uint32_t timeout_ms, std::string* err);
}  // namespace n64run
