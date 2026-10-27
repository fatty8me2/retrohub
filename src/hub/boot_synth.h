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

/* Menu UI sounds (soft, short, console-like) and the quiet ambient menu loop. Same stereo float format as render(). */
enum class Ui { Move, Select, Back, Error, Popup, Chime, Count };
constexpr float kUiMaxSeconds = 1.6f;        // every Ui sound is at most this long
constexpr float kAmbientSeconds = 24.f;      // render_ambient() length (loops)
std::vector<float> render_ui(Ui u);
std::vector<float> render_ambient();

/* 16-bit PCM stereo WAV writer (for review/QA files). */
bool write_wav(const char* path, const std::vector<float>& interleaved_stereo);

} // namespace bootsynth
