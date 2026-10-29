#pragma once
#include <string>

/* UI hang watchdog (hang_watchdog.cpp). Call start() on the UI thread, tick() once per frame. When no tick arrives for
 * threshold_s seconds a report goes to <logs_dir>\hang-<time>.txt (newest 5 kept): the breadcrumb phase the UI thread is
 * stuck in, how long it has been there, and the phases before it. Cooperative only: no other thread is suspended or dumped. */
void hang_watchdog_start(const std::string& logs_dir_utf8, double threshold_s = 5.0);
void hang_watchdog_tick();
void hang_watchdog_phase(const char* where);   // breadcrumb (string literal, UI thread) written into the report
const char* hang_watchdog_current_phase();
void hang_watchdog_detail(int d);              // extra context for the report (the page index)
void hang_watchdog_stop();
void hang_watchdog_pause_push();
void hang_watchdog_pause_pop();

/* Hold around a modal or deliberately blocking UI-thread call (file dialogs, message boxes, ...): not reported as a hang. */
struct HangWatchdogPause {
    HangWatchdogPause() { hang_watchdog_pause_push(); }
    ~HangWatchdogPause() { hang_watchdog_pause_pop(); }
    HangWatchdogPause(const HangWatchdogPause&) = delete;
    HangWatchdogPause& operator=(const HangWatchdogPause&) = delete;
};

/* Scoped breadcrumb for a nested section: restores the enclosing phase on exit. */
struct HangPhase {
    const char* prev;
    explicit HangPhase(const char* where) : prev(hang_watchdog_current_phase()) { hang_watchdog_phase(where); }
    ~HangPhase() { hang_watchdog_phase(prev); }
    HangPhase(const HangPhase&) = delete;
    HangPhase& operator=(const HangPhase&) = delete;
};
