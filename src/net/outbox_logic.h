#pragma once
/* Offline outbox for friends actions (SO-F4). Pure logic, no I/O: friends.cpp persists items to <data_dir>/outbox.json and sends
 * `frame` (the exact wire JSON) in order after the next welcome. Nothing is ever dropped silently: every refusal or expiry is returned
 * to the caller, which reports it as Event::SendFailed. */
#include <cstdint>
#include <string>
#include <vector>

namespace hubnet {

struct OutItem {
    std::string kind;       // add_friend | accept | decline | cancel | remove_friend | block | unblock | message | invite
    std::string uid;        // the other person (empty for add_friend and invite)
    std::string frame;      // the wire JSON to send
    std::string local_id;   // messages: the conversation line id ("local-...") shown as pending until sent
    std::string text;       // human label for failure texts (message text, friend code, game title)
    int64_t at = 0;         // unix ms when queued
};

constexpr size_t kOutboxMax = 50;
constexpr int64_t kOutboxMaxAgeMs = 24LL * 3600 * 1000;

enum class OutAdd { Queued, Merged, Cancelled, Full };

class Outbox {
  public:
    const std::vector<OutItem>& items() const { return v_; }
    size_t size() const { return v_.size(); }
    bool empty() const { return v_.empty(); }
    void set(std::vector<OutItem> items) { v_ = std::move(items); }
    void clear() { v_.clear(); }

    /* Adds `it` at the back. Dedupe: block then unblock (or unblock then block) of one person cancel out (Cancelled: both removed);
     * accept/decline/cancel/remove_friend/block on one person replace an earlier action of that group (Merged: the later wins);
     * an identical add_friend frame is not queued twice (Merged). messages and invites are never merged. Full when kOutboxMax items
     * wait (the new item is not stored). */
    OutAdd add(const OutItem& it) {
        if (it.kind == "block" || it.kind == "unblock") {
            for (size_t i = 0; i < v_.size(); i++)
                if (v_[i].uid == it.uid && (v_[i].kind == "block" || v_[i].kind == "unblock")) {
                    if (v_[i].kind != it.kind) { v_.erase(v_.begin() + i); return OutAdd::Cancelled; }
                    return OutAdd::Merged;
                }
        }
        if (it.kind == "accept" || it.kind == "decline" || it.kind == "cancel" || it.kind == "remove_friend") {
            for (size_t i = 0; i < v_.size(); i++)
                if (v_[i].uid == it.uid && is_resolution(v_[i].kind)) { v_[i] = it; return OutAdd::Merged; }
        }
        if (it.kind == "add_friend")
            for (const OutItem& x : v_) if (x.kind == "add_friend" && x.frame == it.frame) return OutAdd::Merged;
        if (v_.size() >= kOutboxMax) return OutAdd::Full;
        v_.push_back(it);
        return OutAdd::Queued;
    }
    /* Removes and returns items older than kOutboxMaxAgeMs. */
    std::vector<OutItem> expire(int64_t now_unix_ms) {
        std::vector<OutItem> gone, keep;
        for (auto& x : v_) (now_unix_ms - x.at > kOutboxMaxAgeMs ? gone : keep).push_back(x);
        v_ = std::move(keep);
        return gone;
    }
    /* Takes everything in order (the flush after a welcome). */
    std::vector<OutItem> take_all() { std::vector<OutItem> o; o.swap(v_); return o; }
    /* The server answered an error for a queued message/invite sent by flush: the caller keeps `sent` (the batch it just sent). */
    bool has_message(const std::string& local_id) const {
        for (const OutItem& x : v_) if (x.kind == "message" && x.local_id == local_id) return true;
        return false;
    }
    /* Removes one pending message by local id (the user cancelled it). */
    bool remove_message(const std::string& local_id) {
        for (size_t i = 0; i < v_.size(); i++) if (v_[i].kind == "message" && v_[i].local_id == local_id) { v_.erase(v_.begin() + i); return true; }
        return false;
    }
    /* "queued - will send when you're back online" style label for the UI. */
    static std::string queued_text(const std::string& kind) {
        if (kind == "message") return "Queued - will send when you're back online.";
        if (kind == "invite") return "Invite queued - will send when you're back online.";
        return "Saved - will be done when you're back online.";
    }
    static std::string failed_text(const OutItem& x, const std::string& why) {
        std::string what = x.kind == "message" ? "Your message" : x.kind == "invite" ? "Your invite" : "That change";
        return what + " could not be sent (" + why + ").";
    }
  private:
    static bool is_resolution(const std::string& k) { return k == "accept" || k == "decline" || k == "cancel" || k == "remove_friend"; }
    std::vector<OutItem> v_;
};

} // namespace hubnet
