#pragma once
/* LRU bookkeeping for uploaded cover textures (pure logic, no SDL). hubart keeps one entry per Ready texture;
 * touch() on every draw lookup, tick() once per frame, victims() lists what to destroy.
 * Only entries idle for at least `min_idle` frames are eligible, so nothing drawn this frame (or last frame) is evicted. */
#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace hubart {

constexpr size_t kMaxCoverTextures = 400;   // cap on live cover textures (each ~ up to a few hundred KB of VRAM)
constexpr uint64_t kMinIdleFrames = 2;      // never evict a texture drawn in the current or previous frame

class Lru {
public:
    void tick() { ++frame_; }
    uint64_t frame() const { return frame_; }
    void touch(const std::string& id) { last_[id] = frame_; }
    void remove(const std::string& id) { last_.erase(id); }
    size_t size() const { return last_.size(); }
    /* Ids to evict so that at most `cap` remain; oldest first; never touched-this-frame entries (see kMinIdleFrames).
     * May return fewer than needed when too many entries are in use. The caller destroys them and calls remove(). */
    std::vector<std::string> victims(size_t cap, uint64_t min_idle = kMinIdleFrames) const {
        std::vector<std::string> out;
        if (last_.size() <= cap) return out;
        std::vector<std::pair<uint64_t, std::string>> cand;
        for (auto& kv : last_)
            if (frame_ >= kv.second && frame_ - kv.second >= min_idle) cand.push_back({kv.second, kv.first});
        std::sort(cand.begin(), cand.end());
        size_t need = last_.size() - cap;
        for (size_t i = 0; i < cand.size() && i < need; ++i) out.push_back(cand[i].second);
        return out;
    }
private:
    uint64_t frame_ = 0;
    std::unordered_map<std::string, uint64_t> last_;
};

} // namespace hubart
