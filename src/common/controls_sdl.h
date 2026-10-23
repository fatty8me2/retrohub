#pragma once
/* SDL half of the controller profiles: pad list, "press a button" capture, gamecontrollerdb, and the per-frame
 * PadState read the runners use. Everything polls state (SDL_GameControllerUpdate); nothing touches the event queue.
 *
 * Controllers page API (all in namespace hubctl):
 *   load_gamecontroller_db(path)      once at start (non-XInput pads: DualSense, Switch Pro, 8BitDo)
 *   list_pads()                       connected pads in current port order, with name/guid/serial/xinput flag
 *   Capture c; c.start(pad_key);      "press a button": poll c.poll() each frame until it returns a source token
 *   set_mapping / set_analog / reset  (controls.h) then save_store(...)
 *   set_port_order(store, keys)       "my pad is always port 1" */

#include <string>
#include <vector>

#include "controls.h"

typedef struct _SDL_GameController SDL_GameController;

namespace hubctl {

/* Adds SDL_GameControllerDB mappings from the file (returns how many, -1 if unreadable). Call before opening pads. */
int load_gamecontroller_db(const std::string& path);
/* The bundled copy next to the exe: <exe_dir>/controls/gamecontrollerdb.txt (also tries assets/controls/ for dev runs). */
int load_bundled_db();

struct PadInfo {
    int sdl_index = -1;       // SDL joystick index
    std::string name;
    std::string key;          // guid[#serial]: what Store::port_order holds
    std::string guid;
    std::string serial;
    bool xinput = false;      // Xbox-style (XInput) pad
    int port = 0;             // 0-based port after applying the stored order
};
/* Every connected pad SDL recognises as a game controller, sorted by `port_order` (empty = SDL order). */
std::vector<PadInfo> list_pads(const std::vector<std::string>& port_order = {});

/* Opens the recognised pads in port order. Caller closes them. */
std::vector<SDL_GameController*> open_pads(const std::vector<std::string>& port_order, std::vector<std::string>* names = nullptr);
void read_state(SDL_GameController* c, PadState* out);
std::string pad_key(SDL_GameController* c);

/* "Press a button": remembers the resting state of the chosen pad (key from list_pads, or "" = any pad), then poll()
 * returns "" until something moves clearly (button pressed, axis moved past ~60%), then the source token: "a", "leftx-",
 * "righttrigger", ... which is what set_mapping takes. Poll about every 16 ms; give up with cancel() or a timeout. */
class Capture {
  public:
    ~Capture();
    bool start(const std::string& pad_key_or_empty);
    std::string poll();
    void cancel();
    bool active() const { return !pads_.empty(); }
  private:
    std::vector<SDL_GameController*> pads_;
    std::vector<PadState> rest_;
};

}  // namespace hubctl
