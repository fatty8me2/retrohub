#pragma once
#include <string>

/* "RetroHub closed unexpectedly last time" note (crash_report_ui.cpp). init() looks for a crash report from the previous run that has
 * not been dismissed; draw() is called once per frame from the popup section of main.cpp and shows one calm card, only when
 * prompt_allowed() (no game running, hub already in front) and no other popup is open. */
void crash_report_ui_init(const std::string& logs_dir_utf8);
void draw_crash_report_note();
