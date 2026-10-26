/* PLACEHOLDER (no-op) startup sounds for the start animation (boot_anim.h). Replaced by boot_sound.cpp (agent/boot-sound): CMake
 * compiles this file only while boot_sound.cpp does not exist. */
#include "boot_anim.h"

namespace bootanim {

void sound_play(hub::System) {}
void sound_stop() {}
bool sound_available(hub::System) { return false; }

} // namespace bootanim
