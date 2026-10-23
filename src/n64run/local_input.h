#pragma once
/* Local controller input: SDL2 GameControllers + keyboard fallback.
 *
 * Uses SDL_GameControllerUpdate / SDL_PumpEvents / SDL_GetKeyboardState only (never SDL_PollEvent),
 * so it does not steal events from the core's/video plugin's event handling.
 *
 * Keyboard map (default; remappable via the profile's keys; applies to the FIRST locally owned port, merged with pad 0 if one is connected):
 *   arrows      analog stick (+-80)         X = A         C = B        Z = Z trigger
 *   Enter       Start                       A = L         S = R
 *   I/J/K/L     C-up / C-left / C-down / C-right
 *   T/F/G/H     D-pad up / left / down / right
 * Gamepad map (n-th connected pad -> n-th locally owned port):
 *   A -> A, X -> B (Y/B also -> B), Start -> Start, left trigger -> Z, left shoulder -> L,
 *   right shoulder / right trigger -> R, D-pad -> D-pad, right stick -> C buttons (50% threshold),
 *   left stick -> analog (radial 8% deadzone, rescaled, clamped to +-80). */

#include <cstdint>
#include <memory>
#include <vector>

#include "controls.h"
#include "pad_map.h"
namespace n64run {

class LocalInput {
  public:
    ~LocalInput();
    /* Initialise SDL's game controller subsystem and open pads. */
    bool init();
    /* Mapping to use (default: hubctl::default_profile("n64")). Call before init(); the profile also carries the port order. */
    void set_profile(const hubctl::Profile& p);
    int pad_count() const { return (int)pads_.size(); }
    /* Poll everything once (call once per sample). */
    void update();
    /* Packed BUTTONS word for the `owned_index`-th locally owned port (0 = first). */
    uint32_t read(int owned_index) const;
    /* Rumble Pak motor on the pad of the owned port (no-op without rumble support). */
    void rumble(int owned_index, bool on);

  private:
    void rescan();
    hubctl::Profile profile_ = hubctl::default_profile("n64");
    std::unique_ptr<PadMap> map_;
    std::unique_ptr<hubctl::KeyMapper> keys_;   // keyboard pad, from the profile
    std::vector<void*> pads_;   // SDL_GameController*
    std::vector<uint32_t> pad_state_;
    uint32_t kbd_state_ = 0;
    int last_joysticks_ = -1;
    bool inited_ = false;
};

/* Number of game controllers SDL currently sees (initialises the subsystem if needed). */
int count_pads();

}  // namespace n64run
