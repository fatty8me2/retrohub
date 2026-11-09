#pragma once
/* Features turned OFF for now (2026-11-08, at the owner's request): the game start animation and all RetroHub menu sounds.
 * Flip a constant to true to bring that feature back. Players' saved settings are never touched: boot_anim, menu_sounds, menu_volume,
 * menu_music and notif_sound stay in hub.json as stored, so turning a constant back on restores everyone's own choice.
 * Header-only and pure so tests can link it with hublogic only. */
#include "logic.h"

namespace hubui {

constexpr bool kStartAnimationOn = false;   // the cartridge/disc start animation, its startup sounds, and the "real console startup" intro
constexpr bool kMenuSoundsOn = false;       // menu sounds, menu music and the notification chime inside RetroHub

/* Should the start animation run for these settings? */
inline bool start_animation_on(const Settings& s) { return kStartAnimationOn && s.boot_anim; }

/* What the hub sound engine is configured with. */
struct MenuSoundCfg { bool sounds; int volume; bool music; bool notif; };
inline MenuSoundCfg effective_menu_sound(const Settings& s) {
    if (!kMenuSoundsOn) return {false, 0, false, false};
    return {s.menu_sounds, s.menu_volume, s.menu_music, s.notif_sound};
}

}  // namespace hubui
