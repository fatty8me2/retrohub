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

enum class Cmd { None, SaveState, LoadState, Screenshot, SwapDisc, Quit };

struct Command {
    long long seq = 0;
    Cmd cmd = Cmd::None;
    int slot = 0;   // 0-9 for save_state / load_state (-1 = the "auto" state, load only)
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
