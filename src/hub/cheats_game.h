#pragma once
#include "app.h"
/* Cheats panel opened from the running game (game menu "Cheats", or the View+Y overlay): closing it gives focus back to the game. */
void cheats_open_from_game(const hub::GameEntry& e);
