#pragma once
/* Procedural console "startup" sounds (original, synthesized at runtime; see docs/BOOT_SOUNDS.md).
 * Pure CPU code: no SDL, no files. Used by boot_sound.cpp (playback) and hub_bootsound_test. */
#include <vector>
#include "library.h"

namespace bootsynth {

constexpr int kSampleRate = 44100;

/* Render the sound for `s` as interleaved stereo float [-1,1]. Empty vector = no sound for that system.
 * Deterministic (fixed noise seed). Peak is normalised to ~0.8 and the tail fades to silence. */
std::vector<float> render(hub::System s);

/* 16-bit PCM stereo WAV writer (for review/QA files). */
bool write_wav(const char* path, const std::vector<float>& interleaved_stereo);

} // namespace bootsynth
