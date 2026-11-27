#pragma once
/* macOS-only native helpers for the hub (dialogs_mac.mm): file / folder panels and one accessibility setting. Never included on Windows. */
#include <string>

/* NSOpenPanel. `folder` = choose a directory. `filter_spec` is the Windows-style spec ("*.gba;*.zip"; nullptr / "" / "*.*" = any file).
 * Runs modal on the main thread. Returns the chosen path (UTF-8), or "" on cancel. */
std::string mac_pick_path(bool folder, const std::string& filter_spec);
/* NSSavePanel. `default_ext` has no dot ("png"); default_name is the suggested file name. Returns "" on cancel. */
std::string mac_pick_save_path(const std::string& default_name, const std::string& filter_spec, const std::string& default_ext);
/* NSWorkspace.accessibilityDisplayShouldReduceMotion. */
bool mac_reduce_motion();
/* NSPasteboard: puts the picture in this file (PNG / JPEG) on the clipboard. false when it cannot be read. */
bool mac_copy_image_file_to_clipboard(const std::string& path);

#include <vector>

/* A top-level window of another app (normal layer only). */
struct MacWindow { unsigned id = 0; int pid = 0; int w = 0, h = 0; bool on_screen = false; };
/* CGWindowListCopyWindowInfo: the normal-layer windows owned by one of `pids`. include_offscreen adds minimised / hidden ones.
 * Needs no permission (titles are not read). */
std::vector<MacWindow> mac_windows_of_pids(const std::vector<int>& pids, bool include_offscreen);
/* pid of the frontmost app (NSWorkspace), 0 when unknown. */
int mac_frontmost_pid();
/* Asks the app with this pid to quit the way Cmd+Q does (NSRunningApplication terminate). false when it is not a GUI app (use SIGTERM). */
bool mac_request_quit(int pid);
/* Bounces the Dock icon once (never takes focus; nothing when the hub is already frontmost). */
void mac_bounce_dock();
