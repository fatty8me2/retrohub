#pragma once
/* Game night reminders while RetroHub is closed (Plan 4, social-F15). Pure decisions + a sync loop over an injected scheduler, unit
 * tested in test_gamenight.cpp with a mock (no real toast is ever created).
 *
 * When a night is planned or received, a Windows SCHEDULED toast is registered for 10 minutes before the start. Windows delivers it by
 * itself, so it shows even when RetroHub is closed. House rule: no Windows toast over a running game. While RetroHub runs it owns the
 * reminder (bell / runner OSD), so the scheduled toast is removed shortly before it would fire (kToastGuardMs). RetroHub can close while
 * a game it started keeps running, so NOTHING is ever scheduled (not at exit either) while a game or session is up (game_up): that
 * night's Windows reminder is simply skipped. On a normal exit with no game up the hub re-registers the toasts it had pulled (for_exit).
 * Cancelling, declining (dismissing) or the night passing removes the toast, and with the Windows-notification setting off nothing is
 * scheduled. */
#include <string>

#include "gamenight_logic.h"
#include "toast.h"

namespace hubui {
namespace night {

constexpr int64_t kToastGuardMs = 2LL * 60 * 1000;     // while RetroHub runs: pull the toast this long before it would fire
constexpr int64_t kToastMinLeadMs = 15LL * 1000;       // Windows needs a delivery time in the future
constexpr const char* kToastGroup = "gamenight";

inline int64_t toast_fire_ms(const Night& n) { return n.start_ms - kReminderMs; }

/* Short stable tag from the night id (scheduled-toast tags are length limited). */
inline std::string toast_tag(const Night& n) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : n.id) { h ^= c; h *= 1099511628211ULL; }
    static const char hex[] = "0123456789abcdef";
    std::string t = "gn";
    for (int i = 60; i >= 0; i -= 4) t += hex[(h >> i) & 15];
    return t;
}

enum class ToastAct { None, Schedule, Remove };

/* `for_exit` = RetroHub is closing, so nothing needs pulling any more and the guard window no longer applies. */
inline ToastAct toast_decide(const Night& n, int64_t now, bool setting_on, bool scheduler_ok, bool for_exit, bool game_up = false) {
    const int64_t lead = toast_fire_ms(n) - now;
    const bool wanted = setting_on && scheduler_ok && !game_up && !n.dismissed && !n.reminded && phase_of(n, now) != Phase::Over &&
                        lead >= (for_exit ? kToastMinLeadMs : kToastGuardMs);
    if (n.toast_sched) return wanted ? ToastAct::None : ToastAct::Remove;
    return wanted ? ToastAct::Schedule : ToastAct::None;
}

struct SyncResult {
    bool changed = false;   // a toast_sched flag changed: persist the plans
    bool failed = false;    // a scheduler call failed: retry later, not every frame
    bool remove_failed = false;   // a removal failed: retry soon (a stale toast must not fire over a game)
};

inline SyncResult sync_toasts(Plans& p, int64_t now, bool setting_on, hubtoast::IScheduler& s, bool for_exit = false, bool game_up = false) {
    SyncResult r;
    const bool ok = s.available();
    for (auto& n : p.v) {
        if (!ok) {   // nothing can be pending without a scheduler (dev build / no shortcut): just forget the flag
            if (n.toast_sched) { n.toast_sched = false; r.changed = true; }
            continue;
        }
        switch (toast_decide(n, now, setting_on, ok, for_exit, game_up)) {
            case ToastAct::Schedule:
                if (s.schedule(hubtoast::build_gamenight_xml(n.game_title, n.mine ? std::string() : n.host_name, n.id), toast_tag(n),
                               kToastGroup, toast_fire_ms(n)))
                    { n.toast_sched = true; r.changed = true; }
                else r.failed = true;
                break;
            case ToastAct::Remove:
                if (s.cancel(toast_tag(n), kToastGroup)) { n.toast_sched = false; r.changed = true; }
                else { r.failed = true; r.remove_failed = true; }   // keep the flag: it is retried
                break;
            case ToastAct::None: break;
        }
    }
    return r;
}

}  // namespace night
}  // namespace hubui
