#pragma once
/* Mid-match player drop ("guest crashed, the others play on"): the pure agreement logic shared by n64run lockstep,
 * retrorun lockstep and retrorun rollback.
 *
 * Problem: a PC vanishes without a goodbye. Every survivor holds a slightly different amount of that PC's input (the last
 * packets reached some survivors but not others). To keep playing deterministically, every survivor must treat the dropped
 * port as neutral (all-zero input) from the SAME input index (the "cutoff") on, and must have the real input of every
 * index below the cutoff.
 *
 * Protocol (per dropped slot):
 *  - a survivor that notices the silence (or hears a peer's message about it) computes its proposal = the first index
 *    of the dropped slot's input it does NOT have (newest known + 1), and stops accepting anything from that slot.
 *  - survivors broadcast {proposal, heard mask, the last kWindow indices of the dropped inputs below `hi`} where hi is
 *    their proposal, or the cutoff once decided. The window is how a survivor that missed the dead PC's last packets gets
 *    them from one that did not.
 *  - cutoff = max of all survivors' proposals (so every survivor can obtain the inputs below it); it is decided once a
 *    survivor holds a proposal from every survivor. Messages repeat until every survivor's heard mask covers all survivors
 *    ("settled"), so a lost message only costs latency.
 *  - a survivor that dies while the agreement is open is removed; the cutoff is the max over the remaining proposals, and
 *    because windows go up to the cutoff already decided, nobody ever lacks data below a cutoff another survivor used.
 * This header has no emulator, SDL or network dependency (unit tested in test_dropagree.cpp). */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace netdrop {

constexpr int kWindow = 32;           // indices of input per port that travel with a message
constexpr int kMaxPorts = 4;

struct PortWin { uint8_t port = 0; std::vector<uint64_t> vals; };   // vals[i] = input index first + i

struct Msg {
    uint8_t from = 0, dead = 0;
    uint32_t proposal = 0;
    uint8_t heard = 0;            // survivors whose proposal `from` holds
    uint32_t first = 0;           // input index of vals[0] (the same for every port)
    std::vector<PortWin> ports;
};

/* {type, from, dead, u32 proposal, u8 heard, u32 first, u8 nports, nports x {u8 port, u8 cnt, cnt x u64}} (little endian host order) */
inline size_t encode(uint8_t type, const Msg& m, uint8_t* out, size_t cap) {
    size_t need = 1 + 1 + 1 + 4 + 1 + 4 + 1;
    for (auto& p : m.ports) need += 2 + p.vals.size() * 8;
    if (need > cap) return 0;
    uint8_t* o = out;
    *o++ = type; *o++ = m.from; *o++ = m.dead;
    std::memcpy(o, &m.proposal, 4); o += 4;
    *o++ = m.heard;
    std::memcpy(o, &m.first, 4); o += 4;
    *o++ = (uint8_t)m.ports.size();
    for (auto& p : m.ports) {
        *o++ = p.port; *o++ = (uint8_t)p.vals.size();
        for (uint64_t v : p.vals) { std::memcpy(o, &v, 8); o += 8; }
    }
    return (size_t)(o - out);
}

inline bool decode(uint8_t type, const uint8_t* d, size_t n, Msg* m) {
    if (n < 14 || d[0] != type) return false;
    m->from = d[1]; m->dead = d[2];
    std::memcpy(&m->proposal, d + 3, 4);
    m->heard = d[7];
    std::memcpy(&m->first, d + 8, 4);
    size_t np = d[12], o = 13;
    if (np > (size_t)kMaxPorts) return false;
    m->ports.clear();
    for (size_t i = 0; i < np; i++) {
        if (o + 2 > n) return false;
        PortWin pw;
        pw.port = d[o]; size_t cnt = d[o + 1]; o += 2;
        if (o + cnt * 8 > n) return false;
        pw.vals.resize(cnt);
        for (size_t k = 0; k < cnt; k++) { std::memcpy(&pw.vals[k], d + o, 8); o += 8; }
        m->ports.push_back(std::move(pw));
    }
    return true;
}

/* The agreement state for one dropped slot, seen from one survivor. */
class Agreement {
  public:
    Agreement() = default;
    /* survivors = every slot still playing, including me and excluding `dead`. */
    Agreement(int dead, uint32_t survivors, int me, uint32_t my_proposal) : dead_(dead), survivors_(survivors), me_(me) {
        have_ = 1u << me;
        prop_[me] = my_proposal;
        heard_[me] = (uint8_t)have_;
    }
    int dead() const { return dead_; }
    int me() const { return me_; }
    uint32_t survivors() const { return survivors_; }
    uint32_t my_proposal() const { return prop_[me_]; }

    void on_msg(int from, uint32_t proposal, uint8_t heard) {
        if (from < 0 || from >= 8 || from == me_ || !(survivors_ >> from & 1u)) return;
        if (!(have_ >> from & 1u)) { have_ |= 1u << from; prop_[from] = proposal; }
        heard_[from] = heard;
    }
    void remove_survivor(int s) {
        if (s == me_ || s < 0 || s >= 8) return;
        survivors_ &= ~(1u << s);
    }
    uint8_t heard_mask() const { return (uint8_t)(have_ & survivors_); }
    bool decided() const { return (have_ & survivors_) == survivors_; }
    /* First input index of the dead slot that is neutral. Valid once decided(). */
    uint32_t cutoff() const {
        uint32_t c = 0;
        for (int s = 0; s < 8; s++) if (survivors_ >> s & 1u) c = c > prop_[s] ? c : prop_[s];
        return c;
    }
    /* Window upper bound (exclusive) for outgoing messages. */
    uint32_t window_hi() const { return decided() ? cutoff() : prop_[me_]; }
    /* Everybody holds everybody's proposal: the messages can stop. */
    bool settled() const {
        if (!decided()) return false;
        for (int s = 0; s < 8; s++)
            if ((survivors_ >> s & 1u) && s != me_ && (heard_[s] & survivors_) != survivors_) return false;
        return true;
    }

  private:
    int dead_ = -1, me_ = 0;
    uint32_t survivors_ = 0, have_ = 0;
    uint32_t prop_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t heard_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
};

}  // namespace netdrop
