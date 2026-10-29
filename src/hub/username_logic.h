#pragma once
/* Pure logic for the username picker (wizard Name step, Settings > Account, the "pick a new name" prompt). No UI, no SDL.
 * The picker types a name, the Checker debounces and tracks the live availability answer, the wizard rules say what the
 * primary button does. Unit tested in test_username.cpp. Naming rules live in net/username.h. */
#include "username.h"

#include <cctype>
#include <cstdio>

#include <string>
#include <vector>

namespace hubui {
namespace un {

enum class Avail {
    Empty,        // nothing typed
    Invalid,      // fails the local rules (problem text says why); never sent to the server
    Typing,       // valid locally, waiting for the debounce
    Checking,     // request in flight
    Available,    // free to claim
    Yours,        // already this account's name
    Taken,
    NotAllowed,   // the server refused it (reserved / blocked word)
    Offline,      // cannot reach the server: claim later
};

constexpr double kDebounceSecs = 0.45;

struct Checker {
    std::string text;                      // what is in the box
    Avail state = Avail::Empty;
    hubnet::NameCheck local = hubnet::NameCheck::Empty;
    double changed_at = 0;
    int ref = 0;                           // the in-flight request
    std::string answered_for;              // key the current answer belongs to
    std::vector<std::string> suggestions;  // from the server, or local when taken
    std::string mine_key;                  // my current username's fold key (hubnet::username_fold_key, "" = none yet): typing it again is "yours"

