#pragma once
/* Link-cable save exchange for online Game Boy link sessions (retrorun --link).
 *
 * Every PC runs ALL consoles of the link in one deterministic simulation, so every PC needs every player's
 * battery save before frame 0. Each participating slot broadcasts its own save blob to all others (all-to-all, unlike
 * n64run's host-only save sync) and everybody loads blob(s) into console instance (port owned by s). A PC only ever
 * writes back its own console's save afterwards.
 *
 * Loss/reorder tolerant over Session datagrams (<= hubnet::kMaxPayload):
 *   DATA   owner -> all  {u8 type=0x41, u8 owner, u16 chunk, u16 nchunks, u32 size, u64 hash, bytes}
 *   STATUS any   -> all  {u8 type=0x42, u8 slot, u8 mask}   mask = owners whose blob this PC has verified
 * Done when this PC holds every blob and every other PC reports holding every blob (then a short grace period during
 * which STATUS keeps going out so the others see it too). Hash = FNV-1a over bytes + size (corruption, not tampering). */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hubnet { class Session; }

namespace rlink {

constexpr uint8_t kPktLinkData = 0x41;
constexpr uint8_t kPktLinkStatus = 0x42;
constexpr size_t kChunk = 1000;
constexpr size_t kMaxBlob = 512 * 1024;
constexpr uint32_t kIntervalMs = 50;
constexpr uint32_t kGraceMs = 700;
constexpr int kDataPerTick = 4;

uint64_t blob_hash(const uint8_t* p, size_t n);

using SendFn = std::function<void(const std::vector<uint8_t>& datagram)>;

class LinkExchange {
  public:
    /* slot_mask: participating PCs (bit s = slot s, slots < 8). `mine` is this PC's blob (may be empty). */
    LinkExchange(int my_slot, uint32_t slot_mask, const std::vector<uint8_t>& mine);
    void on_packet(int from, const uint8_t* d, size_t n);
    void tick(uint32_t now_ms, const SendFn& send);
    bool complete() const;                    // holds every blob and every peer reports the same
    bool finished(uint32_t now_ms) const;     // complete for kGraceMs
    bool have(int slot) const { return slot >= 0 && slot < 8 && s_[slot].verified; }
    uint8_t have_bits() const { return have_mask(); }
    uint8_t peer_bits(int slot) const { return peer_mask_[slot & 7]; }
    const std::vector<uint8_t>& blob(int slot) const { return s_[slot].buf; }

  private:
    struct Owner {
        bool known = false, verified = false;
        uint32_t size = 0;
        uint64_t hash = 0;
        uint16_t nch = 0;
        std::vector<bool> have;
        std::vector<uint8_t> buf;
        size_t nhave = 0;
    };
    int me_;
    uint32_t mask_;
    Owner s_[8];
    uint8_t peer_mask_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t have_mask() const;
    uint32_t last_tick_ = 0, complete_since_ = 0;
    bool was_complete_ = false;
    size_t cursor_ = 0;
};

/* Blocking driver over a ready Session. False + *err on timeout/failure. pump is called every loop turn. */
bool run_link_exchange(hubnet::Session& s, int my_slot, uint32_t slot_mask, const std::vector<uint8_t>& mine, uint32_t timeout_ms,
                       std::vector<std::vector<uint8_t>>* out_blobs /* index = slot, size 8 */, const std::function<void()>& pump,
                       std::string* err);

}  // namespace rlink
