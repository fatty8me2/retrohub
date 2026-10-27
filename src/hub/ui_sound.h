#pragma once
/* RetroHub menu sounds: tiny API. All sounds are synthesized (boot_synth), no audio assets.
 * Playback uses its own SDL audio device. Silent while a game runs, while the hub is unfocused, and under
 * SSB64_MUTE / RETROHUB_MUTE / RETRORUN_MUTE / N64RUN_MUTE / RETROHUB_QA / HUB_SCREENSHOT runs. */
namespace ui_sound {

enum class Sfx { Move, Select, Back, Error, Popup, Notify };

/* Pure policy (unit-tested): should a sound play right now? */
bool allowed(bool sounds_on, int volume_pct, bool game_running, bool focused, bool env_muted);
bool env_muted();

/* Settings, pushed whenever they change (cheap; call every frame if easiest). */
void configure(bool sounds_on, int volume_pct, bool music_on, bool notif_chime_on);
/* Per frame: hub state. Starts/stops the menu music accordingly. */
void tick(bool game_running, bool focused);

void play(Sfx s);      // no-op when not allowed; Notify also requires the notif_sound setting
void shutdown();

} // namespace ui_sound
