/* Dolphin emulation speed over a virtual DSU (cemuhook) pad. Pure logic: no Winsock, no windows (test_dolphin_speed.cpp).
 *
 * Dolphin 2509 facts (source: dolphin-emu/dolphin tag 2509):
 *  - Hotkeys "Emulation Speed/Increase|Decrease Emulation Speed" (Core/HotkeyManager.cpp, group "Emulation Speed"); the handler is in
 *    DolphinQt/HotkeyScheduler.cpp: +-0.1 per press on Config::MAIN_EMULATION_SPEED, 0.95..1.05 snaps to 1.0, decrease only applies if the result is > 0.
 *  - Hotkeys obey window focus unless Dolphin.ini [General] HotkeysRequireFocus = False (Core/Config/UISettings.cpp MAIN_FOCUSED_HOTKEYS, default True;
 *    HotkeyScheduler: Core::UpdateInputGate(Config::Get(MAIN_FOCUSED_HOTKEYS))).
 *  - DSU client: Config::System::DualShockUDPClient = file DSUClient.ini, [Server] Enabled = True, Entries = "name:addr:port;"
 *    (InputCommon/ControllerInterface/DualShockUDPClient). The device shows up as "DSUClient/<pad 0-3>/<name>"; buttons: Share, Options, PS, Cross, ...
 */
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "qm_logic.h"

namespace hubds {

constexpr int kPort = 26761;                      /* distinct from the Wii U remote-play DSU port (26760) */
constexpr const char* kDeviceName = "RHSpeed";    /* the "description" field of the DSU server entry = Dolphin's device name */
constexpr int kSlot = 0;                          /* DSU pad id 0 -> Dolphin device "DSUClient/0/RHSpeed" */

/* XInput-layout bits (dsu::DsuBtn) the virtual pad presses; dsu_server.cpp maps kStart -> DS4 Options, kBack -> DS4 Share. */
constexpr uint16_t kBtnStart = 0x0010, kBtnBack = 0x0020;
inline uint16_t pad_buttons(hubqm::DolphinKey k) { return k == hubqm::DolphinKey::Increase ? kBtnStart : kBtnBack; }
inline const char* dolphin_control(hubqm::DolphinKey k) { return k == hubqm::DolphinKey::Increase ? "Options" : "Share"; }

inline std::string server_entries() { return std::string(kDeviceName) + ":127.0.0.1:" + std::to_string(kPort) + ";"; }

/* Hotkey expression: the DSU button OR the old Ctrl+Shift+F13/F14 chord (harmless: nobody has an F13 key). */
inline std::string hotkey_expr(hubqm::DolphinKey k) {
    return std::string("`DSUClient/") + std::to_string(kSlot) + "/" + kDeviceName + ":" + dolphin_control(k) + "` | " + hubqm::dolphin_hotkey_expr(k);
}

/* The scancodes quickmenu passes to dolphin_send_speed_keys (hubqm::dolphin_scan): 0x64 = increase, 0x65 = decrease. */
inline hubqm::DolphinKey key_from_scan(int sc) { return sc == hubqm::dolphin_scan(hubqm::dolphin_vk(hubqm::DolphinKey::Increase)) ? hubqm::DolphinKey::Increase : hubqm::DolphinKey::Decrease; }

/* Press sequencing. Dolphin polls hotkeys every ~5 ms and is edge-triggered: a press needs `hold_ms` with the button down, then `gap_ms`
 * released before the next one. One step = (buttons, duration). A burst of n presses = 2n steps ending released. */
struct Step { uint16_t buttons; int ms; };
inline std::vector<Step> press_steps(const std::vector<hubqm::DolphinKey>& keys, int hold_ms = 40, int gap_ms = 40) {
    std::vector<Step> s;
    for (auto k : keys) { s.push_back({pad_buttons(k), hold_ms}); s.push_back({0, gap_ms}); }
    return s;
}
inline int burst_duration_ms(size_t presses, int hold_ms = 40, int gap_ms = 40) { return (int)presses * (hold_ms + gap_ms); }

/* READBACK. Dolphin 2509 does NOT put the speed in its window title (Core::UpdateTitle only builds "Dolphin 2509 | JIT64 SC | <backend> | HLE"
 * once at boot; FPS/VPS/speed are only drawn in the renderer's OSD, GFX.ini [Settings] ShowSpeed). The test therefore measures the speed from
 * inside the guest: a homebrew loop publishes the PowerPC timebase into RAM, which the test reads from Dolphin's process memory.
 * Speed = guest timebase ticks per wall second / timebase frequency (GameCube: 162 MHz bus / 4 = 40.5 MHz). The timebase is 32 bit here (wraps). */
constexpr double kGcTimebaseHz = 40500000.0;
inline double tb_speed_percent(uint32_t tb0, uint32_t tb1, double wall_s, double tb_hz = kGcTimebaseHz) {
    if (wall_s <= 0) return -1;
    uint32_t d = tb1 - tb0;   /* unsigned wrap */
    return d / tb_hz / wall_s * 100.0;
}

/* Is a measured percentage consistent with `tenths` (x0.1 of 1x)? Only meaningful where the host can keep up (< ~3x).
 * Tolerance: 12 % of the target, at least 4 points (the title speed is a 1 s average and jitters). */
inline bool speed_matches(int tenths, int measured_percent) {
    int want = tenths * 10, tol = want * 12 / 100;
    if (tol < 4) tol = 4;
    return measured_percent >= want - tol && measured_percent <= want + tol;
}

}  // namespace hubds
