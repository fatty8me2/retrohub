#pragma once
/* "Friend invited you" / "came online" Windows toasts.
 *
 * RETIRED WHILE A GAME RUNS (Plan 4, SO-F14; owner decision): a Windows toast is a banner above whatever is in front, and these
 * toasts only ever existed for the in-game case. Even with "Notify me about invites while I'm playing" ticked, nothing is shown while
 * a game runs: invites and "came online" go to the bell (and the runner's own OSD where one exists). The rule lives in ONE place,
 * should_toast() (the pure policy, popup_guard.h's may_windows_toast), and is enforced a second time inside the system notifier
 * itself (set_game_probe), so no call site can bypass it.
 *
 * Pure parts (XML builder, activation-argument parsing, the show/skip policy) live in toast.cpp (library hublogic) and
 * are unit-tested. The real notifier (toast_win.cpp, C++/WinRT ToastNotificationManager) is only linked into RetroHub;
 * tests inject a mock INotifier so no real notification is ever shown. Toasts never take focus.
 *
 * Clicking "Join" activates the protocol link retrohub://join/<CODE>: Windows starts RetroHub.exe with that URL, the
 * second launch hands the code to the running instance over the single-instance pipe (invite.h). */
#include <cstdint>
#include <string>

namespace hubtoast {

std::string xml_escape(const std::string& s);
/* Toast XML: "<friend> invited you to <game>" with a Join button (protocol activation). "" for an invalid code. */
std::string build_invite_xml(const std::string& friend_name, const std::string& game_title, const std::string& code);
/* Extracts the room code from an activation argument string (retrohub://join/<CODE>); false if it is anything else. */
bool parse_activation_code(const std::string& args, std::string* code);
/* Toast XML: "<friend> came online" (no button; clicking just brings RetroHub up). */
std::string build_online_xml(const std::string& friend_name);

struct INotifier {
    virtual ~INotifier() = default;
    virtual bool available() const = 0;                         // false in dev builds (AUMID not registered)
    virtual bool show(const std::string& toast_xml, const std::string& tag) = 0;
};

/* THE gate for every Windows toast. These toasts are for the player who is in a game, and that is exactly when the house rule forbids
 * one (popup_guard.h may_windows_toast), so today this is false whatever the setting says. Kept as the single place to change. */
bool should_toast(bool setting_on, bool game_running, bool notifier_available);
/* Shows the invite toast when the policy allows. Returns true if a toast was handed to the notifier. */
bool notify_invite(INotifier& n, bool setting_on, bool game_running, const std::string& friend_name,
                   const std::string& game_title, const std::string& code);

/* "<friend> came online" toast, same policy as the invite toast (only while a game runs, honours the in-game notification setting). */
bool notify_online(INotifier& n, bool setting_on, bool game_running, const std::string& friend_name);

/* Game night reminders for when RetroHub is CLOSED (Plan 4, social-F15): a Windows *scheduled* toast that Windows itself delivers.
 * It is only pending while RetroHub is not running (RetroHub removes it shortly before it would fire and shows the reminder in the
 * bell / runner OSD instead), and it is never scheduled while a game or session is up, so it cannot land over a game. Tests inject a mock IScheduler: no real toast is created. */
std::string build_gamenight_xml(const std::string& game_title, const std::string& host_name, const std::string& night_id);
struct IScheduler {
    virtual ~IScheduler() = default;
    virtual bool available() const = 0;                          // false in dev builds (AUMID not registered)
    /* Replaces any scheduled toast with the same tag/group, then schedules this one for fire_unix_ms (must be in the future). */
    virtual bool schedule(const std::string& toast_xml, const std::string& tag, const std::string& group, int64_t fire_unix_ms) = 0;
    /* Removes the scheduled toast with that tag/group. Returns true when it is gone (also when it never existed). */
    virtual bool cancel(const std::string& tag, const std::string& group) = 0;
};
/* The real scheduler (toast_win.cpp, same AUMID and Start-menu shortcut gate as system_notifier()). */
IScheduler& system_scheduler();

/* The real notifier (toast_win.cpp). Only usable when shell_registration_allowed() (the Start-menu shortcut that
 * carries the AUMID exists); otherwise available() is false and show() does nothing. */
INotifier& system_notifier();
/* RetroHub.exe registers hub::game_active here; the system notifier then refuses to show anything while it returns true. */
void set_game_probe(bool (*fn)());
/* Windows only (toast_win.cpp): inits a WinRT apartment on a worker thread and loads the XML; shows nothing. */
bool toast_winrt_selftest(const std::string& xml, std::string* err);

}  // namespace hubtoast
