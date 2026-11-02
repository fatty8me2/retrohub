#pragma once
/* "Friend invited you" Windows toasts while a game is running.
 *
 * Pure parts (XML builder, activation-argument parsing, the show/skip policy) live in toast.cpp (library hublogic) and
 * are unit-tested. The real notifier (toast_win.cpp, C++/WinRT ToastNotificationManager) is only linked into RetroHub;
 * tests inject a mock INotifier so no real notification is ever shown. Toasts never take focus.
 *
 * Clicking "Join" activates the protocol link retrohub://join/<CODE>: Windows starts RetroHub.exe with that URL, the
 * second launch hands the code to the running instance over the single-instance pipe (invite.h), and the existing
 * queued-join logic joins once the game exits. */
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

/* Policy: toast only while a game is running, when the setting is on, and when the notifier can work. */
bool should_toast(bool setting_on, bool game_running, bool notifier_available);
/* Shows the invite toast when the policy allows. Returns true if a toast was handed to the notifier. */
bool notify_invite(INotifier& n, bool setting_on, bool game_running, const std::string& friend_name,
                   const std::string& game_title, const std::string& code);

/* "<friend> came online" toast, same policy as the invite toast (only while a game runs, honours the in-game notification setting). */
bool notify_online(INotifier& n, bool setting_on, bool game_running, const std::string& friend_name);

/* The real notifier (toast_win.cpp). Only usable when shell_registration_allowed() (the Start-menu shortcut that
 * carries the AUMID exists); otherwise available() is false and show() does nothing. */
INotifier& system_notifier();
/* Windows only (toast_win.cpp): inits a WinRT apartment on a worker thread and loads the XML; shows nothing. */
bool toast_winrt_selftest(const std::string& xml, std::string* err);

}  // namespace hubtoast
