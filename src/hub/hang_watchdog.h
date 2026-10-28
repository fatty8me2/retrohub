#pragma once
#include <string>

/* UI hang watchdog (hang_watchdog.cpp). Call start() on the UI thread, tick() once per frame. When no tick arrives for
 * threshold_s seconds the UI thread's symbolized stack goes to <logs_dir>\hang-<time>.txt and a minidump to .dmp. */
void hang_watchdog_start(const std::string& logs_dir_utf8, double threshold_s = 5.0);
void hang_watchdog_tick();
void hang_watchdog_phase(const char* where);   // breadcrumb (string literal) written into the report
void hang_watchdog_stop();
