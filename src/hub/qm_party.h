#pragma once
/* Plan 4 quick-menu party pieces shared by quickmenu.cpp and qm_party.cpp. */
#include <string>

/* quickmenu.cpp: queue a toast for the running game's OSD (runner OSD only; nothing is raised over the game). No-op when no runner
 * with a control channel is running. Commands go out one at a time (hubqm::CmdPacer). */
void qm_send_osd(const std::string& text);
