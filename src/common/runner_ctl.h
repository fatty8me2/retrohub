#pragma once
/* Hub <-> runner control channel and the save-state / screenshot file contract (docs/RUNNER_CTL.md).
 * Shared by RetroHub.exe (writes commands, reads acks and slot files) and retrorun / n64run (poll the command file).
 *
 *   control file  <run_dir>/control.json      {"seq":N,"cmd":"save_state|load_state|screenshot|swap_disc|quit","slot":0-9}
 *   ack file      <run_dir>/control_ack.json  {"seq":N,"ok":true,"msg":"..."}   (written by the runner, once per seq)
 *   state slots   <state_dir>/slot<N>.state  (+ auto.state)   thumbnail slot<N>.png (<= 320 px wide), sidecar slot<N>.json
 *   screenshots   <vault>/<system>/<game_key>/shots/<yyyyMMdd-HHmmss>.png  (game_dir = hub::saves::game_dir)
 *
 * The runner polls the file's mtime/size ~4x/s, acts once per seq (seq must be larger than the last one seen; the value present at
 * start-up counts as already handled), then writes the ack. Pure helpers; no SDL. */
#include <cstdint>
#include <ctime>
#include <string>

namespace runctl {

enum class Cmd { None, SaveState, LoadState, Screenshot, SwapDisc, Quit,
                 /* added for the quick menu / OSD (docs/RUNNER_CTL.md); all but OsdMsg, Volume and Mute are refused online */
                 Volume,    // value 0-100
                 Mute,      // value 1 = mute, 0 = unmute
                 Ff,        // value 1 = on (the last chosen fast speed, default 2x), 0 = off (1x)
                 Speed,     // speed = 0.25 0.5 1 1.5 2 3 5 8, or 0 = "max" (uncapped)
                 Rewind,    // value 1/0   (runners reply "unsupported" for now)
                 Shader,    // text = shader name (unsupported for now)
                 Overlay,   // value 1/0   (unsupported for now)
                 Clip,      // (unsupported for now)
                 OsdMsg,    // text = toast shown on the runner's OSD
                 Input };   // value 0 = "input off" (neutral input to the core while the hub's quick menu is open), 1 = on. Allowed online.

struct Command {
    long long seq = 0;
    Cmd cmd = Cmd::None;
    int slot = 0;   // 0-9 for save_state / load_state (-1 = the "auto" state, load only)
    int value = 0;          // volume 0-100, or 0/1 for mute / ff / rewind / overlay
    double speed = 1.0;     // Cmd::Speed: one of kSpeeds, or 0 = max
    std::string text;       // Cmd::OsdMsg toast text, Cmd::Shader name (ASCII/UTF-8, <= 120 bytes)
};

constexpr double kSpeeds[] = {0.25, 0.5, 1.0, 1.5, 2.0, 3.0, 5.0, 8.0};
bool speed_valid(double s);                                   // in kSpeeds, or 0 (= max)
bool parse_speed_arg(const std::string& s, double* out);      // "2", "2x", "1.5", "max" -> out (0 = max); false if not allowed
std::string speed_label(double s);                            // "Speed 2x", "Speed 0.5x", "Speed max"
bool offline_only(Cmd c);                                     // true for everything except Quit/Screenshot/OsdMsg/Volume/Mute (volume and mute are local, allowed online)

/* "input off" gate (runner side). While off, the local player's input is neutral (frames/packets still flow, so netplay timing is
 * unchanged). Failsafes: re-enabled after kInputOffTimeoutMs without a refresh (the hub re-sends "input off" while its menu is
 * open, so a dead hub cannot leave the game dead), and on a fresh View+Y press. */
constexpr uint64_t kInputOffTimeoutMs = 30000;
struct InputGate {
    bool off = false;
    uint64_t refreshed_ms = 0;
    bool combo_prev = false;
    void set(bool on, uint64_t now_ms) { off = !on; refreshed_ms = now_ms; }
    bool suppressed(uint64_t now_ms) {
        if (off && now_ms - refreshed_ms > kInputOffTimeoutMs) off = false;
        return off;
    }
    /* View+Y no longer re-enables input: pressing it again while the menu is open (or the press that opened it, when "input off"
     * arrives before the runner sees the combo) let the pad drive the game behind the menu. A dead hub is covered by the timeout. */
    void note_view_y(bool down, uint64_t now_ms) {
        (void)now_ms;
        combo_prev = down;
    }
};

constexpr int kSlots = 10;
constexpr int kAutoSlot = -1;
constexpr int kThumbWidth = 320;

const char* cmd_name(Cmd c);
Cmd cmd_from_name(const std::string& s);

/* Parse a control file body. False for malformed JSON, a missing/non-positive seq, an unknown cmd, or a bad slot. */
bool parse_control(const std::string& text, Command* out);
std::string make_control(const Command& c);

struct Ack {
    long long seq = 0;
    bool ok = false;
    std::string msg;
};
std::string make_ack(const Ack& a);
bool parse_ack(const std::string& text, Ack* out);

/* Paths. `ctl` is the control file path; the ack is its sibling. */
std::string ack_path_for(const std::string& ctl);
std::string state_file(const std::string& state_dir, int slot);     // slot -1: auto.state
std::string thumb_file(const std::string& state_dir, int slot);     // .png
std::string sidecar_file(const std::string& state_dir, int slot);   // .json
std::string shots_dir(const std::string& game_dir);                 // <vault>/<system>/<game key>/shots
std::string shot_name(std::time_t t);                               // yyyyMMdd-HHmmss.png (local time)

/* Sidecar written next to every state. */
struct Sidecar {
    std::string game_key;
    long long time = 0;       // unix seconds the state was written
    long long playtime = 0;   // seconds played this session when written (0 = unknown)
    int disc = 0;             // PS1: 0-based disc in the drive (0 otherwise)
};
std::string make_sidecar(const Sidecar& s);
bool parse_sidecar(const std::string& text, Sidecar* out);

/* Atomic small-file write (tmp + rename). */
bool write_text_atomic(const std::string& path, const std::string& text);
bool read_text(const std::string& path, std::string* out);

/* Hub side: write the next command for a run (seq = previous seq in the file + 1). Returns the seq, 0 on failure. */
long long send_command(const std::string& ctl_path, Cmd cmd, int slot = 0);
/* Same with the full argument set (c.seq is ignored and replaced). e.g. {Cmd::Volume, value=40}, {Cmd::Speed, speed=2}, {Cmd::OsdMsg, text="Hi"}. */
long long send_command_ex(const std::string& ctl_path, Command c);

/* Runner side. Poll from the main loop with a millisecond clock; returns true once per new seq. */
class Poller {
  public:
    explicit Poller(std::string ctl_path = std::string());
    bool poll(uint64_t now_ms, Command* out, uint64_t interval_ms = 250);
    void ack(bool ok, const std::string& msg) const;   // for the command last returned by poll()
    long long last_seq() const { return last_; }
    bool enabled() const { return !path_.empty(); }
  private:
    std::string stamp() const;
    std::string path_, stamp_;
    long long last_ = 0, pending_ = 0;
    uint64_t next_ = 0;
};

}  // namespace runctl
