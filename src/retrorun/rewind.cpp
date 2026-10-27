#include "rewind.h"

#include <algorithm>
#include <cstring>

namespace rwd {

int auto_seconds(const std::string& s) {
    return (s == "gba" || s == "gb" || s == "gbc" || s == "nes" || s == "snes") ? 20 : 0;
}
int forced_seconds(const std::string& s) {
    int a = auto_seconds(s);
    return a ? a : 5;
}
bool parse_mode(const std::string& s, int* mode) {
    if (s == "auto") *mode = 0;
    else if (s == "on") *mode = 1;
    else if (s == "off") *mode = 2;
    else return false;
    return true;
}

static void put_varint(std::vector<uint8_t>& o, size_t v) {
    while (v >= 0x80) { o.push_back((uint8_t)(v | 0x80)); v >>= 7; }
    o.push_back((uint8_t)v);
}
static size_t get_varint(const uint8_t*& p, const uint8_t* end) {
    size_t v = 0;
    int sh = 0;
    while (p < end) {
        uint8_t b = *p++;
        v |= (size_t)(b & 0x7F) << sh;
        if (!(b & 0x80)) break;
        sh += 7;
    }
    return v;
}

/* ops: [zero words][literal words][literal XOR values...] repeated */
static void encode(const uint64_t* a, const uint64_t* b, size_t nw, std::vector<uint8_t>& out) {
    size_t i = 0;
    while (i < nw) {
        size_t z = i;
        while (z < nw && a[z] == b[z]) z++;
        size_t l = z;
        while (l < nw && a[l] != b[l]) l++;
        put_varint(out, z - i);
        put_varint(out, l - z);
        for (size_t k = z; k < l; k++) {
            uint64_t x = a[k] ^ b[k];
            uint8_t t[8];
            std::memcpy(t, &x, 8);
            out.insert(out.end(), t, t + 8);
        }
        i = l;
    }
}
static void apply(uint64_t* dst, size_t nw, const std::vector<uint8_t>& d) {
    const uint8_t* p = d.data();
    const uint8_t* end = p + d.size();
    size_t i = 0;
    while (p < end && i < nw) {
        i += get_varint(p, end);
        size_t l = get_varint(p, end);
        for (size_t k = 0; k < l && i < nw && p + 8 <= end; k++, i++, p += 8) {
            uint64_t x;
            std::memcpy(&x, p, 8);
            dst[i] ^= x;
        }
    }
}

Ring::Ring(size_t state_bytes, size_t max_frames, size_t max_bytes)
    : state_bytes_(state_bytes), words_((state_bytes + 7) / 8), max_frames_(std::max<size_t>(1, max_frames)), max_bytes_(max_bytes) {
    cur_.assign(words_, 0);
}

size_t Ring::bytes() const { return delta_bytes_ + (has_cur_ ? words_ * 8 : 0); }

void Ring::clear() {
    entries_.clear();
    delta_bytes_ = 0;
    has_cur_ = false;
}

void Ring::evict() {
    while (!entries_.empty() && (entries_.size() + 1 > max_frames_ || bytes() > max_bytes_)) {
        delta_bytes_ -= entries_.front().size();
        entries_.pop_front();
    }
}

void Ring::push(const uint8_t* state) {
    std::vector<uint64_t> nw(words_, 0);
    std::memcpy(nw.data(), state, state_bytes_);
    if (has_cur_) {
        std::vector<uint8_t> d;
        encode(cur_.data(), nw.data(), words_, d);
        d.shrink_to_fit();
        delta_bytes_ += d.size();
        entries_.push_back(std::move(d));
    }
    cur_.swap(nw);
    has_cur_ = true;
    evict();
}

bool Ring::step_back(std::vector<uint8_t>* out) {
    if (!has_cur_ || entries_.empty()) return false;
    apply(cur_.data(), words_, entries_.back());
    delta_bytes_ -= entries_.back().size();
    entries_.pop_back();
    out->resize(state_bytes_);
    std::memcpy(out->data(), cur_.data(), state_bytes_);
    return true;
}

bool Ring::newest(std::vector<uint8_t>* out) const {
    if (!has_cur_) return false;
    out->resize(state_bytes_);
    std::memcpy(out->data(), cur_.data(), state_bytes_);
    return true;
}

}  // namespace rwd
