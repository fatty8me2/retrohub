/* Dolphin emulation speed without keyboard injection or window focus: a virtual DSU pad on 127.0.0.1:26761 whose buttons are bound to
 * Dolphin's speed hotkeys (emulators.cpp dolphin_hotkeys_ini / dolphin_dsu_ini). See dolphin_speed_logic.h for the Dolphin facts. */
#pragma once
#include <functional>
#include <vector>

/* Start the DSU server (idempotent). False if the port is taken (then dolphin_speed_send falls back to keyboard injection). Call at Dolphin launch. */
bool dolphin_speed_start();
void dolphin_speed_stop();
bool dolphin_speed_running();
/* True once Dolphin's DSU client has registered with the server (so a button press will be seen). */
bool dolphin_speed_connected();
/* Same contract as dolphin_send_speed_keys (app.h): vks are the scancodes from hubqm::dolphin_scan; done_cb(presses completed; -1 = nothing sent)
 * is called on a worker thread. Needs no focus, no SendInput. Waits (up to 4 s) for Dolphin's DSU client; reports -1 at once when the server is
 * not running (caller then uses dolphin_send_speed_keys). No readback exists in production: Dolphin 2509 does not expose its speed (see
 * dolphin_speed_logic.h); the hub tracks the commanded speed and re-syncs through the 0.1x floor. */
void dolphin_speed_send(const std::vector<int>& vks, std::function<void(int)> done_cb);
