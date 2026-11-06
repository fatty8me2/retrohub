#pragma once
/* Plan 4 Wave 3 R1: resume beyond GB/GBA/NES/SNES, the state sidecar and the "newest save or this PC's resume point" choice.
 * Logic (pure, tested): resume_logic.h. Everything here runs in the hub before a launch or after a game closed; nothing is
 * ever shown while a game runs. */
#include <string>

namespace hub { struct GameEntry; }

/* The runner flags for this launch: " --resume" or "" (online runs never call this). `states` / `battery` are the vault folders.
 * `blocked` = another feature (Transfer Pak, a real console startup) makes a resumed state wrong. */
std::string resume_launch_arg(const hub::GameEntry& e, const std::string& battery, const std::string& states, bool blocked);

/* Play: when another PC's save is newer than this PC's resume point and the player has not chosen yet, opens the small choice
 * (hub window, before launch) and returns true: the caller must stop; the choice calls play_entry again. */
bool resume_choice_gate(const hub::GameEntry& e, bool blocked);

/* When the game closed: records which PC wrote the resume point (offline runs that wrote a state this session). */
void resume_note_exit(const std::string& system_short, const std::string& key, bool online, unsigned long long start_unix);

void resume_choice_draw();   // once per frame, with the other dialogs
void resume_choice_shot();   // screenshots: open the Play choice with made-up values
