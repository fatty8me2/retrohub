#pragma once
/* friend_pick_ui.h - "send a file to a friend" pieces shared by the screenshot viewer and the hack page (Plan 4 p4-social).
 *  FriendPicker: a small controller-friendly modal that lets you tick up to 8 friends.
 *  attach_send_*: uploads a file with hub::attach and, when it is up, sends one DM per friend that carries the attachment. */
#include "app.h"

#include <set>

namespace hubui {

class FriendPicker {
  public:
    /* Ask for friends; draw(id) must be called every frame at the same ImGui ID level as open() was. */
    void open(const std::string& title, const std::string& hint, int max_friends = 8);
    /* Draws the modal. Returns true on the frame the player presses Send; chosen() then holds the uids (1..max). */
    bool draw(const char* id);
    const std::vector<std::string>& chosen() const { return chosen_; }
    bool is_open() const { return open_; }
  private:
    bool want_open_ = false, open_ = false, focus_ = false;
    std::string title_, hint_;
    int max_ = 8;
    std::set<std::string> sel_;
    std::vector<std::string> chosen_;
};

/* Starts the upload of `path` (kind shot|state|patch) for these friends and, once done, DMs each with `text` + the attachment.
 * Returns false (after showing the reason) when it cannot start: file missing, too big, no friends. */
bool attach_send_start(const std::string& path, const std::string& kind, const std::string& display_name, const std::vector<std::string>& uids,
                       const std::string& text);
void attach_send_tick();                // once per frame (friends_ui fr_tick): collects finished uploads, sends the DMs, shows the result
std::string attach_send_status();       // "Sending... 40%" while an upload runs, else ""

/* A small cached texture of a PNG (shot_viewer.cpp), for chat thumbnails. Null when unreadable. */
SDL_Texture* shot_thumb_tex(const std::string& path, int* w, int* h);

} // namespace hubui
