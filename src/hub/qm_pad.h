#pragma once
/* Direct controller reads for the quick menu (qm_pad.cpp). Raw mask: bit b = SDL_GameControllerButton b, plus the bits below. */
#include <cstdint>
#include <vector>
#include "qm_nav.h"

union SDL_Event;

namespace qmpad {
constexpr int kLT = 24, kRT = 25, kStickUp = 26, kStickDown = 27, kStickLeft = 28, kStickRight = 29;
}

uint32_t qm_pad_raw();                         // every connected pad OR-ed (SDL + XInput); focus does not matter
std::vector<uint32_t> qm_pad_each();           // one raw mask per pad (combos are checked per pad: P1 View + P2 Y is not View+Y)
uint32_t qm_keyboard_held_async();             // nav keys physically down, whatever has the focus (the open gate only)
void qm_pad_event(const SDL_Event& e);         // main loop, every event, before any filtering: latches button-down taps
uint32_t qm_pad_take_taps();                   // raw bits that went down since the last call
uint32_t qm_nav_bits(uint32_t raw);            // raw pad mask -> hubqm::NavBit
uint32_t qm_keyboard_nav_bits();               // arrows / Enter / Space / Esc / Backspace, only while RetroHub has the keyboard
void qm_pad_restrict(int only_instance, bool no_xinput);   // e2e test: read only the virtual joystick
