#pragma once
/* The order/dedup rules of the background description queue (gameinfo_ui.cpp), as a plain container so a console test can pin them:
 *  - urgent entries (what the cases view shows, an open details window) go to the FRONT, newest urgent first; the rest keep arrival order;
 *  - a name is queued at most once: a repeat is ignored, an urgent repeat of a waiting background entry moves it to the front;
 *  - a name that was already handled this session is not queued again (prefetch is called every frame); forget() / requeue() reopen it
 *    after a network failure.
 * Not thread-safe: the caller holds its own mutex. */

#include <deque>
#include <set>
#include <string>
#include <utility>

namespace gameinfo {

template <class T>
class PrefetchQueue {
public:
    struct Entry { std::string name; T payload; bool urgent = false; };

    /* true when the entry is now waiting in the queue (new, or moved to the front); false when ignored. */
    bool add(const std::string& name, T payload, bool urgent) {
        if (done_.count(name)) return false;   // handled this session (urgent or not): the result is on disk, not looked up again
        if (waiting_.count(name)) {
            if (!urgent) return false;
            for (auto it = q_.begin(); it != q_.end(); ++it)
                if (it->name == name) {
                    if (it->urgent) return false;   // already urgent: keep its place
                    q_.erase(it);
                    break;
                }
        }
        waiting_.insert(name);
        done_.erase(name);
        Entry e{name, std::move(payload), urgent};
        if (urgent) q_.push_front(std::move(e)); else q_.push_back(std::move(e));
        return true;
    }
    bool empty() const { return q_.empty(); }
    size_t size() const { return q_.size(); }
    bool front_urgent() const { return !q_.empty() && q_.front().urgent; }
    Entry pop() {
        Entry e = std::move(q_.front());
        q_.pop_front();
        waiting_.erase(e.name);
        done_.insert(e.name);
        return e;
    }
    /* A handled entry that must be tried again later (offline): back of the queue, no longer "done". */
    void requeue(Entry e) {
        done_.erase(e.name);
        if (waiting_.insert(e.name).second) q_.push_back(std::move(e));
    }
    bool handled(const std::string& name) const { return done_.count(name) != 0; }
    void forget(const std::string& name) { done_.erase(name); }

private:
    std::deque<Entry> q_;
    std::set<std::string> waiting_, done_;
};

}  // namespace gameinfo
