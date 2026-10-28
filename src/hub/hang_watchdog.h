#pragma once
#include <string>

/* UI hang watchdog (hang_watchdog.cpp). Call start() on the UI thread, tick() once per frame. When no tick arrives for
 * threshold_s seconds the UI thread's symbolized stack goes to <logs_dir>\hang-<time>.txt and a minidump to .dmp (newest 5 kept). */
void hang_watchdog_start(const std::string& logs_dir_utf8, double threshold_s = 5.0);
void hang_watchdog_tick();
void hang_watchdog_phase(const char* where);   // breadcrumb (string literal) written into the report
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
