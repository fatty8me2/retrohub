// Small LRU cache for GPU textures that are queued into an ImGui draw list and only rendered at the end of the frame.
// Rules that make that safe:
//  - an entry touched (found or inserted) in the current frame is never evicted;
//  - evicted or replaced entries go to a graveyard and are destroyed at the START of the next frame (begin_frame()),
//    after the previous frame's RenderDrawData has finished, so a handle that is already in a draw list stays valid;
//  - the capacity is soft within a frame (everything touched this frame is kept) and enforced again at every begin_frame().
// Pure logic: V is the cached value (it holds the texture handle), destroy is injected, so it is unit-testable offline.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace hubtex {

template <class V>
class TexCache {
public:
    using Destroy = std::function<void(V&)>;

    TexCache(size_t capacity, Destroy destroy) : cap_(capacity), destroy_(std::move(destroy)) {}
    // No destructor work on purpose: process-wide caches outlive SDL, so call destroy_all() explicitly when the renderer goes away.
    TexCache(const TexCache&) = delete;
    TexCache& operator=(const TexCache&) = delete;

    /* Call once per hub loop iteration, before anything is drawn. */
    void begin_frame() {
        ++frame_;
        for (auto& v : grave_) destroy_(v);
        grave_.clear();
        evict_to_cap();
    }

    /* Existing entry (marked used this frame) or nullptr. */
    V* find(const std::string& key) {
        auto it = map_.find(key);
        if (it == map_.end()) return nullptr;
        touch_entry(it->second);
        return &it->second.v;
    }

    void touch(const std::string& key) { find(key); }

    /* Store v under key (a previous value for the key is buried, not destroyed now). Returns the stored value. */
    V* insert(const std::string& key, V v) {
        auto it = map_.find(key);
        if (it != map_.end()) { grave_.push_back(std::move(it->second.v)); map_.erase(it); }
        Entry& e = map_[key];
        e.v = std::move(v);
        touch_entry(e);
        evict_to_cap();
        return &e.v;
    }

    /* Drop an entry (deferred destruction). */
    void erase(const std::string& key) {
        auto it = map_.find(key);
        if (it == map_.end()) return;
        grave_.push_back(std::move(it->second.v));
        map_.erase(it);
    }

    /* Everything out, deferred (safe to call mid-frame). */
    void clear_deferred() {
        for (auto& kv : map_) grave_.push_back(std::move(kv.second.v));
        map_.clear();
    }

    /* Destroy everything right now (only when nothing can still be queued for drawing). */
    void destroy_all() {
        for (auto& v : grave_) destroy_(v);
        grave_.clear();
        for (auto& kv : map_) destroy_(kv.second.v);
        map_.clear();
    }

    size_t size() const { return map_.size(); }
    size_t graveyard_size() const { return grave_.size(); }
    size_t capacity() const { return cap_; }
    uint64_t frame() const { return frame_; }

private:
    struct Entry { V v{}; uint64_t used_frame = 0, tick = 0; };

    void touch_entry(Entry& e) { e.used_frame = frame_; e.tick = ++clock_; }

    void evict_to_cap() {
        while (map_.size() > cap_) {
            auto victim = map_.end();
            for (auto it = map_.begin(); it != map_.end(); ++it) {
                if (it->second.used_frame == frame_) continue;   // drawn (or about to be) this frame
                if (victim == map_.end() || it->second.tick < victim->second.tick) victim = it;
            }
            if (victim == map_.end()) break;   // everything is in use this frame: stay over capacity until the next frame
            grave_.push_back(std::move(victim->second.v));
            map_.erase(victim);
        }
    }

    size_t cap_;
    Destroy destroy_;
    std::map<std::string, Entry> map_;
    std::vector<V> grave_;
    uint64_t frame_ = 1, clock_ = 0;
};

/* Per-frame decode budget: spend() is true for the first `limit` calls in a frame. */
class DecodeBudget {
public:
    explicit DecodeBudget(int limit) : limit_(limit) {}
    void begin_frame() { used_ = 0; }
    bool spend() { if (used_ >= limit_) return false; ++used_; return true; }
    int used() const { return used_; }
private:
    int limit_, used_ = 0;
};

}  // namespace hubtex
