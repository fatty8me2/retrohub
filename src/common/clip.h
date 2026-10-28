#pragma once
/* Instant replay: a downscaled frame ring buffer ("the last 30 seconds") plus an MP4 (H.264) writer on Windows Media
 * Foundation (the same OS codecs streamrun uses; no new dependency). Shared by retrorun and n64run (docs/RUNNER_CTL.md).
 *
 *   Ring      keeps the newest frames as NV12 at <= max_w pixels wide, sampled to ~20 fps, capped in memory (default 80 MB),
 *             so it costs a few percent of one core and is purely cosmetic: it only reads frames, never touches emulation.
 *   Recorder  Ring + "save now" on a worker thread (the game never hitches) + a status the runner polls from its main loop.
 *   write_mp4 / probe_mp4   the encoder and a decoder-based verifier (used by the headless test).
 *
 * Files land in <vault>/<system>/<game key>/shots/<yyyyMMdd-HHmmss>.mp4 next to a <name>.mp4.jpg thumbnail; the hub's
 * screenshot gallery lists the .mp4 and shows the thumbnail (video-less viewer: "Play clip" opens the default player). */
#include <atomic>
#include <cstdint>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace clip {

constexpr int kDefaultSeconds = 30;
constexpr int kDefaultFps = 20;
constexpr int kDefaultMaxW = 320;
constexpr size_t kDefaultCapBytes = 80u * 1024u * 1024u;

struct Config {
    int max_w = kDefaultMaxW;
    int fps = kDefaultFps;
    int seconds = kDefaultSeconds;
    size_t cap_bytes = kDefaultCapBytes;
};

/* A frozen copy of the ring (frames are shared, so taking one is cheap). */
struct Snapshot {
    int w = 0, h = 0;          // even
    double fps = kDefaultFps;  // playback rate of `frames`
    std::vector<std::shared_ptr<const std::vector<uint8_t>>> frames;   // NV12, w*h*3/2 bytes each, oldest first
    double seconds() const { return fps > 0 ? (double)frames.size() / fps : 0.0; }
};

/* Number of source frames per kept frame: round(src_fps / target_fps), at least 1. */
int sample_every(double src_fps, int target_fps);
/* Max kept frames for a config at the given size (the smaller of seconds*fps and cap_bytes / frame size; at least 1). */
size_t capacity_frames(const Config& c, int w, int h);
/* Downscaled even size for a source of sw x sh. */
void scaled_size(int sw, int sh, int max_w, int* dw, int* dh);

class Ring {
  public:
    explicit Ring(Config c = Config()) : cfg_(c) {}
    /* Offer one emulated frame. px = 32-bit pixels, 4 bytes each, stride in bytes. bgra: byte order B,G,R,x (libretro XRGB8888
     * little endian, Windows DIB); else R,G,B,x (GL RGBA). bottom_up: rows are stored bottom first (glReadPixels).
     * src_fps = the game's frame rate; only every sample_every()th offered frame is kept. Returns true if it was kept. */
    bool offer(const uint8_t* px, int w, int h, int stride_bytes, double src_fps, bool bgra = true, bool bottom_up = false);
    Snapshot snapshot() const;
    void clear();
    size_t frames() const;
    size_t bytes() const;
    double seconds() const;
  private:
    Config cfg_;
    mutable std::mutex m_;
    std::deque<std::shared_ptr<const std::vector<uint8_t>>> q_;
    int w_ = 0, h_ = 0;
    uint64_t seen_ = 0;
    double src_fps_ = 60;
};

struct Info {
    int w = 0, h = 0;
    int frames = 0;
    double seconds = 0;
    double mean_luma = 0;      // 0-255, mean of the last decoded frame (a quick "is it not black" check)
    uint64_t bytes = 0;
};

/* Writes snap as an H.264 MP4 (software encoder, ~1.5 Mbit/s at 320 px). Writes to path + ".part" and renames. Also writes
 * path + ".jpg" (thumbnail from ~85% into the clip, the moment you just saved). Blocking; call from a worker thread. */
bool write_mp4(const Snapshot& snap, const std::string& path, std::string* err);
/* Opens the MP4 with Media Foundation's source reader, decodes every frame to RGB32. False (err filled) if it will not decode. */
bool probe_mp4(const std::string& path, Info* out, std::string* err);

/* Ring + asynchronous save. All methods are called from the runner's main thread. */
class Recorder {
  public:
    explicit Recorder(Config c = Config()) : ring_(c) {}
    ~Recorder() { wait(); }
    void set_dir(const std::string& shots_dir) { dir_ = shots_dir; }
    void set_enabled(bool on) { enabled_ = on; if (!on) ring_.clear(); }
    bool enabled() const { return enabled_; }
    bool can_save() const { return !dir_.empty(); }
    void offer(const uint8_t* px, int w, int h, int stride_bytes, double src_fps, bool bgra = true, bool bottom_up = false) {
        if (enabled_) ring_.offer(px, w, h, stride_bytes, src_fps, bgra, bottom_up);
    }
    /* Start saving the ring. False with *msg set when there is nothing to save, a save is running, or there is no folder.
     * `t` names the file (<yyyyMMdd-HHmmss>.mp4). */
    bool request_save(std::time_t t, std::string* msg);
    /* True once per finished save (ok, path or error message). */
    bool poll(bool* ok, std::string* msg);
    bool busy() const { return busy_.load(); }
    void wait();
    Ring& ring() { return ring_; }
  private:
    Ring ring_;
    std::string dir_;
    bool enabled_ = true;
    std::thread th_;
    std::atomic<bool> busy_{false}, done_{false};
    bool ok_ = false;
    std::string msg_;
};

/* File name for a clip: yyyyMMdd-HHmmss.mp4 (local time), same stamp style as screenshots. */
std::string clip_name(std::time_t t);

}  // namespace clip
