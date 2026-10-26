# Boot sounds: originality decision

Real console startup sounds (GameCube, PS1, PS2, GBA, DS, Wii U, ...) are copyrighted. RetroHub ships no
recordings and no note-for-note or timbre-for-timbre recreations of them. Instead `src/hub/boot_synth.cpp`
synthesizes original pieces of 5-10 s at runtime (sine/square/triangle/saw/noise, ADSR, one-pole filters,
echo, Schroeder reverb); no audio files exist in the repo or the package. Each sound only evokes the era and
feel of its console (N64: cartridge clunk + power hum; GB/GBC: bright square pings; GBA: shimmering sweep;
DS/3DS: soft chime; GC: bubbly arpeggio; PS1: deep swell; PS2: airy pad; Wii U: light bells), with
mechanical layers (cart click, lid thunk, disc spin-up). Melodies are invented here.

Playback (`boot_sound.cpp`) uses its own SDL audio device, so the game's audio is never touched or ducked;
The sound is NOT stopped at launch: it plays to its natural end on its own device. `sound_stop()` (150 ms fade) is only called when the user skips with a button/Esc; a second `sound_play` replaces the first. The short animation plays a ~2 s cut with a 250 ms fade.

Each piece is an intro (mechanical insertion layer aligned with the power moment), development and a reverb/fade tail. Mix-down normalises to RMS 0.11 (about -16 LUFS-ish) with a soft limiter (peak < 0.95) and a cosine fade ending at exactly zero.
Durations: N64 6.0 s, GB 5.0, GBC 5.7, GBA 6.0, NES 5.0, SNES 7.2, DS 6.0, 3DS 6.5, GameCube 8.0, PS1 9.5, PS2 9.5, Wii U 7.0.
Timbres: chiptune square/triangle/noise (NES, GB/GBC), 2-op FM plucks and echo (SNES), marimba/bells (DS, 3DS, Wii U), detuned saw/sine pads with long reverb (GBA, PS1, PS2). Muted by `SSB64_MUTE`, `RETRORUN_MUTE`, `N64RUN_MUTE`, `RETROHUB_QA`.
`hub_bootsound_test` renders every sound and writes WAVs to `qa/out/boot_sounds/` for review (never played).
