#pragma once
/* Frame pacing choice for one pass of retrorun's main loop (main.cpp do_pacing). Pure: unit tested (test_rewind.cpp). */

namespace retrorun {

/* Chosen once at start (main.cpp setup_video): Vsync when the display refresh is within 5% of the core's fps, else Audio (the audio ring is
 * the clock: the loop waits until it drains below its target), Timer without an audio device. None = headless. */
enum class Pace { None, Vsync, Audio, Timer };

/* The pacing for the next frame. A changed speed and headless runs pace by the timer. A frame that plays no audio cannot be paced by the
 * audio clock: the ring never fills, so the wait never waits and the loop spins as fast as the PC can go. That is a rewind step (its audio is
 * muted): on an audio-paced display (120 / 144 Hz) a held rewind used to run through its whole 20 s history in a fraction of a second. */
inline Pace frame_pace(Pace configured, double speed, bool headless, bool silent_frame) {
    if (speed != 1.0 || headless) return Pace::Timer;
    if (configured == Pace::Audio && silent_frame) return Pace::Timer;
    return configured;
}

}  // namespace retrorun
