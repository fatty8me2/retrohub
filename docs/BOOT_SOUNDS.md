# Boot sounds: originality decision

Real console startup sounds (GameCube, PS1, PS2, GBA, DS, Wii U, ...) are copyrighted. RetroHub ships no
recordings and no note-for-note or timbre-for-timbre recreations of them. Instead `src/hub/boot_synth.cpp`
synthesizes short original sounds at runtime (sine/square/triangle/saw/noise, ADSR, one-pole filters,
echo, Schroeder reverb); no audio files exist in the repo or the package. Each sound only evokes the era and
feel of its console (N64: cartridge clunk + power hum; GB/GBC: bright square pings; GBA: shimmering sweep;
DS/3DS: soft chime; GC: bubbly arpeggio; PS1: deep swell; PS2: airy pad; Wii U: light bells), with
mechanical layers (cart click, lid thunk, disc spin-up). Melodies are invented here.

Playback (`boot_sound.cpp`) uses its own SDL audio device, so the game's audio is never touched or ducked;
`sound_stop()` cuts it on launch. Muted by `SSB64_MUTE`, `RETRORUN_MUTE`, `N64RUN_MUTE`, `RETROHUB_QA`.
`hub_bootsound_test` renders every sound and writes WAVs to `qa/out/boot_sounds/` for review (never played).