    void set_input(const std::string& t, double now)
    {
        if (hubnet::username_key(t) == hubnet::username_key(text) && state != Avail::Empty) { text = t; return; }   // case-only edit: keep the answer
        text = t;
        changed_at = now;
        ref = 0;
        suggestions.clear();
        local = hubnet::username_validate(t);
        if (local == hubnet::NameCheck::Empty) state = Avail::Empty;
        else if (local == hubnet::NameCheck::Reserved) { state = Avail::NotAllowed; suggestions = hubnet::username_suggestions(t); }
        else if (local != hubnet::NameCheck::Ok) state = Avail::Invalid;
        else if (!mine_key.empty() && hubnet::username_fold_key(t) == mine_key) state = Avail::Yours;   // mine_key is the FOLD key: "Kai Smith" is "kai.smith"
        else state = Avail::Typing;
    }
    /* True when a check should be sent now (and moves to Checking). `connected` false => Offline, nothing sent. */
    bool want_send(double now, bool connected)
    {
        if (state != Avail::Typing || now - changed_at < kDebounceSecs) return false;
        if (!connected) { state = Avail::Offline; return false; }
        state = Avail::Checking;
        return true;
    }
    void sent(int r) { if (r > 0) ref = r; else state = Avail::Offline; }
    /* An answer arrived. Stale ones (another ref, or the text changed) are ignored. Returns true when applied. */
    bool on_result(int r, bool available, const std::string& reason, const std::vector<std::string>& sugg, bool yours = false)
    {
        if (state != Avail::Checking || r != ref) return false;
        suggestions = sugg;
        if (available) state = yours ? Avail::Yours : Avail::Available;
        else if (reason == "rate-limited") state = Avail::Offline;   // too many checks: ask again in a moment
        else if (reason == "taken" || reason == "held" || reason == "username-taken") state = Avail::Taken;   // held = reserved for its previous owner
        else state = Avail::NotAllowed;   // reserved / profanity / looks-like-code / invalid-chars / ...
        if (!available && suggestions.empty()) suggestions = hubnet::username_suggestions(text);
        return true;
    }
    void on_connection_lost() { if (state == Avail::Checking || state == Avail::Typing) state = Avail::Offline; }
    /* Back online while showing Offline: re-check. */
    void on_connection_back(double now) { if (state == Avail::Offline && local == hubnet::NameCheck::Ok) { state = Avail::Typing; changed_at = now - kDebounceSecs - 0.01; } }
    std::string trimmed() const { return hubnet::username_trim(text); }
};

/* Line shown beside the box. The UI draws a check mark icon before it for Available/Yours. */
inline std::string status_text(const Checker& c)
{
    switch (c.state) {
        case Avail::Empty: return "Pick a name friends will know you by.";
        case Avail::Invalid: return hubnet::username_problem_text(c.local);
        case Avail::Typing: return "";
        case Avail::Checking: return "Checking...";
        case Avail::Available: return "available";
        case Avail::Yours: return "this is your username";
        case Avail::Taken: return "taken";
        case Avail::NotAllowed: return "not allowed";
        case Avail::Offline: return "Can't check right now. You can keep going and claim it when you're back online.";
    }
    return "";
}
enum class Tone { Neutral, Good, Bad, Warn };
inline Tone status_tone(Avail a)
{
    switch (a) {
        case Avail::Available: case Avail::Yours: return Tone::Good;
        case Avail::Taken: case Avail::NotAllowed: case Avail::Invalid: return Tone::Bad;
        case Avail::Offline: return Tone::Warn;
        default: return Tone::Neutral;
    }
}
inline bool shows_suggestions(Avail a) { return a == Avail::Taken || a == Avail::NotAllowed; }

/* ---- what pressing the primary button does on the wizard's name step ---- */
enum class NameAction { Disabled, Claim, Continue, ContinueOffline };
/* claimed = this PC already has a confirmed username. claiming = a claim is in flight. */
inline NameAction name_action(Avail a, bool claimed, bool claiming)
{
    if (claiming) return NameAction::Disabled;
    if (a == Avail::Yours) return NameAction::Continue;
    if (a == Avail::Available) return NameAction::Claim;
    if (a == Avail::Offline) return NameAction::ContinueOffline;   // valid locally, cannot reach the server: claim later (banner)
    if (claimed && (a == Avail::Empty || a == Avail::Typing || a == Avail::Checking)) return NameAction::Continue;   // came back to the step after claiming
    return NameAction::Disabled;
}
inline std::string name_action_label(NameAction a, bool claiming)
{
    if (claiming) return "Claiming...";
    switch (a) {
        case NameAction::Claim: return "Claim username";
        case NameAction::ContinueOffline: return "Continue (claim later)";
        default: return "Next";
    }
}

/* Banner while the account has no confirmed username (skipped offline) or the server flagged it. "" = no banner. */
inline std::string banner_text(bool known_claimed, bool needs_new_name, bool online)
{
    if (needs_new_name) return "RetroHub now uses unique usernames. Pick yours so friends can find you.";
    if (!known_claimed) return online ? "Pick your username so friends can find you and your saves follow you."
                                      : "You're offline: claim your username when you're back online so nobody else takes it.";
    return "";
}

/* One-time prompt: shown once per run for needs_new_name accounts until they pick (or choose "Later"). */
inline bool should_prompt(bool needs_new_name, bool already_prompted_this_run) { return needs_new_name && !already_prompted_this_run; }

/* "Change username" explanation (the server holds the old name for you so nobody can grab it the moment you leave it). */
inline std::string hold_rule_text()
{
    return "When you change your username, your old one stays reserved for you for " + std::to_string(hubnet::kUsernameHoldDays) +
           " days, so nobody else can take it. You can switch back any time in that period. Your friends, saves and cloud backup move with you.";
}
inline std::string claim_error_text(const std::string& reason)
{
    if (reason == "taken" || reason == "username-taken") return "Someone just took that name. Try another.";
    if (reason == "held") return "That name is reserved for its previous owner for a few days. Try another.";
    if (reason == "rate-limited") return "You changed your username a lot lately. Try again later.";
    if (reason == "reserved" || reason == "profanity" || reason == "looks-like-code" || reason == "invalid-chars" || reason == "too-short" ||
        reason == "too-long" || reason == "bad-shape" || reason == "username-reserved" || reason == "username-invalid") return "That name is not allowed. Try another.";
    return "Could not claim that name right now. Try again.";
}

/* Link-code countdown "9:05". */
inline std::string countdown_text(double secs_left)
{
    int s = (int)(secs_left + 0.999);
    if (s < 0) s = 0;
    char b[16];
    std::snprintf(b, sizeof b, "%d:%02d", s / 60, s % 60);
    return b;
}

/* Returning player: normalised link code ready to send (XXXX-XXXX / 8 chars). Needs 8 letters or digits. */
inline bool link_code_complete(const std::string& typed)
{
    int n = 0;
    for (unsigned char c : typed) if (std::isalnum(c)) n++;
    return n == 8 || n == 12;
}

}  // namespace un
}  // namespace hubui
