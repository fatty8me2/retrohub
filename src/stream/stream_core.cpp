#include "stream_core.h"

#include <algorithm>
#include <cstring>

namespace rplay {

Bytes make_frame_blob(const FrameInfo& fi, const uint8_t* au, size_t len)
{
    Bytes b(kFrameHdr + len);
    put32(&b[0], fi.cap_us); put16(&b[4], fi.w); put16(&b[6], fi.h);
    b[8] = fi.flags; b[9] = fi.codec; put32(&b[10], fi.seq);
    if (len) memcpy(&b[kFrameHdr], au, len);
    return b;
}

bool parse_frame_blob(const Bytes& b, FrameInfo* fi, const uint8_t** au, size_t* len)
{
    if (b.size() < (size_t)kFrameHdr) return false;
    fi->cap_us = get32(&b[0]); fi->w = (uint16_t)get16(&b[4]); fi->h = (uint16_t)get16(&b[6]);
    fi->flags = b[8]; fi->codec = b[9]; fi->seq = get32(&b[10]);
    *au = b.data() + kFrameHdr; *len = b.size() - kFrameHdr;
    return true;
}

static Bytes vhdr(uint8_t flags, uint32_t id, uint16_t idx, uint16_t count, uint16_t extra, size_t payload)
{
    Bytes p(kVideoHdr + payload);
    p[0] = kPktVideo; p[1] = flags; put32(&p[2], id); put16(&p[6], idx); put16(&p[8], count); put16(&p[10], extra);
    return p;
}

std::vector<Bytes> packetize_frame(uint32_t id, bool key, const Bytes& blob, int fec_group)
{
    std::vector<Bytes> out;
    size_t n = std::max<size_t>(1, (blob.size() + kFragData - 1) / kFragData);
    uint8_t kf = key ? kVfKey : 0;
    std::vector<Bytes> par;
    int G = fec_group > 0 ? fec_group : 0;
    if (G) {
        size_t groups = (n + G - 1) / G;
        for (size_t g = 0; g < groups; g++) {
            Bytes p = vhdr(kf | kVfParity, id, (uint16_t)g, (uint16_t)n, (uint16_t)G, kFragData + 2);
            uint16_t xl = 0;
            for (size_t i = g * G; i < std::min(n, (g + 1) * G); i++) {
                size_t off = i * kFragData, l = std::min<size_t>(kFragData, blob.size() - off);
                for (size_t k = 0; k < l; k++) p[kVideoHdr + k] ^= blob[off + k];
                xl ^= (uint16_t)l;
            }
            put16(&p[kVideoHdr + kFragData], xl);
            par.push_back(std::move(p));
        }
    }
    for (size_t i = 0; i < n; i++) {
        size_t off = i * kFragData, l = std::min<size_t>(kFragData, blob.size() - off);
        Bytes p = vhdr(kf, id, (uint16_t)i, (uint16_t)n, 0, l);
        if (l) memcpy(&p[kVideoHdr], &blob[off], l);
        out.push_back(std::move(p));
        if (G && ((i + 1) % G == 0 || i + 1 == n)) out.push_back(std::move(par[i / G]));   // parity right after its group
    }
    return out;
}

/* ---------------- Reassembler ---------------- */

bool Reassembler::try_fec(Pending& p, uint16_t group)
{
    auto it = p.parity.find(group);
    if (it == p.parity.end() || !it->second.have) return false;
    int G = it->second.g;
    if (G <= 0) return false;
    int lo = group * G, hi = std::min<int>(p.count, lo + G), miss = -1;
    for (int i = lo; i < hi; i++)
        if (!p.have[i]) { if (miss >= 0) return false; miss = i; }
    if (miss < 0) return false;
    const Bytes& pd = it->second.data;   // kFragData + 2
    Bytes rec(pd.begin(), pd.begin() + kFragData);
    uint16_t xl = (uint16_t)get16(&pd[kFragData]);
    for (int i = lo; i < hi; i++) {
        if (i == miss) continue;
        const Bytes& f = p.frag[i];
        for (size_t k = 0; k < f.size(); k++) rec[k] ^= f[k];
        xl ^= (uint16_t)f.size();
    }
    if (xl > kFragData) return false;
    rec.resize(xl);
    p.frag[miss] = std::move(rec); p.have[miss] = 1; p.got++;
    recovered_by_fec++;
    return true;
}

void Reassembler::feed(const uint8_t* pkt, int len, uint32_t now)
{
    if (len < kVideoHdr || pkt[0] != kPktVideo) return;
    packets++;
    uint8_t flags = pkt[1]; uint32_t id = get32(pkt + 2);
    uint16_t idx = (uint16_t)get16(pkt + 6), count = (uint16_t)get16(pkt + 8), extra = (uint16_t)get16(pkt + 10);
    if (count == 0 || count > 4096) return;
    if (any_done_ && (int32_t)(id - last_done_) <= 0) { dup_packets++; return; }
    auto it = pend_.find(id);
    if (it == pend_.end()) {
        if (pend_.size() >= 64) { pend_.erase(pend_.begin()); frames_dropped++; need_key = true; }
        Pending np; np.count = count; np.first_ms = now; np.frag.resize(count); np.have.assign(count, 0);
        it = pend_.emplace(id, std::move(np)).first;
    }
    Pending& p = it->second;
    if (p.count != count) return;
    if (flags & kVfKey) p.key = true;
    const uint8_t* pl = pkt + kVideoHdr; int pn = len - kVideoHdr;
    if (flags & kVfParity) {
        if (pn != kFragData + 2) return;
        Parity& pr = p.parity[idx];
        if (!pr.have) { pr.have = true; pr.g = extra; pr.data.assign(pl, pl + pn); }
        try_fec(p, idx);
    } else {
        if (idx >= count || pn > kFragData) return;
        if (p.have[idx]) { dup_packets++; return; }
        p.frag[idx].assign(pl, pl + pn); p.have[idx] = 1; p.got++;
        for (auto& kv : p.parity) if (kv.second.g > 0 && idx / kv.second.g == kv.first) try_fec(p, kv.first);
    }
    flush(now, false);
}

void Reassembler::flush(uint32_t now, bool force_gap)
{
    while (!pend_.empty()) {
        auto f = pend_.begin();
        Pending& q = f->second;
        if (q.got < q.count) break;
        bool gap = any_done_ && f->first != last_done_ + 1;
        if (gap) {
            if (!force_gap && now - q.first_ms < 100) break;   // a missing earlier frame may still show up
            need_key = true; frames_dropped++;
        }
        Bytes blob;
        for (auto& fr : q.frag) blob.insert(blob.end(), fr.begin(), fr.end());
        ready_.push_back({f->first, {q.key, std::move(blob)}});
        last_done_ = f->first; any_done_ = true; frames_done++;
        pend_.erase(f);
    }
}

bool Reassembler::pop(uint32_t* id, Bytes* blob, bool* key)
{
    if (ready_.empty()) return false;
    *id = ready_.front().first; *key = ready_.front().second.first; *blob = std::move(ready_.front().second.second);
    ready_.pop_front();
    return true;
}

std::vector<MissingFrag> Reassembler::nacks(uint32_t now, uint32_t age_ms, uint32_t retry_ms, size_t max_n)
{
    std::vector<MissingFrag> out;
    for (auto& kv : pend_) {
        Pending& p = kv.second;
        if (now - p.first_ms < age_ms) continue;
        for (uint16_t i = 0; i < p.count && out.size() < max_n; i++) {
            if (p.have[i]) continue;
            auto ln = p.last_nack.find(i);
            if (ln != p.last_nack.end() && now - ln->second < retry_ms) continue;
            p.last_nack[i] = now;
            out.push_back({kv.first, i});
        }
    }
    return out;
}

bool Reassembler::expire(uint32_t now, uint32_t giveup)
{
    bool dropped = false;
    while (!pend_.empty()) {
        auto f = pend_.begin();
        if (f->second.got >= f->second.count || now - f->second.first_ms < giveup) break;
        // blocked head frame is hopeless: skip it; later complete frames flush on the next feed()
        last_done_ = f->first; any_done_ = true;
        pend_.erase(f); frames_dropped++; dropped = true; need_key = true;
    }
    flush(now, true);
    return dropped;
}

/* ---------------- NACK ---------------- */

void NackHistory::store(uint32_t id, std::vector<Bytes> pkts)
{
    q_.push_back({id, std::move(pkts)});
    while (q_.size() > max_) q_.pop_front();
}

const Bytes* NackHistory::find(uint32_t id, uint16_t idx) const
{
    for (auto& e : q_) {
        if (e.first != id) continue;
        for (auto& p : e.second)
            if (!(p[1] & kVfParity) && get16(&p[6]) == idx) return &p;
    }
    return nullptr;
}

Bytes make_nack(const std::vector<MissingFrag>& m)
{
    size_t n = std::min<size_t>(m.size(), 64);
    Bytes b(2 + n * 6);
    b[0] = kPktNack; b[1] = (uint8_t)n;
    for (size_t i = 0; i < n; i++) { put32(&b[2 + i * 6], m[i].frame_id); put16(&b[6 + i * 6], m[i].idx); }
    return b;
}

std::vector<MissingFrag> parse_nack(const uint8_t* p, int len)
{
    std::vector<MissingFrag> out;
    if (len < 2 || p[0] != kPktNack) return out;
    int n = p[1];
    if (len < 2 + n * 6) return out;
    for (int i = 0; i < n; i++) out.push_back({get32(p + 2 + i * 6), (uint16_t)get16(p + 6 + i * 6)});
    return out;
}

/* ---------------- audio ---------------- */

Bytes make_audio_pkt(uint8_t codec, uint32_t seq, uint32_t ts, const uint8_t* d, size_t len)
{
    Bytes b(kAudioHdr + len);
    b[0] = kPktAudio; b[1] = codec; put32(&b[2], seq); put32(&b[6], ts);
    if (len) memcpy(&b[kAudioHdr], d, len);
    return b;
}

bool parse_audio_pkt(const uint8_t* p, int len, AudioPkt* o)
{
    if (len < kAudioHdr || p[0] != kPktAudio) return false;
    o->codec = p[1]; o->seq = get32(p + 2); o->ts_ms = get32(p + 6);
    o->data.assign(p + kAudioHdr, p + len);
    return true;
}

void JitterBuffer::push(const AudioPkt& p)
{
    if (!started_) { started_ = true; next_ = p.seq; }
    if ((int32_t)(p.seq - next_) < 0) { late++; return; }
    if (q_.count(p.seq)) { dups++; return; }
    q_[p.seq] = p;
    while ((int)q_.size() > max_) { q_.erase(q_.begin()); overflow++; next_ = q_.begin()->first; }
}

bool JitterBuffer::pop(AudioPkt* out, bool* lost)
{
    *lost = false;
    if (!primed_) {
        if ((int)q_.size() < target_) return false;
        primed_ = true;
        if (!popped_) next_ = q_.begin()->first;
    }
    if (q_.empty()) { primed_ = false; return false; }
    auto it = q_.find(next_);
    if (it != q_.end()) { *out = std::move(it->second); q_.erase(it); next_++; popped_ = true; return true; }
    if ((int)q_.size() >= target_ || q_.begin()->first - next_ > (uint32_t)target_) {
        *lost = true; popped_ = true; lost_total++; out->data.clear(); out->seq = next_; next_++;
        return true;
    }
    return false;
}

/* ---------------- input / stats ---------------- */

Bytes make_input_pkt(uint16_t seq, const PadState& pad)
{
    Bytes b(3 + sizeof(PadState));
    b[0] = kPktInput; put16(&b[1], seq); memcpy(&b[3], &pad, sizeof(PadState));
    return b;
}

bool parse_input_pkt(const uint8_t* p, int len, uint16_t* seq, PadState* pad)
{
    if (len != (int)(3 + sizeof(PadState)) || p[0] != kPktInput) return false;
    *seq = (uint16_t)get16(p + 1); memcpy(pad, p + 3, sizeof(PadState));
    return true;
}

Bytes make_stats_pkt(const StatsReport& s)
{
    Bytes b(10);
    b[0] = kPktStats; put16(&b[1], s.loss_permille); put16(&b[3], s.recv_kbps); put16(&b[5], s.jitter_ms);
    b[7] = s.fps; put16(&b[8], s.rtt_ms);
    return b;
}

bool parse_stats_pkt(const uint8_t* p, int len, StatsReport* s)
{
    if (len != 10 || p[0] != kPktStats) return false;
    s->loss_permille = (uint16_t)get16(p + 1); s->recv_kbps = (uint16_t)get16(p + 3); s->jitter_ms = (uint16_t)get16(p + 5);
    s->fps = p[7]; s->rtt_ms = (uint16_t)get16(p + 8);
    return true;
}

int BitrateController::update(const StatsReport& s)
{
    loss_ = s.loss_permille;
    bool bad = s.loss_permille > 20 || (s.jitter_ms > 20 && s.jitter_ms > last_jitter_ + 15);
    last_jitter_ = s.jitter_ms;
    if (bad) { kbps_ = std::max(min_, kbps_ * 8 / 10); clean_ = 0; }
    else if (++clean_ >= 5) { kbps_ = std::min(max_, kbps_ * 11 / 10); clean_ = 0; }
    return kbps_;
}

} // namespace rplay
