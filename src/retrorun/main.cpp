/* retrorun: minimal libretro frontend for RetroHub (GBA / GB / GBC).
 * See docs/RETRORUN.md. */

#include "winutil.h"
#include <windows.h>
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <thread>
#include <string>
#include <vector>

#include "controls_sdl.h"
#include "exit_combo.h"
#include "clip.h"
#include "features.h"
#include "start_hint.h"
#include "runner_ctl.h"
#include "osd.h"
#include "present_fx.h"
#include "core.h"
#include "ds_link.h"
#include "retro_pad.h"
#include "dsview.h"
#include "image.h"
#include "link_sync.h"
#include "ps1_input.h"
#include "rollback.h"
#include "lockstep.h"
#include "perf_overlay.h"
#include "spectate.h"
#include "log.h"
#include "session.h"
#include "save_sync.h"
#include "cheats.h"
#include "cheevos.h"
#include "miniz.h"

constexpr uint8_t kPktCfg = 0x60;
constexpr uint8_t kPktCfg2 = 0x61;   // PS2: {type, slot, u64 total, u32 core, u32 bios, u32 disc, u32 options}: which part differs is reported   // {type, slot, u64 PlayStation setup fingerprint}; unknown to lockstep/rollback (ignored there)

namespace fs = std::filesystem;

/* ------------------------------------------------------------------ args */

struct Args {
    int volume = 100;   // --volume N (0-100): starting volume, applied once audio opens
    std::string control, game_key, shot_dir, load_state_path;   // --load-state FILE: restore this state right after boot (offline)
    //   // --control FILE (runner_ctl.h), --game-key KEY, --shot-dir DIR
    std::string clip_out;   // --clip-out FILE.mp4: QA, headless run saves its last seconds as a clip at the end and verifies it decodes
    std::string osd_msg;   // --osd-msg TEXT: QA, a toast that stays up (use with RETRORUN_OSD_SHOT=1 and --screenshot)
    std::string core, rom, save_dir, state_dir, system_dir, system, screenshot, log, ini;
    std::string scale_filter;   // --filter nearest|linear (default: nearest, DS linear)
    int integer_scale = -1;     // --integer-scale 0|1 (default: on, DS off)
    std::string fx;             // --fx off|crt|lcd|dmg|sharp (screen filter, default off = the classic look)
    int bezel = 0;              // --bezel 0|1 (procedural console frame in the pillarbox bars, default off)
    std::string disc_ctl;         // --disc-ctl FILE: RetroHub rewrites it to request "insert the next disc" (watched like --cheats)
    std::vector<uint64_t> swap_at;   // --swap-disc-at N[,N]: QA, request a disc swap at these frames
    std::string ra_cfg, ra_cache;   // --ra-cfg FILE / --ra-cache DIR: RetroAchievements token file + cache dir (cheevos.h); offline only
    std::string cheats;           // --cheats FILE: cheat list JSON (src/common/cheats.h); polled for live toggles offline
    struct MemDump { unsigned id; size_t off, len; std::string file; };
    std::vector<MemDump> mem_dumps;   // --dump-mem ID,OFFSET,LEN,FILE: raw libretro memory block dumped at exit (QA)
    bool turbo_online = false;    // --turbo-online: the room host allowed turbo buttons for everyone
    std::string controls;         // --controls FILE / RETROHUB_CONTROLS: resolved controller profile JSON (src/common/controls.h)
    std::string setup_tag;        // --setup-tag TEXT: extra text mixed into the PS2 setup fingerprint (the hub passes the disc serial)
    std::string net, room, title, save_stem;   // save_stem: file stem for <stem>.srm (zipped ROMs); "" = rom file stem
    std::string sync_hash_mode = "auto", pace_override;
    bool share_host_save = false; // --share-host-save: lockstep session boots from the host's save (+ clock) on every PC
    bool link = false;            // two linked Game Boys in one SameBoy instance (see docs/RETRORUN.md)
    std::string link_roms;        // optional "romP0,romP1" (default: --rom for both)
    bool realtime = false;      // --realtime (with --headless): pace like a real game (timer, honours runner_ctl speed) instead of running uncapped; QA
    bool list_opts = false, headless = false, fullscreen = false, resume = false, fuzz = false, selftest = false;
    double stylus_u = -1, stylus_v = -1;   // headless DS test: hold the stylus down at this fraction of the frame
    int frames = -1, scale = 3, slot = 0, players = 1, delay = -1;   // delay -1 = auto
    int ports[4] = {0, -1, -1, -1};
    bool ports_given = false;
    bool rollback = false;        // --rollback: GGPO-style rollback netplay instead of lockstep (see rollback.h)
    bool spectate = false;        // --spectate: watch a running online match as a port-less spectator (confirmed inputs only; needs --net --room --ports)
    bool no_spectators = false;   // --no-spectators: as the host, do not record/serve the input history to spectators
    int synctest = 0;             // --synctest N: offline, every frame roll back N frames and re-simulate, compare
    int rb_pred = 14;             // --rb-pred N: max predicted frames (1..16)
    bool rb_adaptive = true;      // --rb-delay-adapt / --no-rb-delay-adapt: pick the input delay from the measured RTT when --delay is auto
    bool multitap = false, no_multitap = false;  // SNES multitap on port 2 (players 3-4); auto with --players >= 3 or 3+ pads
    bool ds_link = false;   // DS local wireless over the room (netpacket interface), no lockstep
    std::vector<std::string> opts;
    int rewind_mode = 0, runahead = 0;   // --rewind auto|on|off, --runahead off|1|2|auto (offline only; see features.h)
    int selftest_rewind_frames = 0, selftest_rewind_back = 0, selftest_runahead_frames = 0;   // QA: --rewind-selftest N,K / --runahead-selftest N (headless)
};

static std::string narrow(const wchar_t* w) { return winutil::to_utf8(w); }

static void usage() {
    fputs("usage: retrorun --core <x_libretro.dll> --rom <file> --save-dir <dir> --state-dir <dir>\n"
          "  [--system gba|gb|gbc|nds|ps1|nes|snes] [--multitap|--no-multitap] [--system-dir DIR] [--scale 3] [--filter nearest|linear] [--integer-scale 0|1] [--fx off|crt|lcd|dmg|sharp] [--bezel 0|1] [--fullscreen] [--resume] [--title TEXT]\n"
          "  [--opt key=value]... [--ini FILE] [--log FILE]\n"
          "  [--headless [--frames N] [--screenshot out.png|out.bmp]] [--stylus U,V] [--selftest] [--fuzz-input] [--sync-hash auto|state|output] [--rewind auto|on|off] [--runahead off|1|2|auto] [--rollback] [--rb-pred N] [--synctest N] [--pace vsync|audio|timer] [--list-options]\n"
          "  [--net SERVER --room CODE-N --slot K --players N --ports P0,P1,P2,P3 --delay auto|D]\n",
          stderr);
}

static bool parse_args(const std::vector<std::string>& a, Args* o) {
    for (size_t i = 0; i < a.size(); i++) {
        const std::string& k = a[i];
        auto val = [&](std::string* out) {
            if (i + 1 >= a.size()) { fprintf(stderr, "missing value for %s\n", k.c_str()); return false; }
            *out = a[++i];
            return true;
        };
        std::string v;
        if (k == "--core") { if (!val(&o->core)) return false; }
        else if (k == "--rom") { if (!val(&o->rom)) return false; }
        else if (k == "--controls") { if (!val(&o->controls)) return false; }
        else if (k == "--turbo-online") o->turbo_online = true;
        else if (k == "--cheats") { if (!val(&o->cheats)) return false; }
        else if (k == "--ra-cfg") { if (!val(&o->ra_cfg)) return false; }
        else if (k == "--ra-cache") { if (!val(&o->ra_cache)) return false; }
        else if (k == "--disc-ctl") { if (!val(&o->disc_ctl)) return false; }
        else if (k == "--swap-disc-at") {
            if (!val(&v)) return false;
            for (size_t q = 0; q < v.size();) { o->swap_at.push_back(strtoull(v.c_str() + q, nullptr, 10)); size_t c = v.find(',', q); if (c == std::string::npos) break; q = c + 1; }
        }
        else if (k == "--dump-mem") {
            if (!val(&v)) return false;
            unsigned id = 0; unsigned long long off = 0, len = 0; char file[1024] = {0};
            if (sscanf(v.c_str(), "%u,%llu,%llu,%1023[^\n]", &id, &off, &len, file) != 4 || !len) { fprintf(stderr, "bad --dump-mem (want ID,OFFSET,LEN,FILE)\n"); return false; }
            o->mem_dumps.push_back({id, (size_t)off, (size_t)len, file});
        }
        else if (k == "--save-dir") { if (!val(&o->save_dir)) return false; }
        else if (k == "--volume") { if (!val(&v)) return false; o->volume = std::max(0, std::min(100, atoi(v.c_str()))); }
        else if (k == "--control") { if (!val(&o->control)) return false; }
        else if (k == "--game-key") { if (!val(&o->game_key)) return false; }
        else if (k == "--clip-out") { if (!val(&o->clip_out)) return false; }
        else if (k == "--shot-dir") { if (!val(&o->shot_dir)) return false; }
        else if (k == "--state-dir") { if (!val(&o->state_dir)) return false; }
        else if (k == "--system-dir") { if (!val(&o->system_dir)) return false; }
        else if (k == "--system") { if (!val(&o->system)) return false; }
        else if (k == "--multitap") { o->multitap = true; }
        else if (k == "--no-multitap") { o->no_multitap = true; }
        else if (k == "--screenshot") { if (!val(&o->screenshot)) return false; }
        else if (k == "--log") { if (!val(&o->log)) return false; }
        else if (k == "--ini") { if (!val(&o->ini)) return false; }
        else if (k == "--net") { if (!val(&o->net)) return false; }
        else if (k == "--room") { if (!val(&o->room)) return false; }
        else if (k == "--osd-msg") { if (!val(&o->osd_msg)) return false; }
        else if (k == "--title") { if (!val(&o->title)) return false; }
        else if (k == "--setup-tag") { if (!val(&o->setup_tag)) return false; }
        else if (k == "--save-stem") { if (!val(&o->save_stem)) return false; }
        else if (k == "--filter") { if (!val(&v)) return false; if (v != "nearest" && v != "linear") { fprintf(stderr, "bad --filter\n"); return false; } o->scale_filter = v; }
        else if (k == "--fx") { if (!val(&v)) return false; if (!fx::parse_filter(v, nullptr)) { fprintf(stderr, "bad --fx\n"); return false; } o->fx = v; }
        else if (k == "--bezel") { if (!val(&v)) return false; o->bezel = atoi(v.c_str()) ? 1 : 0; }
        else if (k == "--integer-scale") { if (!val(&v)) return false; o->integer_scale = atoi(v.c_str()) ? 1 : 0; }
        else if (k == "--opt") { if (!val(&v)) return false; o->opts.push_back(v); }
        else if (k == "--frames") { if (!val(&v)) return false; o->frames = atoi(v.c_str()); }
        else if (k == "--scale") { if (!val(&v)) return false; o->scale = std::max(1, atoi(v.c_str())); }
        else if (k == "--slot") { if (!val(&v)) return false; o->slot = atoi(v.c_str()); }
        else if (k == "--players") { if (!val(&v)) return false; o->players = atoi(v.c_str()); }
        else if (k == "--delay") { if (!val(&v)) return false; o->delay = v == "auto" ? -1 : std::max(0, std::min(30, atoi(v.c_str()))); }
        else if (k == "--ports") {
            if (!val(&v)) return false;
            int n = 0;
            size_t p = 0;
            while (n < 4 && p <= v.size()) {
                size_t q = v.find(',', p);
                if (q == std::string::npos) q = v.size();
                o->ports[n++] = atoi(v.substr(p, q - p).c_str());
                p = q + 1;
            }
            for (; n < 4; n++) o->ports[n] = -1;
            o->ports_given = true;
        }
        else if (k == "--stylus") {   // U,V in 0..1 of the displayed frame (headless DS input test)
            if (!val(&v)) return false;
            if (sscanf(v.c_str(), "%lf,%lf", &o->stylus_u, &o->stylus_v) != 2) { fprintf(stderr, "bad --stylus (want U,V)\n"); return false; }
        }
        else if (k == "--rollback") o->rollback = true;
        else if (k == "--spectate") o->spectate = true;
        else if (k == "--no-spectators") o->no_spectators = true;
        else if (k == "--no-rollback") o->rollback = false;
        else if (k == "--no-rb-delay-adapt") o->rb_adaptive = false;
        else if (k == "--synctest") { if (!val(&v)) return false; o->synctest = std::max(1, atoi(v.c_str())); }
        else if (k == "--rb-pred") { if (!val(&v)) return false; o->rb_pred = std::max(1, std::min(rb::kMaxPred, atoi(v.c_str()))); }
        else if (k == "--link") o->link = true;
        else if (k == "--share-host-save") o->share_host_save = true;
        else if (k == "--host-save-dir") { std::string ignored; if (!val(&ignored)) return false; std::fprintf(stderr, "note: --host-save-dir is ignored by retrorun (--share-host-save shares the core's own save RAM)\n"); }
        else if (k == "--link-roms") { if (!val(&o->link_roms)) return false; }
        else if (k == "--rewind") { if (!val(&v) || !rwd::parse_mode(v, &o->rewind_mode)) { fprintf(stderr, "bad --rewind (auto|on|off)\n"); return false; } }
        else if (k == "--runahead") { if (!val(&v) || !runahead::parse(v, &o->runahead)) { fprintf(stderr, "bad --runahead (off|1|2|auto)\n"); return false; } }
        else if (k == "--rewind-selftest") { if (!val(&v) || sscanf(v.c_str(), "%d,%d", &o->selftest_rewind_frames, &o->selftest_rewind_back) != 2) return false; }
        else if (k == "--runahead-selftest") { if (!val(&v)) return false; o->selftest_runahead_frames = atoi(v.c_str()); }
        else if (k == "--headless") o->headless = true;
        else if (k == "--realtime") o->realtime = true;
        else if (k == "--fullscreen") o->fullscreen = true;
        else if (k == "--resume") o->resume = true;
        else if (k == "--load-state") { if (!val(&o->load_state_path)) return false; }
        else if (k == "--ds-link") o->ds_link = true;
        else if (k == "--fuzz-input") o->fuzz = true;
        else if (k == "--selftest") o->selftest = true;
        else if (k == "--pace") { if (!val(&o->pace_override)) return false; }
        else if (k == "--sync-hash") { if (!val(&o->sync_hash_mode)) return false; }
        else if (k == "--list-options") o->list_opts = true;
        else if (k == "--help" || k == "-h") { usage(); return false; }
        else { fprintf(stderr, "unknown argument: %s\n", k.c_str()); usage(); return false; }
    }
    if (o->core.empty() || o->rom.empty()) { usage(); return false; }
    return true;
}

/* ------------------------------------------------------------- utilities */

static bool env_is(const char* name) {
    const char* e = getenv(name);
    return e && e[0] == '1';
}

static std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

/* retrorun.ini: "key = value" lines, '#'/';' comments, optional [gba]/[gb]/[gbc]/[global] sections. */
static void load_ini(const std::string& path, const std::string& system, std::map<std::string, std::string>* out) {
    std::ifstream f(fs::u8path(path));
    if (!f) return;
    rlog("options: reading %s", path.c_str());
    std::string line, section = "global";
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[' && line.back() == ']') { section = lower(trim(line.substr(1, line.size() - 2))); continue; }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (section != "global" && section != lower(system)) continue;
        (*out)[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
}

static bool read_file(const std::string& path, std::vector<uint8_t>* out) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

/* Write to <path>.tmp then rename over <path>, so a crash never leaves a torn file. */
static bool write_atomic(const std::string& path, const void* data, size_t size) {
    fs::path p = fs::u8path(path);
    fs::path tmp = p;
    tmp += L".tmp";
    {
        FILE* f = _wfopen(tmp.c_str(), L"wb");
        if (!f) return false;
        bool ok = size == 0 || fwrite(data, 1, size, f) == size;
        ok = (fflush(f) == 0) && ok;
        fclose(f);
        if (!ok) { std::error_code ec; fs::remove(tmp, ec); return false; }
    }
    if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ec;
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

/* Status line -> OSD colour. */
static osd::Kind osd_kind_for(const std::string& s) {
    auto has = [&](const char* w) { return s.find(w) != std::string::npos; };
    if (has("failed") || has("FAILED") || has("Cannot") || has("cannot") || has("No state")) return osd::Kind::Error;
    if (has("disabled") || has("needs") || has("disconnected") || has("not available") || has("only one") || has("does not support")) return osd::Kind::Warn;
    if (has("Saved") || has("Loaded") || has("saved") || has("inserted")) return osd::Kind::Good;
    return osd::Kind::Info;
}

/* ------------------------------------------------------------------ audio */

/* SPSC ring of stereo int16 frames feeding an SDL audio device. The producer (emulation thread)
 * resamples by 1/ratio with linear interpolation; ratio = 1 +/- 0.5% from the ring fill level
 * ("dynamic rate control") when the frame loop is paced by vsync. */
class AudioOut {
  public:
    static constexpr size_t kCap = 16384;   // frames (power of two)

    bool open(double rate) {
        SDL_AudioSpec want{}, got{};
        want.freq = (int)std::lround(rate);
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 512;
        want.callback = &AudioOut::callback;
        want.userdata = this;
        dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);   // SDL converts to the device format itself
        if (!dev_) { rlog("audio: cannot open device: %s", SDL_GetError()); return false; }
        ring_.assign(kCap * 2, 0);
        target_ = (size_t)(rate * 0.07);
        rate_ = rate;
        rlog("audio: %d Hz stereo, buffer target %zu frames", want.freq, target_);
        SDL_PauseAudioDevice(dev_, 0);
        return true;
    }
    void close() { if (dev_) { SDL_CloseAudioDevice(dev_); dev_ = 0; } }
    bool active() const { return dev_ != 0; }
    size_t fill() const { return (size_t)(wr_.load(std::memory_order_acquire) - rd_.load(std::memory_order_acquire)); }
    size_t target() const { return target_; }
    void set_volume(int v) { vol_.store(std::max(0, std::min(100, v)), std::memory_order_relaxed); }   // 0-100 (squared curve), runner_ctl "volume"
    void set_mute(bool m) { mute_.store(m, std::memory_order_relaxed); }
    void set_speed(double s) { speed_ = s > 0 ? s : 1.0; }   // runner_ctl "speed": input consumed s times faster (emulation thread only)
    int volume() const { return vol_.load(std::memory_order_relaxed); }
    bool muted_now() const { return mute_.load(std::memory_order_relaxed); }

    void push(const int16_t* in, size_t frames, bool drc) {
        if (!dev_) return;
        double ratio = 1.0;
        if (drc) {
            double err = ((double)target_ - (double)fill()) / (double)target_;
            ratio = 1.0 + std::max(-1.0, std::min(1.0, err)) * 0.005;
        }
        const double step = speed_ / ratio;   // speed_ > 1: consume input faster (pitch up); < 1: slower
        const double gain = mute_.load(std::memory_order_relaxed) ? 0.0 : std::pow(vol_.load(std::memory_order_relaxed) / 100.0, 2.0);
        uint64_t w = wr_.load(std::memory_order_relaxed);
        const uint64_t r = rd_.load(std::memory_order_acquire);
        for (size_t i = 0; i < frames; i++) {
            const double cl = in[i * 2], cr = in[i * 2 + 1];
            while (phase_ < 1.0) {
                if (w - r >= kCap) { phase_ = 0; break; }   // full: drop
                size_t idx = (size_t)(w & (kCap - 1)) * 2;
                ring_[idx] = (int16_t)std::lround((prev_[0] + (cl - prev_[0]) * phase_) * gain);
                ring_[idx + 1] = (int16_t)std::lround((prev_[1] + (cr - prev_[1]) * phase_) * gain);
                w++;
                phase_ += step;
            }
            phase_ -= 1.0;
            if (phase_ < 0) phase_ = 0;
            prev_[0] = cl;
            prev_[1] = cr;
        }
        wr_.store(w, std::memory_order_release);
    }

  private:
    static void SDLCALL callback(void* ud, Uint8* stream, int len) { ((AudioOut*)ud)->pull((int16_t*)stream, (size_t)len / 4); }
    void pull(int16_t* out, size_t frames) {
        uint64_t r = rd_.load(std::memory_order_relaxed);
        const uint64_t w = wr_.load(std::memory_order_acquire);
        size_t avail = (size_t)(w - r);
        size_t n = std::min(avail, frames);
        for (size_t i = 0; i < n; i++, r++) {
            size_t idx = (size_t)(r & (kCap - 1)) * 2;
            out[i * 2] = ring_[idx];
            out[i * 2 + 1] = ring_[idx + 1];
        }
        if (n < frames) {
            memset(out + n * 2, 0, (frames - n) * 4);
            underruns_++;
        }
        rd_.store(r, std::memory_order_release);
    }

    SDL_AudioDeviceID dev_ = 0;
    std::vector<int16_t> ring_;
    std::atomic<uint64_t> rd_{0}, wr_{0};
    size_t target_ = 0;
    double rate_ = 0, phase_ = 0, prev_[2] = {0, 0};
    double speed_ = 1.0;
    std::atomic<int> vol_{100};
    std::atomic<bool> mute_{false};
    std::atomic<uint32_t> underruns_{0};
};

/* ---------------------------------------------------------------- frontend */

enum class Pace { None, Vsync, Audio, Timer };

class Frontend {
  public:
    Args a;
    Core core;
    AudioOut audio;
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    SDL_Texture* tex = nullptr;
    int tex_w = 0, tex_h = 0;
    // Game-start controller tips (src/common/start_hint.h), drawn into our own frame. RETROHUB_START_HINT=0 turns it off.
    SDL_Texture* hint_tex = nullptr;
    int hint_w = 0, hint_h = 0, hint_for_h = 0;
    uint64_t hint_t0 = 0;
    bool hint_done = false;
    void draw_hint();
    bool fullscreen = false, quit = false, muted = false, online = false, ff = false;
    /* runner_ctl "speed" / "ff" (offline only). speed = multiplier (1 = normal); speed_max = uncapped (also what the Tab key does);
     * fast_speed_ = what "ff on" selects (0 = max), the last fast speed chosen, default 2x. */
    double speed = 1.0, fast_speed_ = 2.0;
    bool boost_active_ = false;   // View+RT quick boost is on
    bool speed_max = false;
    void apply_speed(double s);
    SDL_Texture* osd_tex = nullptr;
    int osd_tex_w = 0, osd_tex_h = 0;
    osd::Layer osd_layer;
    void draw_osd();
    bool srm_size_mismatch = false;
    std::string userdata_dir;
    std::string layout_pref_path() const { return userdata_dir + "\\retrorun_layout.txt"; }
    std::string load_layout_pref() const {   // "nds=<layout>" lines; per system, shared by all games
        std::ifstream f(fs::u8path(layout_pref_path()));
        std::string line;
        while (std::getline(f, line)) {
            line = trim(line);
            if (line.rfind("nds=", 0) == 0) return trim(line.substr(4));
        }
        return "";
    }
    void save_layout_pref(const std::string& v) const {
        std::string t = "nds=" + v + "\n";
        std::error_code ec;
        fs::create_directories(fs::u8path(userdata_dir), ec);
        write_atomic(layout_pref_path(), t.data(), t.size());
    }
    std::string scale_filter_;   // "" = nearest (DS: linear)
    bool integer_scale_ = true;
    /* Screen filters + bezel (present_fx.cpp). Off + no bezel = the classic path, untouched. */
    fx::Engine fxe_;
    fx::Filter fx_filter_ = fx::Filter::Off;
    bool bezel_on_ = false;
    std::vector<uint32_t> fx_px_;
    SDL_Texture* fx_tex_ = nullptr; int fx_tex_w_ = 0, fx_tex_h_ = 0;
    SDL_Texture* bezel_tex_ = nullptr; int bezel_tex_w_ = 0, bezel_tex_h_ = 0;
    std::string bezel_key_;
    bool fx_draw(const SDL_Rect& dst);          // draws bezel + filtered frame; false = caller draws the plain frame
    bool fx_set(const std::string& what, std::string* msg);   // runner_ctl "shader"
    FrameBuf fx_compose(const FrameBuf& f, int ow, int oh);   // headless screenshot of the filtered/bezelled window
    void layout_for(int ow, int oh, SDL_Rect* dst);
    bool is_ps2 = false;       // PS2 core (LRPS2 / ParaLLEl PS2): input-delay lockstep only (no rollback), see docs/PS2_ONLINE.md
    int16_t local_press_[4][2] = {};      // PS2: L2/R2 pressure 0..255 per local pad (rides in the upper 16 bits of the button word)
    bool is_ps1 = false;       // PlayStation (Beetle PSX): DualShock analog sticks, memory card 1 in the .srm
    int16_t frame_stick[4][4] = {};         // analog sticks per port for the frame about to run: lx ly rx ry
    int16_t local_stick_[4][4] = {};
    bool is_ds = false;        // Nintendo DS: two-screen frame, fractional+linear scaling, stylus via the mouse
    int ds_layout = 0;         // dsview::layout_name index (F9 cycles)
    SDL_Rect last_dst{0, 0, 0, 0};   // where the frame was last drawn (stylus mapping)
    dsview::PadCursor pad_cursor;    // DS: right stick moves it, the profile's R3 touches
    bool pad_touch = false;
    int last_mouse_x = -1, last_mouse_y = -1;
    uint64_t cursor_tick = 0;
    uint16_t rumble_lvl[4][2] = {};
    /* Rewind + run-ahead (features.h): offline only. spec_ = a rewind / run-ahead frame is running (audio muted). */
    std::unique_ptr<retrofeat::Features> feat_;
    bool spec_ = false;
    std::unique_ptr<retrofeat::Features> make_feat(bool measure);
    int run_feat_selftest();
    bool ra_rewound_ = false;   // a rewind ran since the last real frame: rc_client is reset once play resumes
    void run_core_frame() {
        if (feat_) { feat_->frame(); if (feat_->rewinding()) ra_rewound_ = true; }
        else { core.run(); ra_.frame(); }
    }
    /* Instant replay (clip.h): ring of downscaled frames; View+LB or runner_ctl "clip" saves the last 30 s as an MP4 in the shots folder. */
    clip::Recorder clips_;
    bool clip_combo_prev_ = false;
    uint64_t clip_cool_until_ = 0;
    void clip_frame() { const FrameBuf& f = core.frame(); if (!f.px.empty() && !spec_) clips_.offer((const uint8_t*)f.px.data(), f.w, f.h, f.w * 4, fps); }
    void clip_request();
    void clip_poll();
    hubctl::ExitCombo exit_combo;     // Back+Start held 1 s on any pad: leave (saves flush in finish())
    dsview::StylusLatch stylus;      // a press only counts if it began over the frame
    Pace pace = Pace::None;
    uint64_t frame_no = 0;
    uint64_t out_hash = 0xCBF29CE484222325ull, in_hash = 0xCBF29CE484222325ull;   // rolling hash of video+audio output / inputs
    bool use_state_hash = true;
    void fold_frame();
    uint64_t sync_hash();
    std::vector<SDL_GameController*> pads;
    hubctl::Profile prof = hubctl::default_profile("retro");
    std::unique_ptr<retrorun::RetroPad> retro_pad;

    int owner[4] = {0, -1, -1, -1};
    int local_ports[4] = {-1, -1, -1, -1};
    runctl::InputGate in_gate_;   // hub quick menu open => neutral local input (docs/RUNNER_CTL.md "input")
    int n_local = 0;
    uint32_t frame_pad[4] = {0, 0, 0, 0};   // RetroPad bitmask per port for the frame about to run

    std::string rom_stem, srm_path;
    // Window caption prefix: --title if given, else "retrorun - <rom stem>".
    std::string window_base() const { return a.title.empty() ? "retrorun - " + rom_stem : a.title; }
    std::vector<uint8_t> srm_saved;
    uint64_t last_srm_check = 0;
    int slot_sel = 0;
    bool want_save = false, want_load = false;
    std::string status_msg;
    uint64_t status_until = 0;

    hubnet::Session session;
    std::unique_ptr<n64run::Lockstep> lockstep;
    uint32_t rb_dropped_seen_ = 0;
    std::unique_ptr<rb::Engine> rbe;   // rollback netplay (--rollback); replaces lockstep
    bool resim = false;                // re-simulating: drop audio
    uint64_t fr_audio = 0xCBF29CE484222325ull;   // hash of the audio of the frame being stepped
    rb::Sim make_rb_sim();
    /* spectating (spectate.h) */
    std::unique_ptr<spectate::Host> spec_host_;   // rollback host: confirmed frames for spectators
    bool rb_spectator_ = false;
    int run_rb_spectator();
    void spec_host_service();
    void spec_host_finish();
    /* Live join (spectate.h kPktJoin): a snapshot of the confirmed state, compressed off the main thread, sent with rb::xfer to that
     * spectator only, while spec_host_ streams the confirmed inputs from its frame. */
    struct SpecSnap {
        std::thread th;
        std::atomic<bool> ready{false};
        bool ok = false, sending = false, packed = true;
        std::vector<uint8_t> raw, z;
        uint32_t frame = 0, t_join = 0, t_send = 0;
        uint64_t ro = 0, ri = 0, raw_hash = 0;
        rb::xfer::Sender snd;
        ~SpecSnap() { if (th.joinable()) th.join(); }
    };
    std::map<int, std::unique_ptr<SpecSnap>> spec_snap_;
    std::map<int, uint32_t> spec_snap_done_;   // slot -> when its snapshot was delivered (knocks still in flight are ignored)
    uint8_t spec_snap_epoch_ = 0;
    bool spec_live_ok_ = false;
    void spec_on_join(int slot);
    void spec_on_xfer(int slot, const uint8_t* d, size_t n);
    void spec_nolive(int slot, uint8_t reason);
    void spec_snap_service(uint32_t spec_mask);
    void spec_process_joins();
    std::vector<int> spec_join_q_;   // Joins wait for a frame boundary (lockstep hands them over from inside an input poll)
    /* the confirmed-input history server: rollback (spec_host_) or lockstep (inside n64run::Lockstep) */
    spectate::Host* spec_h() { return spec_host_ ? spec_host_.get() : (lockstep ? lockstep->spectate_host() : nullptr); }
    /* lockstep spectator: live join before the first frame, then real time once close to the host */
    bool ls_spectator_live_join();
    void ls_spectator_frame();
    spectate::CatchUp ls_cu_;
    bool ls_live_reported_ = false, ls_live_snapshot_ = false;
    uint32_t ls_t_join_ = 0, ls_f_start_ = 0, ls_done_ep_ = 0x100, ls_done_at_ = 0;
    void note_frame_gap();
    /* player frame pacing: the longest gap between two simulated frames (stall check while a spectator joins) */
    double gap_max_ms_ = 0, gap_win_ms_ = 0;
    uint64_t gap_last_ = 0, gap_over_ = 0;
    int run_synctest();
    void rb_warmup();
    void rb_log_stats();
    uint32_t slot_mask = 1;
    double fps = 60.0;
    int64_t qpc_freq = 1, next_deadline = 0;

    int run(const Args& args);

    /* ---- link mode: N consoles in one core instance ---- */
    bool link = false;
    int link_n = 2;                          // consoles in the simulation
    int link_port_of_slot(int slot) const { for (int i = 0; i < link_n; i++) if (owner[i] == slot) return i; return -1; }
    bool link_own(int inst) const { return inst >= 0 && inst < link_n && owner[inst] == a.slot; }
    std::string link_srm(int inst) const;    // <stem>.srm for the first own console, <stem>.p<N>.srm otherwise
    std::string link_rtc(int inst) const { std::string p = link_srm(inst); return p.substr(0, p.size() - 4) + ".rtc"; }
    std::vector<uint8_t> link_blob_from_disk(int inst) const;
    void link_apply_blob(int inst, const std::vector<uint8_t>& blob);
    void link_save(bool force);
    bool link_loaded = false;   // never write a console save before its real save was loaded into the core
    std::vector<std::vector<uint8_t>> link_saved_;   // last written sram+rtc per console
    SDL_Rect view_src() const;               // portion of the frame shown to this player

  private:
    bool setup_paths();
    bool setup_video();
    void present();
    void perf_tick();
    bool perf_on_ = false;
    perfov::FpsMeter perf_fps_;
    perfov::RateMeter perf_rb_;
    void layout(SDL_Rect* dst);
    void pump_events(bool idle);
    void handle_key(const SDL_KeyboardEvent& k);
    void open_pads();
    void toggle_fullscreen();
    uint32_t poll_local(int i);
    void poll_all_local();
    void set_status(const std::string& s);
    void load_srm();
    bool share_host_save_run();
    bool guest_save_ro = false;   // --share-host-save guest: the save lives in a throwaway dir and is never written back
    std::string rtc_path() const { return srm_path.substr(0, srm_path.size() - 4) + ".rtc"; }
    std::vector<uint8_t> rtc_saved;
    void save_srm(bool force);
    std::string state_path(const std::string& name) const { return a.state_dir + "\\" + name + ".state"; }
    bool save_state(const std::string& name);
    bool load_state(const std::string& name);
    uint64_t state_hash();
    bool setup_net();
    uint64_t ps1_config_fingerprint();
    bool ps2_config_check();   // PS2: core build, BIOS file, disc and core options must match on every PC
    bool ps2_card_presync();   // PS2: before the core boots, the host's memory card is sent to every PC
    bool ps1_config_check();   // all PCs must run the same BIOS / core build / hardware options
    bool setup_link();
    void link_pump();
    void link_send(int flags, const void* buf, size_t len, uint16_t client_id);
    void link_stop(const char* why);
    void link_send_probe(const std::vector<uint8_t>& pr) {
        auto fr = dslink::split(link_msg_id++, pr.data(), pr.size());
        for (auto& f : fr) session.send(-1, f.data(), (int)f.size());
        probe_tx++;
    }
  public:
    void link_send_pub(int f, const void* b, size_t l, uint16_t c) { link_send(f, b, l, c); }
    void link_pump_pub() { link_pump(); }
  private:
    int net_rc = 3;
    /* Exit code when the session dies: 8 when a partner (usually the host) left on purpose or the host is gone, 4 for a plain connection loss. */
    int lost_code() { std::string s = session.status(); return (s.find("host left") != std::string::npos || s.find("partner ended") != std::string::npos) ? 8 : 4; }
    bool link_active = false;
    uint16_t link_msg_id = 0;
    dslink::Reassembler link_reasm;
    uint64_t link_tx = 0, link_rx = 0, link_tx_cmd = 0, link_tx_reply = 0, link_rx_cmd = 0, link_rx_reply = 0, link_last_log = 0;
    std::map<int, uint64_t> link_rx_by_slot;
    bool link_probe = false;   // RETRORUN_LINK_PROBE=1: synthetic 2.3 KB frames, a transport check without a game in wireless mode
    uint64_t probe_tx = 0, probe_rx = 0, probe_bad = 0;
    void finish();
    /* ---- cheats (--cheats; cheats.h). Offline: polled ~2x/s, edits apply live. Online: fixed at launch, hashed in the handshake. ---- */
    cheats::CheatSet cheat_boot;
    cheats::FileWatch cheat_watch;
    cheats::Target cheat_target = cheats::Target::GBA;
    bool cheats_on = false;
    uint64_t cheat_next_poll = 0;
    std::string cheat_applied_sig;
    void cheats_init();
    cheevos::Session ra_;   // RetroAchievements (cheevos.cpp); inactive unless --ra-cfg and offline
    void ra_start();
    void cheats_apply(const cheats::CheatSet& set);
    void cheats_poll();
    /* ---- multi-disc (libretro disk control). Offline a swap applies at once; with rollback netplay it travels as a one-shot event in the
     * input stream (ps1_input.h) and every PC applies it on the same simulated frame. ---- */
    cheats::FileWatch disc_watch;
    uint64_t disc_next_poll = 0;
    unsigned disc_want_ = 0;     // disc the next request starts from (the last one requested)
    bool disc_want_init_ = false;
    uint64_t disc_ev_pending_ = 0, disc_ev_now_ = 0;   // rollback: event queued by the user / the one carried by this frame's local input
    bool swap_combo_prev_ = false;
    size_t swap_at_i_ = 0;
    void disc_request();
    void disc_apply(unsigned target, bool visible);
    void disc_poll();
    void ctl_poll();
    bool take_screenshot(std::string* where);
    runctl::Poller ctl_;
    void do_pacing();
    int selftest();

    uint32_t local_pad_cache_[4] = {0, 0, 0, 0};
};

static Frontend* g_fe = nullptr;

void Frontend::ra_start() {
    if (a.ra_cfg.empty() || !a.net.empty() || a.ds_link || link || a.synctest || a.selftest || a.rollback) return;   // offline play only
    cheevos::Params p;
    if (!cheevos::load_config(a.ra_cfg, &p.cfg)) { rlog("cheevos: cannot read %s", a.ra_cfg.c_str()); return; }
    p.cache_dir = a.ra_cache;
    p.game_key = a.game_key;
    p.game_title_hint = a.title;
    p.system = lower(a.system);
    p.rom_path = a.rom;
    cheevos::Host h;
    h.http = cheevos::real_http();
    h.mmap = core.memory_map();
    h.core_memory = [this](unsigned id, uint8_t** d, size_t* n) { size_t sz = 0; void* m = core.memory(id, &sz); *d = (uint8_t*)m; *n = m ? sz : 0; };
    h.log = [](const std::string& m) { rlog("%s", m.c_str()); };
    ra_.start(p, h);
}

void Frontend::cheats_init() {
    if (a.cheats.empty()) return;
    if (a.ds_link) { rlog("cheats: ignored in DS wireless sessions (every player would need identical state)"); return; }
    if (!core.cheats_supported()) { rlog("cheats: this core has no cheat support; --cheats ignored"); return; }
    if (!cheats::target_for_system(a.system, &cheat_target)) { rlog("cheats: no cheat format known for system '%s'", a.system.c_str()); return; }
    std::string err;
    if (!cheats::load_file(a.cheats, &cheat_boot, &err)) { rlog("cheats: cannot read %s (%s); playing without cheats", a.cheats.c_str(), err.c_str()); return; }
    cheats_on = true;
    cheat_watch = cheats::FileWatch(a.cheats);
    rlog("cheats: %zu in %s%s", cheat_boot.cheats.size(), a.cheats.c_str(), a.net.empty() ? "" : " (online: fixed for the session)");
    cheats_apply(cheat_boot);
}

void Frontend::cheats_apply(const cheats::CheatSet& set) {
    if (ra_.active() && !ra_.allow(cheevos::Feature::Cheats)) return;   // hardcore
    // The core only has "reset all" + "set slot": rebuild the whole table, but only when the effective list changed.
    std::vector<std::pair<std::string, const cheats::Cheat*>> good;
    std::string sig;
    for (const cheats::Cheat& c : set.cheats) {
        std::string code, err;
        if (!cheats::retro_code_string(cheat_target, c, &code, &err)) { if (c.enabled) rlog("cheats: skipping '%s' (%s): %s", c.id.c_str(), c.desc.c_str(), err.c_str()); continue; }
        if (!c.enabled) continue;   // some cores (mGBA) ignore the enabled flag of retro_cheat_set: only hand over active cheats
        good.push_back({code, &c});
        sig += (c.enabled ? "1" : "0") + code + "|";
    }
    if (sig == cheat_applied_sig) return;
    cheat_applied_sig = sig;
    core.cheat_reset();
    unsigned idx = 0, on = 0;
    for (auto& g : good) {
        core.cheat_set(idx++, true, g.first.c_str());
        { on++; rlog("cheats: enabled '%s' (%s): %s", g.second->id.c_str(), g.second->desc.c_str(), g.first.c_str()); }
    }
    rlog("cheats: applied, %u enabled", on);
}

void Frontend::cheats_poll() {
    if (!cheats_on) return;
    uint64_t now = SDL_GetTicks64();
    if (now < cheat_next_poll) return;
    cheat_next_poll = now + 500;
    if (!cheat_watch.changed()) return;
    if (!a.net.empty()) { rlog("cheats: file changed during an online session: ignored (all peers must run the same list)"); return; }
    cheats::CheatSet set; std::string err;
    if (!cheats::load_file(a.cheats, &set, &err)) { rlog("cheats: cannot reload %s: %s", a.cheats.c_str(), err.c_str()); return; }
    cheats_apply(set);
}

/* The user asked for the next disc (hotkey / pad combo / RetroHub overlay). */
void Frontend::disc_request() {
    unsigned n = core.disc_count();
    if (n < 2) { set_status(n ? "This game has only one disc" : "This core cannot swap discs"); return; }
    if (!disc_want_init_) { disc_want_ = core.disc_index(); disc_want_init_ = true; }
    if (online && !rbe) { set_status("Disc swapping needs rollback netplay"); return; }
    unsigned target = (disc_want_ + 1) % n;
    disc_want_ = target;
    if (rbe) disc_ev_pending_ = retrorun::disc_event(target);   // applied by every PC on the same frame (rollback input stream)
    else disc_apply(target, true);
}

/* Insert disc `target`: eject, select, close the tray. Idempotent, so a re-simulated frame may repeat it. `visible` = not a rollback re-simulation. */
void Frontend::disc_apply(unsigned target, bool visible) {
    if (!core.disc_set(target)) { if (visible) rlog("disc: swap to %u failed", target + 1); return; }
    disc_want_ = target; disc_want_init_ = true;
    if (visible) {
        rlog("disc: Disc %u inserted (frame %llu, core disc %u of %u)", target + 1, (unsigned long long)frame_no, core.disc_index() + 1, core.disc_count());
        set_status("Disc " + std::to_string(target + 1) + " inserted");
    }
}

void Frontend::disc_poll() {
    while (swap_at_i_ < a.swap_at.size() && frame_no >= a.swap_at[swap_at_i_]) { swap_at_i_++; disc_request(); }
    if (!a.disc_ctl.empty()) {
        uint64_t now = SDL_GetTicks64();
        if (now >= disc_next_poll) {
            disc_next_poll = now + 250;
            if (disc_watch.changed()) disc_request();
        }
    }
}

void Frontend::set_status(const std::string& s) {
    rlog("%s", s.c_str());
    status_msg = s;
    status_until = SDL_GetTicks64() + 2500;
    if (win) SDL_SetWindowTitle(win, (window_base() + "  |  " + s).c_str());
    osd::push(s, osd_kind_for(s));
}

bool Frontend::setup_paths() {
    std::error_code ec;
    fs::create_directories(fs::u8path(a.save_dir), ec);
    fs::create_directories(fs::u8path(a.state_dir), ec);
    if (a.system_dir.empty()) {
        a.system_dir = narrow((fs::path(winutil::exe_path_w()).parent_path() / L"system").wstring().c_str());
    }
    fs::create_directories(fs::u8path(a.system_dir), ec);
    core.system_dir = a.system_dir;
    core.save_dir = a.save_dir;
    userdata_dir = narrow((fs::path(winutil::exe_path_w()).parent_path() / L"userdata").wstring().c_str());
    core.content_dir = narrow(fs::u8path(a.rom).parent_path().wstring().c_str());
    core.system_tag = a.system;
    is_ds = lower(a.system) == "nds";
    is_ps1 = lower(a.system) == "ps1";
    is_ps2 = lower(a.system) == "ps2";
    scale_filter_ = a.scale_filter;
    if (a.integer_scale >= 0) integer_scale_ = a.integer_scale != 0;
    fx::parse_filter(a.fx, &fx_filter_);
    bezel_on_ = a.bezel != 0;
    // Defaults for the DS core go in first (then the remembered layout) so ini / --opt below override them.
    if (is_ds) {
        // The DS core keeps its own files (Wi-Fi settings, SD card images, ...) in a scratch dir under userdata, never in
        // the vault: the vault battery folder holds only the .srm that retrorun writes.
        core.save_dir = userdata_dir + "\\core-save\\nds";
        std::error_code cec;
        fs::create_directories(fs::u8path(core.save_dir), cec);
        dsview::for_each_default_option([this](const char* k, const char* v) { core.overrides[k] = v; });
        std::string want = load_layout_pref();
        int li = dsview::layout_index(want);
        if (li >= 0) { ds_layout = li; core.overrides["melonds_screen_layout1"] = want; }
    }
    // mGBA's idle-loop removal state is not part of its savestates, so a rolled-back run would diverge from a straight one.
    if (a.rollback || a.synctest) core.overrides["mgba_idle_optimization"] = "Don't Remove";
    if (a.link) {
        // Deterministic link defaults (ini / --opt below may still override): cycle-based RTC, side-by-side layout, my console's audio.
        int mine = 0;
        for (int i = 0; i < 4; i++) if (owner[i] == a.slot) { mine = i; break; }
        core.overrides["sameboy_link"] = "enabled";
        core.overrides["sameboy_rtc"] = "accurate";
        core.overrides["sameboy_screen_layout"] = "left-right";
        core.overrides["sameboy_audio_output"] = "Game Boy #" + std::to_string(std::min(1, mine) + 1);
    }
    if (lower(a.system) == "nes" && (a.ports[2] >= 0 || a.ports[3] >= 0 || (a.net.empty() && a.players >= 3)))
        core.overrides["nestopia_select_adapter"] = "ntsc";   // 3-4 players: the Four Score adapter (every PC derives it from the same port plan)
    if (lower(a.system) == "nes" && (!a.net.empty() || a.synctest)) core.overrides["nestopia_ram_power_state"] = "0x00";   // random power-on RAM would differ per PC
    if ((!a.net.empty() || a.synctest) && !a.link && !is_ds) core.overrides["sameboy_rtc"] = "accurate";   // cycle-based clock: wall time would desync lockstep
    rom_stem = a.save_stem.empty() ? fs::u8path(a.rom).stem().u8string() : a.save_stem;
    srm_path = a.save_dir + "\\" + rom_stem + ".srm";
    // The save folder is per game (same key on every PC), but the ROM file name is not: the same game
    // copied under another file name (e.g. "Other Name (USA).gba" on PC B) would look for its own .srm and
    // miss the synced save. If there is no <stem>.srm and exactly one .srm here, that is this game's save.
    // Only for a vault per-game battery dir (...\battery, or a game dir with meta.json one level up); in any other
    // folder a lone .srm may belong to a different game, so do not guess.
    bool vault_battery = false;
    {
        std::error_code vec;
        fs::path sdp = fs::u8path(a.save_dir);
        while (sdp.has_filename() == false && sdp.has_parent_path() && sdp != sdp.parent_path()) sdp = sdp.parent_path();
        vault_battery = lower(sdp.filename().u8string()) == "battery" ||
                        (sdp.has_parent_path() && fs::exists(sdp.parent_path() / "meta.json", vec));
    }
    if (vault_battery && !fs::exists(fs::u8path(srm_path))) {
        std::error_code sec;
        std::string only;
        int found = 0;
        for (auto& de : fs::directory_iterator(fs::u8path(a.save_dir), sec)) {
            if (sec || !de.is_regular_file(sec)) continue;
            if (lower(de.path().extension().u8string()) == ".srm") { only = de.path().u8string(); found++; }
        }
        if (found == 1) {
            rlog("save: no %s.srm for this file name; using the save here: %s", rom_stem.c_str(), only.c_str());
            srm_path = only;
        }
    }

    // Option overrides: ini next to the parent of the save dir, then in the save dir, then --ini, then --opt.
    fs::path sd = fs::u8path(a.save_dir);
    if (sd.has_parent_path()) load_ini(narrow((sd.parent_path() / L"retrorun.ini").wstring().c_str()), a.system, &core.overrides);
    load_ini(narrow((sd / L"retrorun.ini").wstring().c_str()), a.system, &core.overrides);
    if (!a.ini.empty()) load_ini(a.ini, a.system, &core.overrides);
    for (auto& o : a.opts) {
        size_t eq = o.find('=');
        if (eq != std::string::npos) core.overrides[trim(o.substr(0, eq))] = trim(o.substr(eq + 1));
    }
    if (is_ps1 && (!a.net.empty() || a.synctest)) {
        /* Online (and SyncTest): every PC must run the identical emulation AND produce the identical picture, because the picture
         * hash is the desync check. Pin everything that changes timing, the video output or the state, whatever the user chose
         * offline: the software renderer's resolution / widescreen / PGXP / dither, overclocks, CD speed and access method (the
         * threaded async reader retries sectors by wall-clock), the on-screen messages and FPS counter (drawn into the frame),
         * crop settings, analog-mode toggle, frame duping (the repeated frame depended on the local history), the second memory
         * card, and the deinterlacer: "off" rasterises both fields into the surface, with no previous-field state outside the
         * savestate (weave / FastMAD read it; verified with SyncTest on a 480i picture). */
        static const char* const pins[][2] = {
            {"beetle_psx_internal_resolution", "1x(native)"}, {"beetle_psx_widescreen_hack", "disabled"}, {"beetle_psx_pgxp_mode", "disabled"},
            {"beetle_psx_pgxp_2d_tol", "disabled"}, {"beetle_psx_pgxp_nclip", "disabled"}, {"beetle_psx_pgxp_texture", "disabled"},
            {"beetle_psx_dither_mode", "1x(native)"}, {"beetle_psx_cpu_freq_scale", "100%"}, {"beetle_psx_gpu_overclock", "1x(native)"},
            {"beetle_psx_gte_overclock", "disabled"}, {"beetle_psx_cd_fastload", "2x(native)"}, {"beetle_psx_cd_access_method", "sync"},
            {"beetle_psx_display_osd", "disabled"}, {"beetle_psx_display_internal_fps", "disabled"}, {"beetle_psx_frame_duping", "disabled"},
            {"beetle_psx_deinterlacer", "off"}, {"beetle_psx_crop_overscan", "smart"}, {"beetle_psx_image_crop", "disabled"},
            {"beetle_psx_image_offset", "disabled"}, {"beetle_psx_analog_toggle", "disabled"}, {"beetle_psx_analog_calibration", "disabled"},
            {"beetle_psx_enable_memcard1", "disabled"}, {"beetle_psx_shared_memory_cards", "disabled"}, {"beetle_psx_skip_bios", "disabled"},
            {"beetle_psx_override_bios", "disabled"}, {"beetle_psx_region", "auto"}, {"beetle_psx_core_timing_fps", "auto_toggle"},
            {"beetle_psx_pal_video_timing_override", "disabled"}, {"beetle_psx_line_render", "default"}, {"beetle_psx_aspect_ratio", "corrected"},
            {"beetle_psx_initial_scanline", "0"}, {"beetle_psx_initial_scanline_pal", "0"}, {"beetle_psx_last_scanline", "239"},
            {"beetle_psx_last_scanline_pal", "287"}, {"beetle_psx_use_mednafen_memcard0_method", "libretro"}};
        for (auto& pin : pins) core.overrides[pin[0]] = pin[1];
    }
    if (is_ps2 && (!a.net.empty() || a.synctest)) {
        /* PS2 online: the software renderer (no GPU involved, bit-identical on every PC), no MTVU / renderer threads (thread timing), fast boot
         * (the BIOS logo is skipped identically everywhere). Whatever the user chose offline, these are pinned; the rest of the pcsx2_* options
         * are part of the setup fingerprint (ps2_config_check). */
        static const char* const pins2[][2] = {
            {"pcsx2_renderer", "Software (SW)"}, {"pcsx2_mtvu", "disabled"}, {"pcsx2_sw_renderer_threads", "0"}, {"pcsx2_fastboot", "enabled"},
            {"pcsx2_analog_mode1", "enabled"}, {"pcsx2_analog_mode2", "enabled"}, {"pcsx2_axis_deadzone1", "0%"}, {"pcsx2_axis_deadzone2", "0%"},
            {"pcsx2_axis_scale1", "100%"}, {"pcsx2_axis_scale2", "100%"}, {"pcsx2_button_deadzone1", "0%"}, {"pcsx2_button_deadzone2", "0%"},
            {"pcsx2_invert_left_stick1", "disabled"}, {"pcsx2_invert_left_stick2", "disabled"}, {"pcsx2_invert_right_stick1", "disabled"},
            {"pcsx2_invert_right_stick2", "disabled"}, {"pcsx2_enable_rumble1", "disabled"}, {"pcsx2_enable_rumble2", "disabled"},
            {"pcsx2_shared_memory_cards", "enabled"}, {"pcsx2_enable_cheats", "disabled"}, {"pcsx2_dev9_eth", "disabled"}, {"pcsx2_dev9_hdd", "disabled"},
            {"pcsx2_usb_camera", "disabled"}, {"pcsx2_ee_cpu", "Recompiler"}};
        for (auto& pin : pins2) core.overrides[pin[0]] = pin[1];
    }
    return true;
}

/* ------------------------------------------------------------------ video */

void Frontend::layout(SDL_Rect* dst) {
    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    layout_for(ow, oh, dst);
}

void Frontend::layout_for(int ow, int oh, SDL_Rect* dst) {
    const FrameBuf& f = core.frame();
    double aspect = core.av().geometry.aspect_ratio;
    if (aspect <= 0.f) aspect = (double)f.w / std::max(1, f.h);
    int fh = f.h;
    if (link) { SDL_Rect v = view_src(); aspect = (double)v.w / std::max(1, v.h); fh = v.h; }
    dsview::Rect r = dsview::fit(ow, oh, fh, aspect, !is_ds && !is_ps1 && integer_scale_);   // integer scale when it fits (not for DS)
    dst->x = r.x; dst->y = r.y; dst->w = r.w; dst->h = r.h;
}

/* Cmd::Overlay: FPS (+ ping / input delay / rollback re-sim rate when online) in the OSD's top-left slot. Display only. */
void Frontend::perf_tick() {
    if (!perf_on_) return;
    uint64_t now = SDL_GetTicks64();
    if (!perf_fps_.frame(now)) return;
    perfov::Sample s;
    s.fps = perf_fps_.fps();
    if (rbe) {
        s.rtt_ms = rbe->rtt_ms();
        s.delay = rbe->delay();
        s.resim_per_s = perf_rb_.update(now, rbe->stats().resim_frames);
    } else if (lockstep) {
        s.delay = lockstep->current_delay();
    }
    osd::set_stats(perfov::format(s));
}

void Frontend::present() {
    perf_tick();
    const FrameBuf& f = core.frame();
    if (!ren || f.px.empty()) return;
    clip_frame();
    if (!tex || tex_w != f.w || tex_h != f.h) {
        if (tex) SDL_DestroyTexture(tex);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, (scale_filter_.empty() ? (is_ds || is_ps1) : scale_filter_ == "linear") ? "linear" : "nearest");
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, f.w, f.h);
        tex_w = f.w;
        tex_h = f.h;
        if (!tex) { rlog("video: texture create failed: %s", SDL_GetError()); return; }
    }
    SDL_UpdateTexture(tex, nullptr, f.px.data(), f.w * 4);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_Rect dst;
    layout(&dst);
    last_dst = dst;
    if (fx_draw(dst)) {}   // bezel and/or screen filter (present_fx.cpp); otherwise the classic copy below
    else if (link) { SDL_Rect v = view_src(); SDL_RenderCopy(ren, tex, &v, &dst); }
    else SDL_RenderCopy(ren, tex, nullptr, &dst);
    if (is_ds && pad_cursor.active && last_dst.w > 0) {   // pad stylus marker
        int cx = last_dst.x + (int)((pad_cursor.x * 0.5 + 0.5) * last_dst.w), cy = last_dst.y + (int)((pad_cursor.y * 0.5 + 0.5) * last_dst.h);
        SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
        SDL_Rect h{cx - 7, cy - 1, 15, 3}, v{cx - 1, cy - 7, 3, 15};
        SDL_RenderFillRect(ren, &h); SDL_RenderFillRect(ren, &v);
        SDL_SetRenderDrawColor(ren, pad_touch ? 255 : 0, 0, 0, 255);
        SDL_Rect c{cx - 1, cy - 1, 3, 3};
        SDL_RenderFillRect(ren, &c);
    }
    draw_hint();
    draw_osd();
    SDL_RenderPresent(ren);
}

/* ------------------------------------------------------------------ screen filters + bezel (present_fx.cpp) */

bool Frontend::fx_draw(const SDL_Rect& dst) {
    if (fx_filter_ == fx::Filter::Off && !bezel_on_) return false;
    if (dst.w <= 0 || dst.h <= 0) return false;
    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    if (bezel_on_ && ow > 0 && oh > 0) {
        std::string key = a.system + "|" + std::to_string(ow) + "x" + std::to_string(oh) + "|" + std::to_string(dst.x) + "," + std::to_string(dst.y) + "," + std::to_string(dst.w) + "," + std::to_string(dst.h);
        if (!bezel_tex_ || bezel_tex_w_ != ow || bezel_tex_h_ != oh || key != bezel_key_) {
            const auto& bz = fxe_.bezel(a.system, ow, oh, fx::Rect{dst.x, dst.y, dst.w, dst.h});
            if (!bezel_tex_ || bezel_tex_w_ != ow || bezel_tex_h_ != oh) {
                if (bezel_tex_) SDL_DestroyTexture(bezel_tex_);
                bezel_tex_ = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, ow, oh);
                bezel_tex_w_ = ow; bezel_tex_h_ = oh;
            }
            if (bezel_tex_) SDL_UpdateTexture(bezel_tex_, nullptr, bz.data(), ow * 4);
            bezel_key_ = key;
        }
        if (bezel_tex_) SDL_RenderCopy(ren, bezel_tex_, nullptr, nullptr);
    }
    if (fx_filter_ == fx::Filter::Off) return false;   // bezel only: the plain frame copy follows
    const FrameBuf& f = core.frame();
    SDL_Rect v{0, 0, f.w, f.h};
    if (link) v = view_src();
    fx::Src src{f.px.data() + (size_t)v.y * f.w + v.x, f.w, v.w, v.h};
    fxe_.render(fx_filter_, src, dst.w, dst.h, &fx_px_);
    if (!fx_tex_ || fx_tex_w_ != dst.w || fx_tex_h_ != dst.h) {
        if (fx_tex_) SDL_DestroyTexture(fx_tex_);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
        fx_tex_ = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, dst.w, dst.h);
        fx_tex_w_ = dst.w; fx_tex_h_ = dst.h;
        if (!fx_tex_) return false;
    }
    SDL_UpdateTexture(fx_tex_, nullptr, fx_px_.data(), dst.w * 4);
    SDL_RenderCopy(ren, fx_tex_, nullptr, &dst);
    return true;
}

/* runner_ctl "shader <name>": off|crt|lcd|dmg|sharp, "next" (cycle), "bezel:on|off|toggle". */
bool Frontend::fx_set(const std::string& what, std::string* msg) {
    std::string w = lower(trim(what));
    if (w.rfind("bezel", 0) == 0) {
        std::string arg = w.size() > 6 ? w.substr(6) : "toggle";
        if (arg == "on" || arg == "1") bezel_on_ = true;
        else if (arg == "off" || arg == "0") bezel_on_ = false;
        else if (arg == "toggle") bezel_on_ = !bezel_on_;
        else { *msg = "unknown bezel option"; return false; }
        *msg = bezel_on_ ? "Bezel on" : "Bezel off";
        return true;
    }
    fx::Filter f;
    if (w == "next") f = fx::next_filter(fx_filter_);
    else if (!fx::parse_filter(w, &f)) { *msg = "unknown shader"; return false; }
    fx_filter_ = f;
    *msg = std::string("Filter: ") + fx::filter_label(f);
    return true;
}

FrameBuf Frontend::fx_compose(const FrameBuf& f, int ow, int oh) {
    FrameBuf out;
    out.w = ow; out.h = oh;
    SDL_Rect dst{0, 0, 0, 0};
    layout_for(ow, oh, &dst);
    if (bezel_on_ && dst.w > 0) out.px = fxe_.bezel(a.system, ow, oh, fx::Rect{dst.x, dst.y, dst.w, dst.h});
    else out.px.assign((size_t)ow * oh, 0xFF000000u);
    if (dst.w <= 0 || dst.h <= 0 || f.px.empty()) return out;
    if (fx_filter_ != fx::Filter::Off) {
        fx_px_.clear();
        fxe_.render(fx_filter_, fx::Src{f.px.data(), f.w, f.w, f.h}, dst.w, dst.h, &fx_px_);
    } else {
        fx_px_.resize((size_t)dst.w * dst.h);
        for (int y = 0; y < dst.h; y++) for (int x = 0; x < dst.w; x++) fx_px_[(size_t)y * dst.w + x] = f.px[(size_t)(y * f.h / dst.h) * f.w + x * f.w / dst.w];
    }
    for (int y = 0; y < dst.h; y++) {
        int oy = dst.y + y;
        if (oy < 0 || oy >= oh) continue;
        for (int x = 0; x < dst.w; x++) { int ox = dst.x + x; if (ox >= 0 && ox < ow) out.px[(size_t)oy * ow + ox] = fx_px_[(size_t)y * dst.w + x]; }
    }
    return out;
}

void Frontend::draw_osd() {
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(ren, &w, &h);
    bool changed = false;
    if (w <= 0 || h <= 0 || !osd::render(&osd_layer, w, h, &changed)) return;
    if (!osd_tex || osd_tex_w != w || osd_tex_h != h) {
        if (osd_tex) SDL_DestroyTexture(osd_tex);
        osd_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!osd_tex) return;
        SDL_SetTextureBlendMode(osd_tex, SDL_BLENDMODE_BLEND);
        osd_tex_w = w; osd_tex_h = h;
        changed = true;
    }
    if (changed) SDL_UpdateTexture(osd_tex, nullptr, osd_layer.px.data(), w * 4);
    SDL_RenderCopy(ren, osd_tex, nullptr, nullptr);
}

void Frontend::apply_speed(double s) {
    if (!ra_.allow_speed(s == 0 ? 2.0 : s)) return;   // hardcore: only 1x
    speed_max = (s == 0);
    speed = speed_max ? 1.0 : s;
    if (s == 0 || s > 1.0) fast_speed_ = s;
    ff = speed_max;
    core.set_fast_forwarding(ff);
    audio.set_speed(speed);
    next_deadline = 0;   // re-anchor the timer pacing at the new rate
    osd::push(runctl::speed_label(s));
    rlog("speed: %s", runctl::speed_label(s).c_str());
}

void Frontend::draw_hint() {
    static const char* off = std::getenv("RETROHUB_START_HINT");
    if (hint_done || (off && off[0] == '0')) return;
    uint64_t now = SDL_GetTicks64();
    if (!hint_t0) hint_t0 = now;
    long long el = (long long)(now - hint_t0);
    if (starthint::finished(el)) {
        hint_done = true;
        if (hint_tex) { SDL_DestroyTexture(hint_tex); hint_tex = nullptr; }
        return;
    }
    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    if (ow <= 0 || oh <= 0) return;
    if (!hint_tex || hint_for_h != oh) {
        if (hint_tex) SDL_DestroyTexture(hint_tex);
        starthint::Bitmap b = starthint::render(starthint::pick_mode(pads.empty(), true), starthint::scale_for_height(oh));
        hint_tex = b.px.empty() ? nullptr : SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, b.w, b.h);
        if (!hint_tex) { hint_done = true; return; }
        SDL_UpdateTexture(hint_tex, nullptr, b.px.data(), b.w * 4);
        SDL_SetTextureBlendMode(hint_tex, SDL_BLENDMODE_BLEND);
        hint_w = b.w; hint_h = b.h; hint_for_h = oh;
    }
    starthint::Bitmap geo; geo.w = hint_w; geo.h = hint_h;
    int x, y;
    starthint::placement(ow, oh, geo, &x, &y);
    SDL_SetTextureAlphaMod(hint_tex, (Uint8)(starthint::alpha_at(el) / starthint::kMaxAlpha * 255.f));
    SDL_Rect dst{x, y, hint_w, hint_h};
    SDL_RenderCopy(ren, hint_tex, nullptr, &dst);
}

bool Frontend::setup_video() {
    if (a.headless) return true;
    const retro_game_geometry& g = core.av().geometry;
    double aspect = g.aspect_ratio > 0 ? g.aspect_ratio : (double)g.base_width / g.base_height;
    int w = (int)std::lround(g.base_height * a.scale * aspect), h = (int)(g.base_height * a.scale);
    if (link) { w = 160 * a.scale; h = 144 * a.scale; }
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    win = SDL_CreateWindow(window_base().c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, flags);
    if (!win) { rlog("video: window failed: %s", SDL_GetError()); return false; }

    // Vsync-paced when the display refresh is within 5% of the core's frame rate; otherwise audio/timer paced.
    SDL_DisplayMode dm{};
    double hz = 0;
    if (SDL_GetWindowDisplayMode(win, &dm) == 0) hz = dm.refresh_rate;
    fps = core.av().timing.fps > 1 ? core.av().timing.fps : 60.0;
    bool use_vsync = hz > 0 && std::fabs(hz / fps - 1.0) < 0.05;
    if (!a.pace_override.empty()) use_vsync = a.pace_override == "vsync";
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | (use_vsync ? SDL_RENDERER_PRESENTVSYNC : 0));
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) { rlog("video: renderer failed: %s", SDL_GetError()); return false; }
    SDL_RendererInfo ri{};
    SDL_GetRendererInfo(ren, &ri);
    if (use_vsync && !(ri.flags & SDL_RENDERER_PRESENTVSYNC)) use_vsync = false;
    pace = use_vsync ? Pace::Vsync : (audio.active() && a.pace_override != "timer" ? Pace::Audio : Pace::Timer);
    rlog("video: %dx%d window, display %.2f Hz, core %.3f fps, pacing=%s", w, h, hz, fps,
         pace == Pace::Vsync ? "vsync+DRC" : pace == Pace::Audio ? "audio" : "timer");
    if (a.fullscreen) toggle_fullscreen();
    return true;
}

void Frontend::toggle_fullscreen() {
    if (!win) return;
    fullscreen = !fullscreen;
    SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    SDL_ShowCursor(fullscreen && !is_ds ? SDL_DISABLE : SDL_ENABLE);   // DS: the cursor is the stylus
}

/* ------------------------------------------------------------------ input */

void Frontend::open_pads() {
    for (auto* p : pads) SDL_GameControllerClose(p);
    std::vector<std::string> names;
    pads = hubctl::open_pads(prof.port_order, &names);   // port order: the profile's pad list, else SDL order
    for (size_t i = 0; i < names.size(); i++) rlog("controller %zu: %s", i, names[i].c_str());
}

/* Pad for the i-th locally owned port. */
/* QA-only scripted input: RETRORUN_INPUT_SCRIPT=<file> with lines "frame:BUTTONS" (BUTTONS = names joined by '+', e.g. A+UP; empty/-/0 = none;
 * or 0x<hex> RetroPad mask). A mask applies from that frame until the next line. Used for every locally owned port. */
struct ScriptShot { uint64_t frame; std::string path; bool done; };
static std::vector<ScriptShot> g_script_shots;   // "frame:@shot <file.png>" lines: screenshot of the frame shown at that frame
static uint32_t scripted_pad(uint64_t frame, bool* active) {
    static bool loaded = false;
    static std::vector<std::pair<uint64_t, uint32_t>> tab;
    if (!loaded) {
        loaded = true;
        const char* f = getenv("RETRORUN_INPUT_SCRIPT");
        std::ifstream in(f ? f : "");
        std::string ln;
        static const char* names[] = {"B", "Y", "SELECT", "START", "UP", "DOWN", "LEFT", "RIGHT", "A", "X", "L", "R"};
        while (f && std::getline(in, ln)) {
            size_t c = ln.find(':');
            if (c == std::string::npos || ln[0] == '#') continue;
            uint64_t fr = strtoull(ln.c_str(), nullptr, 10);
            std::string b = ln.substr(c + 1);
            while (!b.empty() && (b.back() == '\r' || b.back() == ' ')) b.pop_back();
            if (b.compare(0, 6, "@shot ") == 0) { g_script_shots.push_back({fr, b.substr(6), false}); continue; }
            uint32_t m = 0;
            if (b.size() > 2 && b[0] == '0' && b[1] == 'x') m = (uint32_t)strtoul(b.c_str(), nullptr, 16);
            else {
                size_t p = 0;
                while (p <= b.size()) {
                    size_t q = b.find('+', p); if (q == std::string::npos) q = b.size();
                    std::string t = b.substr(p, q - p); p = q + 1;
                    for (int k = 0; k < 12; k++) if (t == names[k]) m |= 1u << k;
                }
            }
            tab.push_back({fr, m});
        }
        std::stable_sort(tab.begin(), tab.end(), [](auto& x, auto& y) { return x.first < y.first; });
    }
    *active = !tab.empty() || !g_script_shots.empty();
    uint32_t m = 0;
    for (auto& e : tab) { if (e.first > frame) break; m = e.second; }
    return m;
}

uint32_t Frontend::poll_local(int i) {
    int port = local_ports[i];
    {
        bool act = false;
        uint32_t sm = scripted_pad(frame_no, &act);
        for (auto& sh : g_script_shots)
            if (!sh.done && sh.frame <= frame_no) { sh.done = true; if (write_frame_image(sh.path, core.frame())) rlog("screenshot: %s (frame %llu)", sh.path.c_str(), (unsigned long long)frame_no); }
        if (act) return sm;
    }
    if (a.fuzz) {
        uint32_t x = (uint32_t)(frame_no / 3) * 2654435761u ^ (uint32_t)(port + 1) * 40503u ^ 0x9E3779B9u;
        x ^= x >> 15; x *= 2246822519u; x ^= x >> 13;
        return x & 0xFF3u & ~0xCu;   // never Start/Select
    }
    if (a.headless) return 0;
    uint32_t m = 0;
    if (i == 0 && retro_pad) m |= retro_pad->key_mask(SDL_GetKeyboardState(nullptr), frame_no, hubctl::turbo_allowed(online, a.turbo_online));
    if ((size_t)i < pads.size()) {
        hubctl::PadState st;
        hubctl::read_state(pads[i], &st);
        m |= retro_pad->mask(st, frame_no, hubctl::turbo_allowed(online, a.turbo_online));
    }
    return m;
}

void Frontend::poll_all_local() {
    if (!a.headless && !a.fuzz) {
        bool combo = false;
        for (size_t i = 0; i < pads.size(); i++) {
            hubctl::PadState st;
            hubctl::read_state(pads[i], &st);
            if (hubctl::exit_combo_down(st)) combo = true;
            if (i == 0 && is_ds) {
                uint64_t now = SDL_GetTicks64();
                double dt = cursor_tick ? std::min(0.1, (now - cursor_tick) / 1000.0) : 0.0;
                cursor_tick = now;
                pad_cursor.step(st.a[2] / 32767.0, st.a[3] / 32767.0, dt);   // rightx, righty
                pad_touch = (retro_pad->mask(st) >> RETRO_DEVICE_ID_JOYPAD_R3) & 1;
            }
        }
        bool swap_combo = false;   // Back + L3 + R3: next disc
        for (size_t i = 0; i < pads.size(); i++) {
            hubctl::PadState st;
            hubctl::read_state(pads[i], &st);
            if (st.b[hubctl::kBtnBack] && st.b[7] && st.b[8]) swap_combo = true;
        }
        bool boost = false;   // View + RT held: quick boost at the last fast speed (offline only)
        for (size_t i = 0; i < pads.size(); i++) {
            hubctl::PadState st;
            hubctl::read_state(pads[i], &st);
            if (hubctl::boost_combo_down(st)) boost = true;
        }
        if (!online && !a.ds_link) {
            if (boost && !boost_active_ && speed == 1.0 && !speed_max) { boost_active_ = true; apply_speed(fast_speed_); }
            else if (!boost && boost_active_) { boost_active_ = false; apply_speed(1.0); }
        } else boost_active_ = false;
        if (swap_combo && !swap_combo_prev_) disc_request();
        swap_combo_prev_ = swap_combo;
        {   // View+LB tap: save the last 30 s (rising edge, 3 s cooldown)
            bool cc = false;
            for (size_t i = 0; i < pads.size(); i++) {
                hubctl::PadState st;
                hubctl::read_state(pads[i], &st);
                if (hubctl::clip_combo_down(st)) cc = true;
            }
            if (cc && !clip_combo_prev_ && SDL_GetTicks64() >= clip_cool_until_) { clip_cool_until_ = SDL_GetTicks64() + 3000; clip_request(); }
            clip_combo_prev_ = cc;
        }
        if (feat_) {   // hold LT + View (not with Start / Y: those are the exit and quick-menu combos) to rewind
            bool rw = false;
            for (size_t i = 0; i < pads.size(); i++) {
                hubctl::PadState st;
                hubctl::read_state(pads[i], &st);
                if (st.a[4] > hubctl::kDefaultAxisThreshold && st.b[hubctl::kBtnBack] && !st.b[hubctl::kBtnStart] && !st.b[3]) rw = true;
            }
            if (rw && !ra_.allow(cheevos::Feature::Rewind)) rw = false;   // hardcore: no rewind (OSD message)
            feat_->set_held(rw);
        }
        if (exit_combo.update(combo, (long long)SDL_GetTicks64())) { rlog("exit combo (Back+Start held): leaving"); quit = true; }
    }
    for (int i = 0; i < n_local; i++) {
        local_pad_cache_[i] = poll_local(i);
        std::memset(local_stick_[i], 0, sizeof local_stick_[i]);
        local_press_[i][0] = local_press_[i][1] = 0;
        if ((is_ps1 || is_ps2) && a.fuzz) {   // QA: pseudo-random sticks, a new target every 5 frames
            for (int k = 0; k < 4 && !std::getenv("RETRORUN_NOSTICKS"); k++) {
                uint32_t x = (uint32_t)(frame_no / 5) * 2246822519u ^ (uint32_t)(local_ports[i] * 4 + k + 1) * 3266489917u ^ 0x85EBCA6Bu;
                x ^= x >> 15; x *= 2654435761u; x ^= x >> 13;
                local_stick_[i][k] = (int16_t)((int)(x & 0xFFFF) - 32768);
            }
            if (is_ps2 && !std::getenv("RETRORUN_NOSTICKS")) {   // pressure-sensitive L2/R2 (a DualShock 2 reports 0..255): a new level every 7 frames
                uint32_t y = (uint32_t)(frame_no / 7) * 2654435761u ^ (uint32_t)(local_ports[i] + 7) * 0x9E3779B1u;
                y ^= y >> 13; y *= 3266489917u; y ^= y >> 16;
                local_press_[i][0] = (int16_t)((y & 3) ? (y >> 3) & 0xFF : 0);
                local_press_[i][1] = (int16_t)(((y >> 2) & 3) ? (y >> 11) & 0xFF : 0);
            }
        } else if ((is_ps1 || is_ps2) && !a.headless && (size_t)i < pads.size() && retro_pad) {
            hubctl::PadState st;
            hubctl::read_state(pads[i], &st);
            retro_pad->sticks(st, local_stick_[i]);
            if (is_ps2) retro_pad->pressure(st, local_press_[i]);
        }
        if (is_ps2) {   // buttons in the low 16 bits, L2/R2 pressure in the high 16; a pressed trigger is also its button bit
            uint32_t m = local_pad_cache_[i] & 0xFFFFu;
            for (int k = 0; k < 2; k++) {
                if (local_press_[i][k]) m |= 1u << (RETRO_DEVICE_ID_JOYPAD_L2 + k);
                else if (m >> (RETRO_DEVICE_ID_JOYPAD_L2 + k) & 1u) local_press_[i][k] = 255;
            }
            local_pad_cache_[i] = m | (uint32_t)(uint8_t)local_press_[i][0] << 16 | (uint32_t)(uint8_t)local_press_[i][1] << 24;
        }
    }
    if (!a.headless) {   // View+Y again re-enables; otherwise neutral local input while the hub menu is open (frames still flow online)
        bool vy = false;
        for (size_t i = 0; i < pads.size(); i++) { hubctl::PadState st; hubctl::read_state(pads[i], &st); if (st.b[hubctl::kBtnBack] && st.b[hubctl::kBtnYButton]) vy = true; }
        in_gate_.note_view_y(vy, SDL_GetTicks64());
        if (in_gate_.suppressed(SDL_GetTicks64()))
            for (int i = 0; i < n_local; i++) {
                local_pad_cache_[i] = 0;
                std::memset(local_stick_[i], 0, sizeof local_stick_[i]);
                local_press_[i][0] = local_press_[i][1] = 0;
            }
    }
    if (!online)
        for (int i = 0; i < n_local; i++) {
            frame_pad[local_ports[i]] = local_pad_cache_[i];
            std::memcpy(frame_stick[local_ports[i]], local_stick_[i], sizeof local_stick_[i]);
        }
}

void Frontend::handle_key(const SDL_KeyboardEvent& k) {
    if (k.repeat) return;
    switch (k.keysym.sym) {
        case SDLK_ESCAPE: quit = true; break;
        case SDLK_F11: toggle_fullscreen(); break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (k.keysym.mod & KMOD_ALT) toggle_fullscreen();
            break;
        case SDLK_F9:
            if (is_ds) {
                ds_layout = (ds_layout + 1) % dsview::layout_count();
                core.set_option("melonds_screen_layout1", dsview::layout_name(ds_layout));
                save_layout_pref(dsview::layout_name(ds_layout));
                set_status(std::string("Screen layout: ") + dsview::layout_name(ds_layout));
            }
            break;
        case SDLK_F10: disc_request(); break;
        case SDLK_F12: { std::string w; take_screenshot(&w); } break;
        case SDLK_F5: want_save = true; break;
        case SDLK_F7:
            if (online || a.ds_link) set_status("Loading states is disabled during netplay");
            else want_load = true;
            break;
        case SDLK_F6: slot_sel = (slot_sel + 9) % 10; set_status("State slot " + std::to_string(slot_sel)); break;
        case SDLK_F8: slot_sel = (slot_sel + 1) % 10; set_status("State slot " + std::to_string(slot_sel)); break;
        default: break;
    }
}

void Frontend::pump_events(bool idle) {
    if (a.headless) return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT: quit = true; break;
            case SDL_KEYDOWN: handle_key(e.key); break;
            case SDL_CONTROLLERDEVICEADDED:
            case SDL_CONTROLLERDEVICEREMOVED: open_pads(); break;
            default: break;
        }
    }
    const Uint8* k = SDL_GetKeyboardState(nullptr);
    ff = !online && !a.ds_link && ((k[SDL_SCANCODE_TAB] && ra_.allow(cheevos::Feature::FastForward)) || speed_max);
    core.set_fast_forwarding(ff);
    if (win && status_until && SDL_GetTicks64() > status_until) {
        status_until = 0;
        SDL_SetWindowTitle(win, window_base().c_str());
    }
    if (idle && quit) {
        // Blocked inside lockstep and the user asked to leave: save and go.
        finish();
        ExitProcess(0);
    }
}

/* ------------------------------------------------------------ saves/states */

void Frontend::load_srm() {
    size_t n = 0;
    void* mem = core.save_ram(&n);
    if (!mem || !n) { rlog("save: core exposes no battery RAM (yet)"); return; }
    std::vector<uint8_t> d;
    if (read_file(srm_path, &d)) {
        memcpy(mem, d.data(), std::min(n, d.size()));
        rlog("save: loaded %s (%zu bytes into %zu)", srm_path.c_str(), d.size(), n);
        if (d.size() != n) {
            // A synced save of another size (different core/dump) is never silently truncated or padded: keep the
            // original as <srm>.bak (once) before the first write replaces it.
            srm_size_mismatch = true;
            rlog("save: WARNING size mismatch: file is %zu bytes, core save RAM is %zu; the original will be kept as %s.bak",
                 d.size(), n, srm_path.c_str());
        }
    } else {
        rlog("save: no existing %s (battery RAM %zu bytes)", srm_path.c_str(), n);
    }
    srm_saved.assign((uint8_t*)mem, (uint8_t*)mem + n);
    // Cores that keep the cartridge clock apart from the battery RAM (SameBoy/Gambatte MBC3: Gold/Silver/Crystal) get <stem>.rtc.
    size_t nr = 0;
    void* rm = core.memory(RETRO_MEMORY_RTC, &nr);
    if (rm && nr) {
        std::vector<uint8_t> r;
        if (read_file(rtc_path(), &r) && r.size() == nr) { memcpy(rm, r.data(), nr); rlog("save: loaded clock %s (%zu bytes)", rtc_path().c_str(), nr); }
        rtc_saved.assign((uint8_t*)rm, (uint8_t*)rm + nr);
    }
}

/* --share-host-save (lockstep GB/GBC/GBA): slot 0 sends the save RAM + clock it booted with, every other PC overwrites its
 * own (throwaway-dir) save with those bytes before frame 0, so all PCs start from identical battery data. Guests never write
 * to a vault. Runs between "session ready" and delay negotiation. */
bool Frontend::share_host_save_run() {
    size_t n = 0, nr = 0;
    void* mem = core.save_ram(&n);
    void* rm = core.memory(RETRO_MEMORY_RTC, &nr);
    std::vector<n64run::SaveFile> files;
    if (a.slot == 0) {
        if (mem && n) files.push_back({"game.srm", std::vector<uint8_t>((uint8_t*)mem, (uint8_t*)mem + n)});
        if (rm && nr) files.push_back({"game.rtc", std::vector<uint8_t>((uint8_t*)rm, (uint8_t*)rm + nr)});
        for (auto& f : files)
            rlog("hostsave: host shares %s (%zu bytes, hash %016llx)", f.name.c_str(), f.data.size(), (unsigned long long)n64run::save_hash(f.data.data(), f.data.size()));
        if (files.empty()) rlog("hostsave: core exposes no battery RAM; nothing to share");
    } else {
        guest_save_ro = true;
    }
    std::string dir = a.save_dir + "\\hostsave";
    std::error_code ec;
    if (a.slot != 0) { fs::remove_all(fs::u8path(dir), ec); fs::create_directories(fs::u8path(dir), ec); }
    std::string err;
    if (!n64run::run_save_sync(session, a.slot, slot_mask, a.slot == 0 ? &files : nullptr, dir, 30000, &err)) {
        rlog("hostsave: save sync failed: %s", err.c_str());
        return false;
    }
    if (a.slot != 0) {
        std::vector<uint8_t> d;
        if (mem && n && read_file(dir + "\\game.srm", &d)) {
            if (d.size() != n) { rlog("hostsave: host save is %zu bytes but this core's save RAM is %zu: different core/game?", d.size(), n); return false; }
            memcpy(mem, d.data(), n);
            srm_saved.assign((uint8_t*)mem, (uint8_t*)mem + n);
            rlog("hostsave: booted from host save (%zu bytes, hash %016llx)", n, (unsigned long long)n64run::save_hash(d.data(), d.size()));
        }
        if (rm && nr && read_file(dir + "\\game.rtc", &d)) {
            if (d.size() != nr) { rlog("hostsave: host clock is %zu bytes but this core's is %zu", d.size(), nr); return false; }
            memcpy(rm, d.data(), nr);
            rlog("hostsave: booted from host clock (%zu bytes, hash %016llx)", nr, (unsigned long long)n64run::save_hash(d.data(), d.size()));
        }
    }
    rlog("hostsave: save sync complete");
    return true;
}

void Frontend::save_srm(bool force) {
    if (guest_save_ro) return;
    size_t n = 0;
    void* mem = core.save_ram(&n);
    if (!mem || !n) return;
    {
        size_t nr = 0;
        void* rm = core.memory(RETRO_MEMORY_RTC, &nr);
        if (rm && nr && (force || rtc_saved.size() != nr || memcmp(rtc_saved.data(), rm, nr) != 0) ) {
            if (write_atomic(rtc_path(), rm, nr)) rtc_saved.assign((uint8_t*)rm, (uint8_t*)rm + nr);
        }
    }
    if (!force && srm_saved.size() == n && memcmp(srm_saved.data(), mem, n) == 0) return;
    if (srm_size_mismatch) {
        srm_size_mismatch = false;
        std::error_code bec;
        fs::path bak = fs::u8path(srm_path + ".bak");
        if (!fs::exists(bak, bec)) {
            fs::copy_file(fs::u8path(srm_path), bak, bec);
            rlog("save: %s original kept as %s", bec ? "FAILED to keep" : "size-mismatched", bak.u8string().c_str());
        }
    }
    if (write_atomic(srm_path, mem, n)) {
        srm_saved.assign((uint8_t*)mem, (uint8_t*)mem + n);
        rlog("save: wrote %s (%zu bytes)", srm_path.c_str(), n);
    } else {
        rlog("save: FAILED to write %s", srm_path.c_str());
    }
}

bool Frontend::save_state(const std::string& name) {
    size_t n = core.serialize_size();
    if (!n) { set_status("Core does not support save states"); return false; }
    std::vector<uint8_t> buf(n);
    if (!core.serialize(buf.data(), n)) { set_status("Save state failed (core error)"); return false; }
    if (!write_atomic(state_path(name), buf.data(), n)) { set_status("Save state failed (disk)"); return false; }
    {   // thumbnail + sidecar next to the state (runner_ctl.h contract)
        write_frame_image_scaled(a.state_dir + "\\" + name + ".png", core.frame(), runctl::kThumbWidth);
        runctl::Sidecar sc;
        sc.game_key = a.game_key;
        sc.time = (long long)std::time(nullptr);
        sc.playtime = (long long)(SDL_GetTicks64() / 1000);
        sc.disc = is_ps1 ? (int)core.disc_index() : 0;
        runctl::write_text_atomic(a.state_dir + "\\" + name + ".json", runctl::make_sidecar(sc));
    }
    set_status("Saved state " + name);
    return true;
}

bool Frontend::take_screenshot(std::string* where) {
    if (a.shot_dir.empty()) { set_status("Screenshots are not available here"); return false; }
    std::string p = a.shot_dir + "\\" + runctl::shot_name(std::time(nullptr));
    if (!write_frame_image(p, core.frame())) { set_status("Screenshot failed"); return false; }
    if (where) *where = p;
    set_status("Screenshot saved");
    return true;
}

void Frontend::clip_request() {
    std::string m;
    if (clips_.request_save(std::time(nullptr), &m)) osd::push("Saving clip...");
    else osd::push(m, osd::Kind::Warn);
}

void Frontend::clip_poll() {
    bool ok = false; std::string m;
    if (!clips_.poll(&ok, &m)) return;
    rlog("clip: %s %s", ok ? "saved" : "FAILED", m.c_str());
    osd::push(ok ? "Clip saved" : "Clip failed: " + m, ok ? osd::Kind::Good : osd::Kind::Error);
}

void Frontend::ctl_poll() {
    clip_poll();
    runctl::Command c;
    if (!ctl_.poll(SDL_GetTicks64(), &c)) return;
    rlog("control: seq %lld %s slot %d", c.seq, runctl::cmd_name(c.cmd), c.slot);
    switch (c.cmd) {
        case runctl::Cmd::SaveState: {
            if (online || a.ds_link) { ctl_.ack(false, "Save states are disabled in online sessions"); break; }
            bool ok = save_state("slot" + std::to_string(c.slot));
            ctl_.ack(ok, ok ? "saved" : status_msg);
            break;
        }
        case runctl::Cmd::LoadState: {
            if (online || a.ds_link) { ctl_.ack(false, "Load state is disabled in online sessions"); break; }
            bool ok = load_state(c.slot < 0 ? "auto" : "slot" + std::to_string(c.slot));
            ctl_.ack(ok, ok ? "loaded" : status_msg);
            break;
        }
        case runctl::Cmd::Screenshot: {
            std::string w;
            bool ok = take_screenshot(&w);
            ctl_.ack(ok, ok ? w : status_msg);
            break;
        }
        case runctl::Cmd::Clip: {
            std::string m;
            bool ok = clips_.request_save(std::time(nullptr), &m);
            if (ok) { osd::push("Saving clip..."); m = "saving"; }
            else osd::push(m, osd::Kind::Warn);
            ctl_.ack(ok, m);
            break;
        }
        case runctl::Cmd::SwapDisc: disc_request(); ctl_.ack(true, "requested"); break;
        case runctl::Cmd::Quit: quit = true; ctl_.ack(true, "quitting"); break;
        case runctl::Cmd::OsdMsg: osd::push(c.text); ctl_.ack(true, "shown"); break;
        case runctl::Cmd::Input: in_gate_.set(c.value != 0, SDL_GetTicks64()); ctl_.ack(true, c.value ? "input on" : "input off"); break;
        case runctl::Cmd::Volume: case runctl::Cmd::Mute: case runctl::Cmd::Ff: case runctl::Cmd::Speed:
        case runctl::Cmd::Rewind: case runctl::Cmd::Shader: case runctl::Cmd::Overlay:
            if ((online || a.ds_link) && runctl::offline_only(c.cmd)) { ctl_.ack(false, "Not available in online sessions"); break; }
            if (c.cmd == runctl::Cmd::Volume) {
                audio.set_volume(c.value);
                osd::push("Volume " + std::to_string(c.value) + "%");
                ctl_.ack(true, "volume " + std::to_string(c.value));
            } else if (c.cmd == runctl::Cmd::Mute) {
                audio.set_mute(c.value != 0);
                osd::push(c.value ? "Muted" : "Sound on");
                ctl_.ack(true, c.value ? "muted" : "unmuted");
            } else if (c.cmd == runctl::Cmd::Ff) {
                boost_active_ = false;   // a menu choice made while View+RT is held wins: releasing the boost must not drop it back to 1x
                apply_speed(c.value ? fast_speed_ : 1.0);
                ctl_.ack(true, c.value ? "fast-forward on" : "fast-forward off");
            } else if (c.cmd == runctl::Cmd::Speed) {
                boost_active_ = false;
                apply_speed(c.speed);
                ctl_.ack(true, runctl::speed_label(c.speed));
            } else if (c.cmd == runctl::Cmd::Shader) {
                std::string m;
                bool ok = fx_set(c.text, &m);
                if (ok) osd::push(m);
                ctl_.ack(ok, m);
            } else if (c.cmd == runctl::Cmd::Rewind) {
                if (c.value && !ra_.allow(cheevos::Feature::Rewind)) { ctl_.ack(false, "Hardcore: rewind is off"); break; }
                bool ok = feat_ && feat_->set_latched(c.value != 0);
                ctl_.ack(ok, ok ? (c.value ? "rewinding" : "rewind off") : "Rewind is not available for this game");
            } else if (c.cmd == runctl::Cmd::Overlay) {
                perf_on_ = c.value != 0;
                perf_fps_.reset();
                if (!perf_on_) osd::set_stats("");
                ctl_.ack(true, perf_on_ ? "stats overlay on" : "stats overlay off");
            } else {
                ctl_.ack(false, "unsupported");
            }
            break;
        default: ctl_.ack(false, "unknown command"); break;
    }
}

bool Frontend::load_state(const std::string& name) {
    if (ra_.active() && !ra_.allow(cheevos::Feature::LoadState)) { set_status("Hardcore: save states are off"); return false; }
    std::vector<uint8_t> buf;
    if (!read_file(state_path(name), &buf)) { set_status("No state " + name); return false; }
    if (!core.unserialize(buf.data(), buf.size())) { set_status("Load state failed (core error)"); return false; }
    set_status("Loaded state " + name);
    ra_.reset();   // a restored moment is not progress: restart achievement trigger tracking
    return true;
}

void Frontend::fold_frame() {
    const FrameBuf& f = core.frame();
    uint64_t h = f.px.empty() ? 0 : n64run::hash_block(f.px.data(), f.px.size() * 4);
    out_hash = (out_hash ^ h) * 0x100000001B3ull;
    for (int p = 0; p < 4; p++) in_hash = (in_hash ^ frame_pad[p]) * 0x100000001B3ull;
}

/* Hash used for the periodic desync check. */
uint64_t Frontend::sync_hash() {
    if (is_ps2) {   // the PS2 core's serialized state holds host pointers (ASLR) and differs between identical runs: hash EE RAM instead
        size_t sz = 0;
        void* p = core.memory(RETRO_MEMORY_SYSTEM_RAM, &sz);
        return p && sz ? n64run::hash_block(p, sz) : out_hash;
    }
    return use_state_hash ? state_hash() : out_hash;
}

uint64_t Frontend::state_hash() {
    size_t n = core.serialize_size();
    if (!n) return 0;
    std::vector<uint8_t> buf(n);
    if (!core.serialize(buf.data(), n)) return 0;
    if (const char* dp = std::getenv("RB_DUMPSTATE")) { FILE* f = fopen(dp, "wb"); if (f) { fwrite(buf.data(), 1, buf.size(), f); fclose(f); } }   // QA: last hashed state
    return n64run::hash_block(buf.data(), buf.size());
}

/* ------------------------------------------------------------------ link mode */

/* SameBoy: console n (1-based) SRAM = ((2n-1)<<8)|0, RTC = (2n<<8)|1 (probed: 0x100/0x201, 0x300/0x401). */
static unsigned link_mem(int inst, unsigned type) { return (type == RETRO_MEMORY_RTC ? (unsigned)(inst + 1) * 2 : (unsigned)(inst + 1) * 2 - 1) << 8 | type; }

std::string Frontend::link_srm(int inst) const {
    int first = -1;
    for (int i = 0; i < link_n; i++) if (owner[i] == a.slot) { first = i; break; }
    if (inst == first || first < 0) return srm_path;
    return srm_path.substr(0, srm_path.size() - 4) + ".p" + std::to_string(inst + 1) + ".srm";
}

SDL_Rect Frontend::view_src() const {
    const FrameBuf& f = core.frame();
    int me = 0;
    for (int i = 0; i < link_n; i++) if (owner[i] == a.slot) { me = i; break; }
    if (f.w > f.h * 1.2) { int hw = f.w / link_n; return {hw * me, 0, hw, f.h}; }   // side by side
    int hh = f.h / link_n;
    return {0, hh * me, f.w, hh};                                                     // stacked
}

/* blob = u32 'LNK1', u32 sram_size, u32 rtc_size, sram, rtc */
std::vector<uint8_t> Frontend::link_blob_from_disk(int inst) const {
    std::vector<uint8_t> sram, rtc, out;
    read_file(link_srm(inst), &sram);
    read_file(link_rtc(inst), &rtc);
    auto p32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) out.push_back((uint8_t)(v >> (8 * i))); };
    p32(0x314B4E4C); p32((uint32_t)sram.size()); p32((uint32_t)rtc.size());
    out.insert(out.end(), sram.begin(), sram.end());
    out.insert(out.end(), rtc.begin(), rtc.end());
    return out;
}

void Frontend::link_apply_blob(int inst, const std::vector<uint8_t>& b) {
    auto g32 = [&](size_t o) { return (uint32_t)b[o] | (uint32_t)b[o + 1] << 8 | (uint32_t)b[o + 2] << 16 | (uint32_t)b[o + 3] << 24; };
    if (b.size() < 12 || g32(0) != 0x314B4E4C) { rlog("link: console %d: no usable save blob (%zu bytes), starting blank", inst + 1, b.size()); return; }
    uint32_t ns = g32(4), nr = g32(8);
    if (12ull + ns + nr != b.size()) { rlog("link: console %d: malformed blob", inst + 1); return; }
    size_t n = 0;
    void* m = core.memory(link_mem(inst, RETRO_MEMORY_SAVE_RAM), &n);
    if (m && n) memcpy(m, b.data() + 12, std::min<size_t>(n, ns));
    rlog("link: console %d: save RAM %u bytes -> core %zu (blob hash %016llx)", inst + 1, ns, n, (unsigned long long)rlink::blob_hash(b.data(), b.size()));
    m = core.memory(link_mem(inst, RETRO_MEMORY_RTC), &n);
    if (m && n && nr) memcpy(m, b.data() + 12 + ns, std::min<size_t>(n, nr));
}

/* Writes only the consoles this PC owns. */
void Frontend::link_save(bool force) {
    if (!link_loaded) return;
    link_saved_.resize(link_n);
    for (int i = 0; i < link_n; i++) {
        if (!link_own(i)) continue;
        std::vector<uint8_t> cur;
        size_t ns = 0, nr = 0;
        void* ms = core.memory(link_mem(i, RETRO_MEMORY_SAVE_RAM), &ns);
        void* mr = core.memory(link_mem(i, RETRO_MEMORY_RTC), &nr);
        if (ms) cur.insert(cur.end(), (uint8_t*)ms, (uint8_t*)ms + ns);
        if (mr) cur.insert(cur.end(), (uint8_t*)mr, (uint8_t*)mr + nr);
        if (!force && cur == link_saved_[i]) continue;
        if (ms && ns) write_atomic(link_srm(i), ms, ns);
        if (mr && nr) write_atomic(link_rtc(i), mr, nr);
        link_saved_[i] = cur;
        rlog("save: console %d wrote %s (%zu + %zu bytes)", i + 1, link_srm(i).c_str(), ns, nr);
    }
}

/* ------------------------------------------------------------------ netplay */

/* PlayStation netplay: the BIOS, the core build and the hardware options (multitap, pinned settings) decide what every instruction does:
 * two PCs that differ would drift apart a few seconds in and show up as a desync. Compare a fingerprint of them up front instead. */
uint64_t Frontend::ps1_config_fingerprint() {
    uint64_t h = 0xCBF29CE484222325ull;
    auto mix = [&](const std::string& t) { for (unsigned char c : t) h = (h ^ c) * 0x100000001B3ull; h = (h ^ 0xFF) * 0x100000001B3ull; };
    mix(core.info().library_name ? core.info().library_name : "");
    mix(core.info().library_version ? core.info().library_version : "");
    mix("discs=" + std::to_string(core.disc_count()));   // a disc swap is an index: every PC needs the same number of discs
    for (auto& kv : core.overrides) if (kv.first.rfind("beetle_psx_", 0) == 0) mix(kv.first + "=" + kv.second);   // ordered map: same on every PC
    std::vector<std::pair<std::string, uint64_t>> bios;
    std::error_code ec;
    for (auto& de : fs::directory_iterator(fs::u8path(a.system_dir), ec)) {
        if (ec || !de.is_regular_file(ec)) continue;
        std::string n = lower(de.path().filename().u8string());
        if (n.rfind("scph", 0) != 0 || n.size() < 4 || n.substr(n.size() - 4) != ".bin") continue;
        std::vector<uint8_t> d;
        if (read_file(de.path().u8string(), &d)) bios.push_back({n, n64run::hash_block(d.data(), d.size())});
    }
    std::sort(bios.begin(), bios.end());
    if (bios.empty()) mix("openbios");
    for (auto& b : bios) { mix(b.first); mix(std::to_string((unsigned long long)b.second)); }
    return h;
}

/* ---- PlayStation 2 online (LRPS2) ---------------------------------------------------------------------------------------------------- */

namespace {
uint64_t fnv_mix(uint64_t h, const void* p, size_t n) {
    const unsigned char* c = (const unsigned char*)p;
    for (size_t i = 0; i < n; i++) h = (h ^ c[i]) * 0x100000001B3ull;
    return (h ^ 0xFF) * 0x100000001B3ull;
}
uint32_t fold32(uint64_t h) { return (uint32_t)(h ^ (h >> 32)); }

/* Disc identity without reading 4 GB: size + 64 KiB blocks from 64 evenly spaced places (the first and the last included). */
uint64_t disc_fingerprint(const std::string& path) {
    FILE* f = _wfopen(fs::u8path(path).wstring().c_str(), L"rb");
    if (!f) return 0;
    _fseeki64(f, 0, SEEK_END);
    long long size = _ftelli64(f);
    uint64_t h = fnv_mix(0xCBF29CE484222325ull, &size, sizeof size);
    std::vector<uint8_t> buf(65536);
    const int n = 64;
    for (int i = 0; i < n; i++) {
        long long off = size > (long long)buf.size() ? (size - (long long)buf.size()) / (n - 1) * i : 0;
        _fseeki64(f, off, SEEK_SET);
        size_t got = fread(buf.data(), 1, buf.size(), f);
        h = fnv_mix(h, buf.data(), got);
        if (size <= (long long)buf.size()) break;
    }
    fclose(f);
    return h;
}

std::string ps2_card_file(const std::string& system_dir) { return system_dir + "\\pcsx2\\memcards\\Mcd001.ps2"; }
}  // namespace

/* The four things that make two LRPS2 emulations differ: the core build, the BIOS file, the disc and the core options. */
bool Frontend::ps2_config_check() {
    uint32_t part[4] = {0, 0, 0, 0};   // core, bios, disc, options
    uint64_t h = fnv_mix(0xCBF29CE484222325ull, core.info().library_name ? core.info().library_name : "", core.info().library_name ? strlen(core.info().library_name) : 0);
    h = fnv_mix(h, core.info().library_version ? core.info().library_version : "", core.info().library_version ? strlen(core.info().library_version) : 0);
    part[0] = fold32(h);
    std::string bios_name;
    auto it = core.overrides.find("pcsx2_bios");
    if (it != core.overrides.end()) bios_name = it->second;
    std::string bios_hash_txt = "none";
    {
        std::vector<uint8_t> d;
        if (!bios_name.empty() && read_file(a.system_dir + "\\pcsx2\\bios\\" + bios_name, &d)) {
            uint64_t bh = n64run::hash_block(d.data(), d.size());
            part[1] = fold32(bh);
            char t[64];
            snprintf(t, sizeof t, "%s %08x", bios_name.c_str(), part[1]);
            bios_hash_txt = t;
        }
    }
    uint64_t dh = disc_fingerprint(a.rom);
    dh = fnv_mix(dh, a.setup_tag.data(), a.setup_tag.size());
    part[2] = fold32(dh);
    uint64_t oh = 0xCBF29CE484222325ull;
    for (auto& kv : core.overrides)
        if (kv.first.rfind("pcsx2_", 0) == 0 && kv.first != "pcsx2_bios") { std::string t = kv.first + "=" + kv.second; oh = fnv_mix(oh, t.data(), t.size()); }
    part[3] = fold32(oh);
    uint64_t total = 0xCBF29CE484222325ull;
    for (int i = 0; i < 4; i++) total = fnv_mix(total, &part[i], sizeof part[i]);
    rlog("net: PS2 setup fingerprint %016llx (core %08x, BIOS %s, disc %08x [%s], options %08x)", (unsigned long long)total, part[0], bios_hash_txt.c_str(), part[2],
         a.setup_tag.c_str(), part[3]);
    static const char* const what[4] = {"emulator core build", "BIOS file", "game disc", "emulator options"};
    static const int code[4] = {11, 9, 10, 11};   // exit codes: 9 BIOS, 10 disc, 11 core build / options (logic.cpp exit_code_text)
    uint32_t seen = 1u << a.slot, t0 = hubnet::now_ms(), last = 0, done_at = 0, bad_at = 0;
    for (;;) {
        session.update();
        int from;
        std::vector<uint8_t> m;
        while (session.recv(&from, &m)) {
            if (m.size() == 26 && m[0] == kPktCfg2 && m[1] < 8 && (int)m[1] == from) {
                uint64_t theirs;
                uint32_t tp[4];
                std::memcpy(&theirs, &m[2], 8);
                std::memcpy(tp, &m[10], 16);
                if (theirs != total) {
                    if (!bad_at) {
                        int w = tp[1] != part[1] ? 1 : tp[2] != part[2] ? 2 : tp[0] != part[0] ? 0 : 3;
                        rlog("net: FAIL: slot %d runs a different %s (theirs %08x, mine %08x). Everyone in the room needs the same BIOS file, the same disc and the same emulator.",
                             from, what[w], tp[w], part[w]);
                        net_rc = code[w];
                        bad_at = hubnet::now_ms() ? hubnet::now_ms() : 1;   // keep announcing ours for a moment so the other PCs report the mismatch too
                    }
                } else seen |= 1u << from;
            }
        }
        uint32_t now = hubnet::now_ms();
        if (session.failed()) return false;
        if (now - last >= 100 || !last) {
            uint8_t pkt[26] = {kPktCfg2, (uint8_t)a.slot};
            std::memcpy(pkt + 2, &total, 8);
            std::memcpy(pkt + 10, part, 16);
            session.send(-1, pkt, 26);
            last = now ? now : 1;
        }
        if (bad_at) { if (now - bad_at > 1500) return false; }
        else if ((seen & slot_mask) == slot_mask) {
            if (!done_at) done_at = now ? now : 1;
            if (now - done_at > 1500) return true;
        }
        if (now - t0 > 40000) { rlog("net: FAIL: not every PC reported its PS2 setup within 40 s"); net_rc = 4; return false; }
        pump_events(false);
        if (quit) return false;
        Sleep(5);
    }
}

/* Memory card sharing (docs/SAVES.md, "PlayStation 2"): the host's Mcd001.ps2 is deflated and sent to every PC through a short-lived session on a
 * sibling room, BEFORE the core boots (the core opens the card file while it loads the game). Guests overwrite their own throwaway copy;
 * nobody's vault is written. A host without a card shares "no card": everyone starts with a blank one. */
bool Frontend::ps2_card_presync() {
    std::string err;
    uint32_t mask = n64run::derive_slot_mask(owner, a.slot, std::max(1, a.players), &err);
    if (!mask) { rlog("net: bad --slot/--players/--ports: %s", err.c_str()); net_rc = 2; return false; }
    std::string room = a.room;   // "ABCDEF-n" -> "ABCDEF-(n + 100000)": a sibling game room for the card exchange
    size_t dash = room.find('-');
    if (dash == std::string::npos) room += "-100000";
    else room = room.substr(0, dash + 1) + std::to_string(100000 + atoi(room.c_str() + dash + 1) % 800000);
    hubnet::Session ps;
    hubnet::SessionConfig cfg;
    cfg.server = a.net;
    cfg.room = room;
    cfg.slot = a.slot;
    cfg.expected = std::max(1, a.players);
    if (!ps.start(cfg)) { rlog("net: cannot start the card session"); net_rc = 4; return false; }
    uint32_t t0 = hubnet::now_ms();
    while (!ps.ready()) {
        ps.update();
        if (ps.failed() || quit || hubnet::now_ms() - t0 > 60000) { rlog("net: card session failed: %s", ps.status().c_str()); net_rc = 4; ps.stop(); return false; }
        Sleep(5);
    }
    const std::string card = ps2_card_file(a.system_dir);
    std::error_code ec;
    fs::create_directories(fs::u8path(a.system_dir + "\\pcsx2\\memcards"), ec);
    std::vector<n64run::SaveFile> files;
    if (a.slot == 0) {
        std::vector<uint8_t> raw;
        if (read_file(card, &raw) && !raw.empty()) {
            mz_ulong zn = mz_compressBound((mz_ulong)raw.size());
            std::vector<uint8_t> z(zn + 8);
            uint64_t rn = raw.size();
            std::memcpy(z.data(), &rn, 8);
            if (mz_compress2(z.data() + 8, &zn, raw.data(), (mz_ulong)raw.size(), 9) != MZ_OK) { rlog("hostsave: cannot compress the memory card"); net_rc = 6; ps.stop(); return false; }
            z.resize(zn + 8);
            rlog("hostsave: host shares its memory card (%zu bytes, %zu compressed, hash %016llx)", raw.size(), z.size(), (unsigned long long)n64run::save_hash(raw.data(), raw.size()));
            files.push_back({"card.srm", std::move(z)});
        } else rlog("hostsave: host has no memory card; everyone starts with a blank one");
    }
    std::string dir = a.save_dir + "\\hostsave";
    if (a.slot != 0) { fs::remove_all(fs::u8path(dir), ec); fs::create_directories(fs::u8path(dir), ec); }
    std::string serr;
    if (!n64run::run_save_sync(ps, a.slot, mask, a.slot == 0 ? &files : nullptr, dir, 60000, &serr)) {
        rlog("hostsave: memory card sync failed: %s", serr.c_str());
        net_rc = 6;
        ps.stop();
        return false;
    }
    if (a.slot != 0) {
        std::vector<uint8_t> z;
        fs::remove(fs::u8path(card), ec);
        if (read_file(dir + "\\card.srm", &z) && z.size() > 8) {
            uint64_t rn;
            std::memcpy(&rn, z.data(), 8);
            if (rn == 0 || rn > (64ull << 20)) { rlog("hostsave: implausible card size"); net_rc = 6; ps.stop(); return false; }
            std::vector<uint8_t> raw((size_t)rn);
            mz_ulong out = (mz_ulong)rn;
            if (mz_uncompress(raw.data(), &out, z.data() + 8, (mz_ulong)(z.size() - 8)) != MZ_OK || out != rn) { rlog("hostsave: cannot unpack the host's memory card"); net_rc = 6; ps.stop(); return false; }
            if (!write_atomic(card, raw.data(), raw.size())) { rlog("hostsave: cannot write %s", card.c_str()); net_rc = 6; ps.stop(); return false; }
            rlog("hostsave: booted from the host's memory card (%zu bytes, hash %016llx)", raw.size(), (unsigned long long)n64run::save_hash(raw.data(), raw.size()));
        } else rlog("hostsave: the host has no memory card; starting with a blank one");
        guest_save_ro = true;
    }
    fs::remove(fs::u8path(ps2_card_file(a.system_dir).substr(0, ps2_card_file(a.system_dir).size() - 7) + "2.ps2"), ec);   // slot 2 always starts blank
    ps.say_bye();
    uint32_t t1 = hubnet::now_ms();
    while (hubnet::now_ms() - t1 < 300) { ps.update(); Sleep(5); }
    ps.stop();
    return true;
}

bool Frontend::ps1_config_check() {
    const uint64_t mine = ps1_config_fingerprint();
    rlog("net: PlayStation setup fingerprint %016llx (BIOS %s)", (unsigned long long)mine, ([&] {
        std::error_code ec;
        for (auto& de : fs::directory_iterator(fs::u8path(a.system_dir), ec)) { std::string n = lower(de.path().filename().u8string()); if (n.rfind("scph", 0) == 0) return "real"; }
        return "OpenBIOS";
    })());
    uint32_t seen = 1u << a.slot, t0 = hubnet::now_ms(), last = 0, done_at = 0, bad_at = 0;
    for (;;) {
        session.update();
        int from;
        std::vector<uint8_t> m;
        while (session.recv(&from, &m)) {
            if (m.size() == 10 && m[0] == kPktCfg && m[1] < 8 && (int)m[1] == from) {
                uint64_t theirs;
                std::memcpy(&theirs, &m[2], 8);
                if (theirs != mine) {
                    if (!bad_at) {
                        rlog("net: FAIL: slot %d runs a different PlayStation setup (BIOS, core build or console options differ; theirs %016llx, mine %016llx). "
                             "Everyone in the room needs the same BIOS file (or all on the built-in OpenBIOS).", from, (unsigned long long)theirs, (unsigned long long)mine);
                        bad_at = hubnet::now_ms() ? hubnet::now_ms() : 1;   // keep announcing ours for a moment so the other PCs report the mismatch too
                    }
                } else seen |= 1u << from;
            }
        }
        uint32_t now = hubnet::now_ms();
        if (session.failed()) return false;
        if (now - last >= 100 || !last) {
            uint8_t pkt[10] = {kPktCfg, (uint8_t)a.slot};
            std::memcpy(pkt + 2, &mine, 8);
            session.send(-1, pkt, 10);
            last = now ? now : 1;
        }
        if (bad_at) { if (now - bad_at > 1500) return false; }
        else if ((seen & slot_mask) == slot_mask) {
            if (!done_at) done_at = now ? now : 1;   // keep announcing a moment so slower peers get ours
            if (now - done_at > 1500) return true;
        }
        if (now - t0 > 40000) { rlog("net: FAIL: not every PC reported its PlayStation setup within 40 s"); return false; }
        pump_events(false);
        if (quit) return false;
        Sleep(5);
    }
}

bool Frontend::setup_net() {
    if (a.net.empty()) return true;
    if (a.room.empty()) { rlog("net: --room required with --net"); return false; }
    online = true;
    if (!a.log.empty()) net_set_log_file((a.log + ".net").c_str());
    hubnet::SessionConfig cfg;
    cfg.server = a.net;
    cfg.room = a.room;
    cfg.slot = a.slot;
    cfg.expected = std::max(1, a.players);
    if (a.spectate) {
        if (a.ds_link || link || a.share_host_save) { rlog("net: --spectate does not work with link/DS/--share-host-save games"); return false; }
        slot_mask = 1u;   // the players: the host plus every owner named in --ports
        for (int i = 0; i < 4; i++) if (owner[i] >= 0 && owner[i] < 4) slot_mask |= 1u << owner[i];
        a.slot = 1;       // placeholder; the server assigns the real slot
        cfg.slot = 1;
        cfg.expected = 2;
        cfg.spectate = true;
    } else {
        std::string err;
        slot_mask = n64run::derive_slot_mask(owner, a.slot, std::max(1, a.players), &err);
        if (!slot_mask) { rlog("net: bad --slot/--players/--ports: %s", err.c_str()); return false; }
    }
    if (!session.start(cfg)) { rlog("net: cannot start session"); return false; }
    rlog("net: joining %s as slot %d of %d PCs (slot mask %x, delay %s)", a.room.c_str(), a.slot, a.players, slot_mask,
         a.delay < 0 ? "auto" : std::to_string(a.delay).c_str());
    uint32_t t0 = hubnet::now_ms(), last = 0;
    while (!session.ready()) {
        session.update();
        pump_events(false);
        if (quit) return false;
        if (session.failed()) { rlog("net: session failed: %s", session.status().c_str()); return false; }
        uint32_t now = hubnet::now_ms();
        if (now - t0 > 60000) { rlog("net: timed out waiting for peers (%s)", session.status().c_str()); return false; }
        if (now - last > 2000) { last = now; rlog("net: %s", session.status().c_str()); }
        Sleep(5);
    }
    rlog("net: session ready");
    if (a.spectate) {
        a.slot = session.my_slot();
        rlog("net: spectating as slot %d (no port, confirmed inputs only)", a.slot);
        if (a.rollback) {
            rb_warmup();
            rb_spectator_ = true;
            return true;
        }
    }
    if (!a.spectate) {   // every PC must hold the same cheat list (an empty one included) or the emulations diverge
        uint64_t ch = cheats_on ? cheats::set_hash(cheat_boot, cheat_target) : cheats::kEmptyHash;
        n64run::CheatSync cs = n64run::exchange_cheat_hash(session, a.slot, slot_mask, ch, 8000, [this] { pump_events(false); }, [](const std::string& m) { rlog("%s", m.c_str()); });
        if (cs == n64run::CheatSync::Mismatch) { rlog("net: cheat lists differ between players; leaving"); net_rc = 5; return false; }
        if (cs == n64run::CheatSync::Failed) { rlog("net: connection lost during the cheat check"); net_rc = lost_code(); return false; }
    }
    if (!a.spectate && a.share_host_save && !link && !is_ds && !is_ps2 && !share_host_save_run()) { net_rc = 6; return false; }   // PS2: the card was exchanged before the core booted (ps2_card_presync)
    if (link) {
        std::vector<std::vector<uint8_t>> blobs;
        std::string lerr;
        int mine = link_port_of_slot(a.slot);
        if (mine < 0) { rlog("link: this slot owns no console"); return false; }
        if (!rlink::run_link_exchange(session, a.slot, slot_mask, link_blob_from_disk(mine), 60000, &blobs, [this] { pump_events(false); }, &lerr)) {
            rlog("link: save exchange failed: %s", lerr.c_str());
            net_rc = lost_code();
            return false;
        }
        for (int i = 0; i < link_n; i++)
            if (owner[i] >= 0 && owner[i] < 8 && (slot_mask & (1u << owner[i]))) link_apply_blob(i, blobs[owner[i]]);
        link_loaded = true;
        rlog("link: saves exchanged and loaded for %d consoles", link_n);
    }
    if (!a.spectate && is_ps1 && !ps1_config_check()) { net_rc = 7; return false; }
    if (!a.spectate && is_ps2 && !ps2_config_check()) { if (net_rc == 3 || net_rc == 4) net_rc = lost_code(); return false; }
    if (a.rollback) {
        const bool auto_delay = a.delay < 0;
        a.delay = a.delay >= 0 ? std::min(a.delay, 4) : 1;
        rb::Config rc;
        rc.adaptive_delay = auto_delay && a.rb_adaptive;
        rc.frame_ms = 1000.0 / (fps > 1 ? fps : 60.0);
        for (int i = 0; i < 4; i++) rc.owner[i] = owner[i];
        rc.my_slot = a.slot;
        rc.slot_mask = slot_mask;
        rc.delay = a.delay;
        rc.max_pred = a.rb_pred;
        if (const char* hi = std::getenv("RB_HASH_INTERVAL")) rc.hash_interval = std::max(1, atoi(hi));
        rc.use_state_hash = false;   // confirmed video hash: serialized bytes differ benignly after a load (audio state)
        if (!env_is("RETRORUN_NO_RESYNC") && !is_ps1 && !is_ps2) rc.max_resyncs = 3;   // desync recovery: the host's state replaces everybody's (rollback.h); a 4th desync ends the game
        rc.val_bytes = is_ps1 ? 8 : 2;   // PlayStation: buttons + 4 stick bytes + 16 event bits (disc swap) per port
        if (is_ps1) rc.event_mask = retrorun::kEventMask;
        const bool big_room = a.players >= 3;   // any 3+ PC rollback room   // 3-4 PCs through the relay hit its 400 frames/s room cap
        if (is_ps1 || big_room) rc.relay_half_rate = true;
        if (is_ps1 || big_room) rc.send_gap_ms = 30;   // ~60 packets/s per PC: 4 PCs through the relay stay under its 400 frames/s room cap
        if (is_ps1) rc.pred_budget = 3;   // a PlayStation frame costs 3-8 ms: re-simulating 6 frames per late input would drop frames, so the input delay takes over sooner
        rb_warmup();
        rb::Transport t;
        t.pump = [this] { session.update(); };
        t.send = [this](int to, const void* d, int n) { session.send(to, d, n); };
        t.recv = [this](int* from, std::vector<uint8_t>* o) {
            while (session.recv(from, o)) {
                if (!o->empty() && *from > 0 && (session.spectator_mask() >> *from & 1u)) {   // nothing from a spectator reaches the engine
                    const uint8_t t0 = (*o)[0];
                    if (t0 == spectate::kPktAck) { if (spec_host_) spec_host_->on_ack(*from, o->data(), o->size(), hubnet::now_ms()); }
                    else if (t0 == spectate::kPktJoin) spec_on_join(*from);
                    else if (t0 == rb::xfer::kPkt) spec_on_xfer(*from, o->data(), o->size());
                    continue;
                }
                if (!o->empty() && (*o)[0] == spectate::kPktAck) continue;
                return true;
            }
            return false;
        };
        t.now_ms = [] { return hubnet::now_ms(); };
        t.log = [](const std::string& s) { rlog("%s", s.c_str()); };
        t.notice = [this](const std::string& s) { set_status(s); };
        t.relayed = [this] { for (int sl = 0; sl < 8; sl++) if (sl != a.slot && (slot_mask >> sl & 1u) && session.is_relayed(sl)) return true; return false; };
        rbe = std::make_unique<rb::Engine>(rc, make_rb_sim(), t);
        if (a.slot == 0 && !a.no_spectators) {   // keep the confirmed-input history so spectators can join at any time
            uint8_t pm = 0;
            for (int i = 0; i < 4; i++) if (owner[i] >= 0) pm |= (uint8_t)(1u << i);
            spec_host_.reset(new spectate::Host(pm, 8));
            spec_host_->set_log([](const std::string& m) { rlog("%s", m.c_str()); });
            spec_host_->set_hold_ms(spectate::kHoldMs);   // a new spectator first says whether it wants a live snapshot (kPktJoin)
            spec_live_ok_ = !is_ps1 && !is_ps2 && !env_is("RETRORUN_NO_LIVE_SPECTATE");   // the cores whose states pass desync recovery
            rbe->set_confirmed_sink([this](uint32_t, const uint64_t pads[4]) { spec_host_->push(pads); });
            rbe->set_hash_sink([this](uint32_t fr, uint64_t h) { spec_host_->add_hash(fr, h); });
        }
        rlog("net: ROLLBACK netplay, input delay %d, max prediction %d, state %zu bytes, sync hash %s", a.delay, rc.max_pred,
             core.serialize_size(), "output");
        if (!rbe->start_barrier(30000)) { rlog("net: start barrier failed"); net_rc = lost_code(); return false; }
        rlog("net: all %d players ready, starting", a.players);
        return true;
    }
    if (!a.spectate) {
        n64run::DelayNegotiation dn;
        dn.fixed_delay = a.delay;
        dn.my_slot = a.slot;
        dn.slot_mask = slot_mask;
        dn.frame_ms = 1000.0 / (fps > 1 ? fps : 60.0);
        int d = 2;
        if (!n64run::negotiate_delay(session, dn, [this] { pump_events(false); }, [](const std::string& m) { rlog("%s", m.c_str()); }, &d)) {
            rlog("net: input delay negotiation failed (%s)", session.status().c_str());
            net_rc = lost_code();
            return false;
        }
        a.delay = d;
    }
    rlog("net: input delay %d", a.delay);

    n64run::LockstepOptions o;
    for (int i = 0; i < 4; i++) o.owner[i] = owner[i];
    o.my_slot = a.slot;
    o.num_slots = a.players;
    o.slot_mask = slot_mask;
    o.hash_label = is_ps2 ? "ee-ram" : use_state_hash ? "state" : "output";
    o.wide = is_ps1 || is_ps2;   // sticks travel as a second word per port
    o.delay = a.delay;
    o.online = true;
    o.spectator = a.spectate;
    o.serve_spectators = !a.no_spectators;
    o.hash_interval = 600;
    n64run::LockstepHooks h;
    h.read_local = [this](int i) { return local_pad_cache_[i]; };
    h.read_local_aux = [this](int i) { return retrorun::pack_sticks(local_stick_[i]); };
    h.update_local = [this]() { pump_events(true); poll_all_local(); };
    h.idle = [this]() { pump_events(true); };
    h.hash_rdram = [this](uint64_t* out) {
        uint64_t v = sync_hash();
        if (!v) return false;
        *out = v;
        return true;
    };
    h.request_stop = [this]() { quit = true; };
    h.on_drop = [this](int slot, uint32_t) { set_status("Player in slot " + std::to_string(slot) + " disconnected - playing on"); };
    h.log = [](const std::string& s) { rlog("%s", s.c_str()); };
    h.on_spectate_msg = [this](int from, const std::vector<uint8_t>& m) {
        if (a.spectate) {   // late repeats of the live state after we loaded it: our Done was lost
            if (m.size() >= 4 && m[0] == rb::xfer::kPkt && (m[1] == rb::xfer::Begin || m[1] == rb::xfer::Chunk) && m[3] == ls_done_ep_ && hubnet::now_ms() - ls_done_at_ > 100) {
                auto v = rb::xfer::header(rb::xfer::Done, a.slot, (uint8_t)ls_done_ep_);
                v.push_back(1);
                session.send(0, v.data(), (int)v.size());
                ls_done_at_ = hubnet::now_ms();
            }
            return;
        }
        if (m[0] == spectate::kPktJoin) spec_on_join(from);
        else if (m[0] == rb::xfer::kPkt) spec_on_xfer(from, m.data(), m.size());
    };
    if (a.spectate) h.set_limiter = [this](bool on) { ff = !on; next_deadline = 0; };   // far behind: fast-forward (no pacing, no audio, no video)
    lockstep = std::make_unique<n64run::Lockstep>(o, &session, h);
    if (lockstep->spectate_host()) {
        lockstep->spectate_host()->set_hold_ms(spectate::kHoldMs);
        spec_live_ok_ = !is_ps1 && !is_ps2 && !env_is("RETRORUN_NO_LIVE_SPECTATE");
    }
    if (!lockstep->start_barrier(30000)) { rlog("net: start barrier failed"); net_rc = lost_code(); return false; }
    if (a.spectate && !ls_spectator_live_join()) { net_rc = lost_code(); return false; }
    rlog("net: all %d players ready, starting", a.players);
    return true;
}

/* Lockstep spectator, before the first frame: ask the host for a live snapshot (spectate.h). The inputs from its frame are buffered in the
 * lockstep's spectate client while the state downloads; an old host / young match / refused core streams from sample 0 instead. */
bool Frontend::ls_spectator_live_join() {
    spectate::JoinClient jc(!env_is("RETRORUN_NO_LIVE_SPECTATE"));
    ls_t_join_ = hubnet::now_ms();
    if (jc.playing()) return true;
    spectate::Client& cli = lockstep->spectate_client_mut();
    rb::xfer::Receiver rcv;
    uint32_t last_rx = hubnet::now_ms(), dl_progress = 0;
    size_t dl_got = 0;
    for (;;) {
        if (quit) return false;
        pump_events(false);
        session.update();
        int from; std::vector<uint8_t> m;
        const uint32_t now0 = hubnet::now_ms();
        while (session.recv(&from, &m)) {
            if (from != 0 || m.empty()) continue;
            last_rx = now0;
            if (m[0] == spectate::kPktData) {
                if (jc.mode() == spectate::JoinClient::Mode::Knock) {
                    if (m.size() < 6 || rb::xfer::g32(m.data() + 2) != 0) continue;   // inputs from F that overtook our Begin: resent later
                    jc.on_legacy_data();
                }
                cli.on_data(m.data(), m.size(), now0);
            } else if (m[0] == spectate::kPktNoLive && m.size() >= 3) {
                if (jc.mode() == spectate::JoinClient::Mode::Knock) {
                    jc.on_nolive(m[2]);
                    rlog("spectate: the host has no live snapshot for us (reason %u): watching from the start, fast-forwarding to the live game", (unsigned)m[2]);
                    if (m[2] != spectate::kRefuseEarly) set_status("Can't join live: replaying from the start");
                }
            } else if (m[0] == rb::xfer::kPkt && m.size() >= 4 && m[2] == 0) {
                if (m[1] == rb::xfer::Begin) {
                    rb::xfer::Meta mt;
                    if (rb::xfer::dec_begin(m.data(), m.size(), &mt) && jc.mode() == spectate::JoinClient::Mode::Knock && rcv.on_begin(m[3], mt)) {
                        jc.on_begin(mt.frame, now0);
                        cli.start_at(mt.frame);
                        dl_progress = now0;
                        rlog("spectate: live join: downloading the state of frame %u (%u bytes, %u chunks) %u ms after joining", mt.frame, mt.comp, (unsigned)mt.chunks, now0 - ls_t_join_);
                    }
                } else if (m[1] == rb::xfer::Chunk && m.size() >= 6 && jc.mode() == spectate::JoinClient::Mode::Download) {
                    rcv.on_chunk(m[3], rb::xfer::g16(m.data() + 4), m.data() + 6, m.size() - 6);
                }
            }
        }
        const uint32_t now = hubnet::now_ms();
        if (jc.playing()) return true;   // refused / old host: lockstep plays from sample 0 (and fast-forwards)
        if (jc.want_knock(now)) { uint8_t k[3] = {spectate::kPktJoin, (uint8_t)a.slot, 1}; session.send(0, k, 3); }
        if (jc.mode() == spectate::JoinClient::Mode::Download) {
            if (rcv.received() != dl_got) { dl_got = rcv.received(); dl_progress = now; }
            uint16_t nx; uint64_t bits;
            if (rcv.ack_due(now, &nx, &bits)) { auto v = rb::xfer::enc_ack(a.slot, rcv.epoch(), nx, bits); session.send(0, v.data(), (int)v.size()); }
            cli.service(now, a.slot, [this](int slot, const uint8_t* d, int n) { session.send(slot, d, n); });
            if (rcv.complete()) {
                const rb::xfer::Meta& mt = rcv.meta();
                std::vector<uint8_t> raw(mt.raw);
                bool ok;
                if (mt.flags & 1) { mz_ulong on = (mz_ulong)raw.size(); ok = mz_uncompress(raw.data(), &on, rcv.payload().data(), (mz_ulong)rcv.payload().size()) == MZ_OK && on == raw.size(); }
                else { raw = rcv.payload(); ok = raw.size() == mt.raw; }
                if (ok && rb::hash_bytes(raw.data(), raw.size()) != mt.hash) ok = false;
                if (ok) ok = core.unserialize(raw.data(), raw.size());
                if (!ok) { rlog("spectate: the live state did not load"); return false; }
                lockstep->spectate_start_at(mt.frame);
                frame_no = mt.frame;
                ls_f_start_ = mt.frame;
                ls_live_snapshot_ = true;
                jc.on_loaded();
                ls_done_ep_ = rcv.epoch();
                auto v = rb::xfer::header(rb::xfer::Done, a.slot, rcv.epoch());
                v.push_back(1);
                session.send(0, v.data(), (int)v.size());
                ls_done_at_ = now;
                rlog("spectate: live join: state of frame %u loaded %u ms after joining; %u behind the host", mt.frame, now - ls_t_join_, cli.lag());
                return true;
            }
            if (jc.download_stuck(now, dl_progress)) { rlog("spectate: the live state stopped arriving"); return false; }
        }
        if (session.failed() || now - last_rx > 15000) { rlog("net: connection lost while joining as a spectator"); return false; }
        Sleep(1);
    }
}

/* Lockstep spectator, after each frame's input poll: note when it first got within the live lag of the host. */
void Frontend::ls_spectator_frame() {
    const spectate::Client& c = lockstep->spectate_client();
    ls_cu_.update(c.lag(), hubnet::now_ms());
    if (ls_cu_.live_once() && !ls_live_reported_) {
        ls_live_reported_ = true;
        rlog("spectate: LIVE at frame %llu, %u frames behind the host, %u ms after joining (%s, %llu frames fast-forwarded)", (unsigned long long)frame_no, c.lag(),
             ls_cu_.live_at() - ls_t_join_, ls_live_snapshot_ ? "live snapshot" : "replay from frame 0", (unsigned long long)(frame_no - ls_f_start_));
    }
}

/* ------------------------------------------------------------ DS local wireless */

static void RETRO_CALLCONV np_send(int flags, const void* buf, size_t len, uint16_t cid) { if (g_fe) g_fe->link_send_pub(flags, buf, len, cid); }
static void RETRO_CALLCONV np_poll() { if (g_fe) g_fe->link_pump_pub(); }

void Frontend::link_send(int flags, const void* buf, size_t len, uint16_t cid) {
    if (!link_active) return;
    (void)flags;
    if (buf && len) {
        auto frags = dslink::split(link_msg_id++, buf, len);
        if (frags.empty()) { rlog("link: dropped an unsendable packet of %zu bytes", len); return; }
        int ty = dslink::frame_type((const uint8_t*)buf, len);
        link_tx++;
        if (ty == 2) link_tx_cmd++; else if (ty == 1) link_tx_reply++;
        for (auto& f : frags) session.send(cid == RETRO_NETPACKET_BROADCAST ? -1 : (int)cid, f.data(), (int)f.size());
    }
    session.update();   // flush hint / opportunistic pump
}

void Frontend::link_pump() {
    if (!link_active) return;
    session.update();
    int from = -1;
    std::vector<uint8_t> d, msg;
    while (session.recv(&from, &d)) {
        if (!link_reasm.add(from, d.data(), d.size(), &msg)) continue;
        if (msg.size() == 2310 && memcmp(msg.data() + 10, "RHPROBE", 7) == 0) {   // never handed to the core
            bool good = true;
            for (size_t i = 17; i < msg.size(); i++) if (msg[i] != (uint8_t)(i * 31)) { good = false; break; }
            if (good) probe_rx++; else probe_bad++;
            continue;
        }
        link_rx++;
        link_rx_by_slot[from]++;
        int ty = dslink::frame_type(msg.data(), msg.size());
        if (ty == 2) link_rx_cmd++; else if (ty == 1) link_rx_reply++;
        core.netpacket().receive(msg.data(), msg.size(), (uint16_t)from);
    }
    if (session.failed()) link_stop("connection lost");
}

void Frontend::link_stop(const char* why) {
    if (!link_active) return;
    link_active = false;
    rlog("link: stopped (%s)", why);
    if (core.netpacket().stop) core.netpacket().stop();
}

bool Frontend::setup_link() {
    if (!core.has_netpacket()) { rlog("link: this core has no local-wireless netpacket interface"); return false; }
    if (a.net.empty() || a.room.empty()) { rlog("link: --net and --room required with --ds-link"); return false; }
    if (!a.log.empty()) net_set_log_file((a.log + ".net").c_str());
    hubnet::SessionConfig cfg;
    cfg.server = a.net;
    cfg.room = a.room;
    cfg.slot = a.slot;
    cfg.expected = std::max(1, a.players);
    if (!session.start(cfg)) { rlog("link: cannot start session"); return false; }
    rlog("link: joining %s as slot %d of %d PCs", a.room.c_str(), a.slot, a.players);
    uint32_t t0 = hubnet::now_ms(), last = 0;
    while (!session.ready()) {
        session.update();
        pump_events(false);
        if (quit) return false;
        if (session.failed()) { rlog("link: session failed: %s", session.status().c_str()); return false; }
        uint32_t now = hubnet::now_ms();
        if (now - t0 > 60000) { rlog("link: timed out waiting for peers (%s)", session.status().c_str()); return false; }
        if (now - last > 2000) { last = now; rlog("link: %s", session.status().c_str()); }
        Sleep(5);
    }
    rlog("link: session ready (%s)", session.status().c_str());
    link_active = true;
    link_probe = env_is("RETRORUN_LINK_PROBE");
    core.netpacket().start((uint16_t)a.slot, np_send, np_poll);
    if (core.netpacket().connected && a.slot == 0) {
        /* Client ids are the lobby slots, which have gaps after somebody leaves: announce the real ones (--ports lists the members). */
        bool seen[8] = {false};
        int announced = 0;
        for (int i = 0; i < 4; i++) {
            int s = a.ports[i];
            if (s > 0 && s < 8 && !seen[s]) { seen[s] = true; ++announced; core.netpacket().connected((uint16_t)s); }
        }
        if (!announced)   // older hub without --ports: slots 1..players-1
            for (int s = 1; s < std::max(1, a.players); s++) core.netpacket().connected((uint16_t)s);
    }
    rlog("link: wireless started as client id %d", a.slot);
    return true;
}

/* ------------------------------------------------------------------ main loop */

void Frontend::do_pacing() {
    if (ff || (a.headless && !a.realtime && !a.ds_link && !rbe)) return;
    switch ((speed != 1.0 || a.headless) ? Pace::Timer : pace) {   // a changed speed paces by timer (vsync/audio pacing are tied to 1x)
        case Pace::Audio:
            while (audio.fill() > audio.target() && !quit) {
                SDL_Delay(1);
                pump_events(false);
            }
            break;
        case Pace::Timer: {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            int64_t period = (int64_t)((double)qpc_freq / (fps * speed));
            if (next_deadline == 0 || now.QuadPart - next_deadline > qpc_freq / 10) next_deadline = now.QuadPart;
            next_deadline += period;
            while (!quit) {
                QueryPerformanceCounter(&now);
                int64_t left = next_deadline - now.QuadPart;
                if (left <= 0) break;
                if (left > qpc_freq / 500) SDL_Delay(1);
            }
            break;
        }
        default: break;   // vsync: SDL_RenderPresent blocks
    }
}

std::unique_ptr<retrofeat::Features> Frontend::make_feat(bool measure) {
    retrofeat::Hooks h;
    h.run = [this] { core.run(); };
    h.real_frame = [this] {   // RetroAchievements sees only the real frame: never a run-ahead or rewound one
        if (ra_rewound_) { ra_rewound_ = false; ra_.reset(); }   // rewound memory is not progress: restart trigger tracking
        ra_.frame();
    };
    h.state_size = [this] { return core.serialize_size(); };
    h.save = [this](void* b, size_t n) { return core.serialize(b, n); };
    h.load = [this](const void* b, size_t n) { return core.unserialize(b, n); };
    h.video_hash = [this] { const FrameBuf& f = core.frame(); return f.px.empty() ? (uint64_t)0 : n64run::hash_block(f.px.data(), f.px.size() * 4); };
    h.speculative = [this](bool s) { spec_ = s; };
    h.notify = [](const std::string& t, retrofeat::Note n) {
        osd::push(t, n == retrofeat::Note::Warn ? osd::Kind::Warn : n == retrofeat::Note::Error ? osd::Kind::Error : n == retrofeat::Note::Good ? osd::Kind::Good : osd::Kind::Info);
    };
    h.corner = [](const std::string& t) { osd::set_corner(t, osd::Kind::Warn); };
    retrofeat::Settings st;
    st.system = lower(a.system);
    st.rewind = a.rewind_mode;
    st.runahead = a.runahead;
    st.fps = fps > 1 ? fps : 60.0;
    st.measure = measure;
    auto f = std::make_unique<retrofeat::Features>(h, st);
    rlog("features: rewind %s, run-ahead %d frame(s)", f->rewind_active() ? "on" : "off", f->runahead_frames());
    return f;
}

/* QA (headless, constant zero input): --rewind-selftest N,K and --runahead-selftest N. */
int Frontend::run_feat_selftest() {
    feat_ = make_feat(false);
    for (int i = 0; i < 30; i++) core.run();   // past the boot frames
    std::string rep;
    bool ok = true;
    if (a.selftest_rewind_frames) { ok = feat_->selftest_rewind(a.selftest_rewind_frames, a.selftest_rewind_back, &rep) && ok; rlog("%s", rep.c_str()); }
    if (a.selftest_runahead_frames) { ok = feat_->selftest_runahead(a.selftest_runahead_frames, &rep) && ok; rlog("%s", rep.c_str()); }
    rlog("FEATTEST %s", ok ? "PASS" : "FAIL");
    return ok ? 0 : 6;
}

void Frontend::finish() {
    static bool done = false;
    if (done) return;
    done = true;
    if (const char* pk = getenv("RETRORUN_PEEK"); pk && core.game_loaded()) {   // QA-only: "memid:offset:len[,..]" (libretro memory id, e.g. 2 = system RAM; link mode: raw ids) -> "peek:" log lines at exit
        std::string spec = pk;
        size_t p = 0;
        while (p < spec.size()) {
            size_t q = spec.find(',', p); if (q == std::string::npos) q = spec.size();
            std::string t = spec.substr(p, q - p); p = q + 1;
            unsigned id = 0, off = 0, len = 0;
            if (sscanf(t.c_str(), "%i:%i:%i", &id, &off, &len) != 3) continue;
            size_t n = 0;
            const uint8_t* m = (const uint8_t*)core.memory(id, &n);
            std::string hex;
            for (unsigned i = 0; m && off + i < n && i < len; i++) { char b[4]; snprintf(b, sizeof b, "%02x", m[off + i]); hex += b; }
            rlog("peek: %s size=%zu %s", t.c_str(), n, hex.c_str());
        }
    }
    if (const char* sd = getenv("RETRORUN_STATE_DUMP"); sd && core.game_loaded() && core.serialize_size()) {   // QA-only: whole serialized core state (all consoles in --link mode) at exit
        std::vector<uint8_t> buf(core.serialize_size());
        if (core.serialize(buf.data(), buf.size()) && write_atomic(sd, buf.data(), buf.size())) rlog("state-dump: %s (%zu bytes)", sd, buf.size());
    }
    if (core.game_loaded() && link) {
        link_save(false);
    } else if (core.game_loaded()) {
        save_srm(false);
        {   // final clock (the periodic save only writes it together with a changed save RAM)
            size_t nr = 0;
            void* rm = core.memory(RETRO_MEMORY_RTC, &nr);
            if (rm && nr && !guest_save_ro && !is_ds && !rtc_saved.empty() && (rtc_saved.size() != nr || memcmp(rtc_saved.data(), rm, nr) != 0))
                write_atomic(rtc_path(), rm, nr);
        }
        if (!a.selftest && !is_ds && core.serialize_size()) {   // DS states are ~19 MB and the hub never resumes them
            size_t n = core.serialize_size();
            std::vector<uint8_t> buf(n);
            if (core.serialize(buf.data(), n) && write_atomic(state_path("auto"), buf.data(), n))
                rlog("state: wrote auto.state (%zu bytes)", n);
        }
    }
}

rb::Sim Frontend::make_rb_sim() {
    rb::Sim s;
    s.state_size = [this] { return core.serialize_size(); };
    s.save = [this](uint8_t* b, size_t n) { return core.serialize(b, n); };
    s.load = [this](const uint8_t* b, size_t n) { return core.unserialize(b, n); };
    s.pack_state = [](const uint8_t* in, size_t n, std::vector<uint8_t>* out) {
        mz_ulong zn = mz_compressBound((mz_ulong)n);
        out->assign(zn, 0);
        if (mz_compress2(out->data(), &zn, in, (mz_ulong)n, 6) != MZ_OK) return false;
        out->resize(zn);
        return true;
    };
    s.unpack_state = [](const uint8_t* in, size_t n, uint8_t* out, size_t raw) {
        mz_ulong on = (mz_ulong)raw;
        return mz_uncompress(out, &on, in, (mz_ulong)n) == MZ_OK && on == raw;
    };
    s.step = [this](const uint64_t pads_in[4], bool visible) {
        uint64_t pads[4] = {pads_in[0], pads_in[1], pads_in[2], pads_in[3]};
        /* QA only: RETRORUN_TEST_DESYNC="<slot>:<frame>,..." makes this PC run 24 core steps (replays included, so a rollback cannot heal it) with wrong pads and one extra emulated frame each, from that frame on */
        static int poison_left = 0;
        if (visible) {
            static const std::vector<std::pair<int, uint64_t>> inj = [] {
                std::vector<std::pair<int, uint64_t>> v;
                if (const char* e = std::getenv("RETRORUN_TEST_DESYNC")) {
                    std::string str = e;
                    size_t i = 0;
                    while (i < str.size()) {
                        size_t j = str.find(',', i); if (j == std::string::npos) j = str.size();
                        size_t c = str.find(':', i);
                        if (c != std::string::npos && c < j) v.push_back({atoi(str.substr(i, c - i).c_str()), (uint64_t)atoll(str.substr(c + 1, j - c - 1).c_str())});
                        i = j + 1;
                    }
                }
                return v;
            }();
            static uint64_t seen_max = 0;
            if (!inj.empty()) {
                uint64_t cur = rbe ? rbe->cur() : 0;
                if (cur >= seen_max) {   // a frame replayed after a resync is not poisoned again
                    for (auto& e : inj) if (e.first == a.slot && e.second == cur) { poison_left = 24; rlog("TEST: injected desync at frame %llu", (unsigned long long)cur); }
                    seen_max = cur + 1;
                }
            }
        }
        const bool poisoned = poison_left > 0;
        if (poisoned) { poison_left--; for (int q = 0; q < 4; q++) pads[q] ^= 0x1F3u; }
        resim = !visible;
        fr_audio = 0xCBF29CE484222325ull;
        for (int p = 0; p < 4; p++) {
            if (is_ps1) retrorun::unpack_pad(pads[p], &frame_pad[p], frame_stick[p]);   // buttons + quantised sticks, same on every PC
            else frame_pad[p] = (uint32_t)pads[p];
            unsigned dt;
            if (is_ps1 && retrorun::disc_event_of(pads[p], &dt)) disc_apply(dt, visible);   // before the frame runs, on every PC at the same frame
        }
        core.run();
        if (poisoned) core.run();   // QA desync injection: one extra emulated frame, visible whatever the game does with the pads
        resim = false;
        if (const char* dd = std::getenv("RB_SAVEFRAMES")) {   // QA: dump every stepped frame as dd\NNNN.bmp
            static int n = 0;
            char nm[512];
            snprintf(nm, sizeof nm, "%s\\%04d.bmp", dd, n++);
            write_frame_image(nm, core.frame());
        }
    };
    s.frame_sum = [this] {
        const FrameBuf& f = core.frame();
        uint64_t h = f.px.empty() ? 0 : n64run::hash_block(f.px.data(), f.px.size() * 4);
        if (const char* dd = std::getenv("RB_SAVEBYHASH")) {   // QA: write each distinct frame as dd\<hash>.bmp (find what differs after a rollback)
            char nm[512];
            snprintf(nm, sizeof nm, "%s\\%016llx.bmp", dd, (unsigned long long)h);
            if (!fs::exists(fs::u8path(nm))) write_frame_image(nm, f);
        }
        if (is_ps2) return (uint64_t)0;   // the PS2 core shows no frame on some runs; EE RAM (mem_hash) is the check
        return h;   // video only: audio resampler state is not part of any core's savestate, and audio never feeds back into the game
    };
    s.mem_hash = [this]() -> uint64_t {
        uint64_t h = 0xCBF29CE484222325ull;
        for (unsigned k = 0; k < 10; k++)
            for (unsigned t : {0u, 2u, 3u}) {
                size_t sz = 0;
                void* p = core.memory(t | (k << 8), &sz);
                if (p && sz) h = (h ^ n64run::hash_block(p, sz)) * 0x100000001B3ull;
            }
        return h;
    };
    s.poll_local = [this] { poll_all_local(); disc_ev_now_ = disc_ev_pending_; disc_ev_pending_ = 0; };
    s.read_local = [this](int port) -> uint64_t {
        for (int i = 0; i < n_local; i++)
            if (local_ports[i] == port) return is_ps1 ? (retrorun::pack_pad(local_pad_cache_[i], local_stick_[i]) | (i == 0 ? disc_ev_now_ : 0)) : (uint64_t)local_pad_cache_[i];
        return 0;
    };
    s.idle = [this] { pump_events(true); };
    return s;
}

/* frame pacing seen by a player (logged: a spectator joining must not stall the match) */
void Frontend::note_frame_gap() {
    uint64_t pc = SDL_GetPerformanceCounter();
    if (gap_last_) {
        double g = (double)(pc - gap_last_) * 1000.0 / (double)SDL_GetPerformanceFrequency();
        gap_max_ms_ = std::max(gap_max_ms_, g);
        gap_win_ms_ = std::max(gap_win_ms_, g);
        if (g > 50.0) gap_over_++;
    }
    gap_last_ = pc;
    if (frame_no % 120 == 0) {
        if (session.spectator_mask() || gap_win_ms_ > 50.0) rlog("frame-gap: frame %llu, 2 s max %.1f ms, spectators 0x%x", (unsigned long long)frame_no, gap_win_ms_, session.spectator_mask());
        gap_win_ms_ = 0;
    }
}

/* Called at a frame boundary (rollback: after run_frame; lockstep: before the frame's input poll). */
void Frontend::spec_host_service() {
    if (!spec_h()) return;
    uint32_t sm = session.spectator_mask();
    if (!spec_join_q_.empty()) spec_process_joins();
    if (!spec_snap_.empty()) spec_snap_service(sm);
    for (auto it = spec_snap_done_.begin(); it != spec_snap_done_.end();) { if (!(sm >> it->first & 1u)) it = spec_snap_done_.erase(it); else ++it; }
    // also with sm == 0: service() forgets departed spectators, so a new one in the same slot starts held, not at a stale position
    if (spec_host_) spec_host_->service(hubnet::now_ms(), sm, [this](int slot, const uint8_t* d, int n) { session.send(slot, d, n); });
}

void Frontend::spec_nolive(int slot, uint8_t reason) {
    uint8_t m[3] = {spectate::kPktNoLive, 0, reason};
    session.send(slot, m, 3);
    if (spec_h()) spec_h()->release(slot);
    rlog("spectate: slot %d joins from frame 0 (no live snapshot, reason %u)", slot, (unsigned)reason);
}

void Frontend::spec_on_join(int slot) {
    if (std::find(spec_join_q_.begin(), spec_join_q_.end(), slot) == spec_join_q_.end()) spec_join_q_.push_back(slot);
}

void Frontend::spec_process_joins() {
    std::vector<int> q;
    q.swap(spec_join_q_);
    for (int slot : q) {
    spectate::Host* H = spec_h();
    if (!H || !(session.spectator_mask() >> slot & 1u)) continue;
    uint32_t now = hubnet::now_ms();
    auto dn = spec_snap_done_.find(slot);
    if (dn != spec_snap_done_.end() && now - dn->second < 5000) continue;   // knocks sent before our Begin reached it
    if (!H->held(slot) && dn == spec_snap_done_.end() && !spec_snap_.count(slot)) {   // already streaming to it from 0 (its hold timed out): keep going
        uint8_t m[3] = {spectate::kPktNoLive, 0, spectate::kRefuseEarly};
        session.send(slot, m, 3);
        continue;
    }
    auto d = spectate::decide_join(spec_live_ok_, H->total(), (int)spec_snap_.size(), spec_snap_.count(slot) > 0, rbe ? rbe->resyncing() : false);
    if (d == spectate::JoinDecision::Ignore || d == spectate::JoinDecision::Busy) continue;
    if (d == spectate::JoinDecision::NoLiveEarly) { spec_nolive(slot, spectate::kRefuseEarly); continue; }
    if (d == spectate::JoinDecision::NoLiveUnsupported) { spec_nolive(slot, spectate::kRefuseUnsupported); continue; }
    auto x = std::make_unique<SpecSnap>();
    uint64_t t0 = SDL_GetPerformanceCounter();
    if (rbe) {
        if (!rbe->snapshot_confirmed(&x->raw, &x->frame, &x->ro, &x->ri) || x->frame > H->total()) continue;   // later: it knocks again
    } else {   // lockstep, between two frames: the state before sample total() (one input poll per frame)
        x->raw.assign(core.serialize_size(), 0);
        if (x->raw.empty() || !core.serialize(x->raw.data(), x->raw.size())) { spec_nolive(slot, spectate::kRefuseUnsupported); continue; }
        x->frame = H->total();
    }
    double copy_ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
    x->t_join = now;
    SpecSnap* px = x.get();
    x->packed = !std::getenv("RETRORUN_TEST_SPECTATE_RAW");   // QA: send the raw state (a worst-case, incompressible-sized transfer)
    x->th = std::thread([px] {   // compression and hashing off the main thread: the players never wait for it
        if (px->packed) {
            mz_ulong zn = mz_compressBound((mz_ulong)px->raw.size());
            px->z.assign(zn, 0);
            px->ok = mz_compress2(px->z.data(), &zn, px->raw.data(), (mz_ulong)px->raw.size(), 6) == MZ_OK;
            px->z.resize(px->ok ? zn : 0);
        } else { px->z = px->raw; px->ok = true; }
        px->raw_hash = rb::hash_bytes(px->raw.data(), px->raw.size());
        px->ready = true;
    });
    rlog("spectate: live join for slot %d: state before confirmed frame %u (%zu bytes, copied in %.2f ms), compressing in the background", slot,
         x->frame, x->raw.size(), copy_ms);
    spec_snap_[slot] = std::move(x);
    }
}

void Frontend::spec_on_xfer(int slot, const uint8_t* d, size_t n) {
    auto it = spec_snap_.find(slot);
    if (it == spec_snap_.end() || !it->second->sending || n < 4 || d[2] != slot) return;
    SpecSnap& x = *it->second;
    if (d[3] != x.snd.epoch()) return;
    if (d[1] == rb::xfer::Ack && n >= 4 + 2 + 8) x.snd.on_ack(slot, rb::xfer::g16(d + 4), rb::xfer::g64(d + 6));
    else if (d[1] == rb::xfer::Done && n >= 5 && d[4] == 1) x.snd.on_done(slot);
}

void Frontend::spec_snap_service(uint32_t sm) {
    uint32_t now = hubnet::now_ms();
    for (auto it = spec_snap_.begin(); it != spec_snap_.end();) {
        const int s = it->first;
        SpecSnap& x = *it->second;
        const bool gone = !(sm >> s & 1u);
        if (!x.sending && !gone) spec_h()->extend_hold(s, now);   // compressing: the hold must not expire into a stream from 0
        if (!x.sending && x.ready) {
            if (x.th.joinable()) x.th.join();   // finished: no wait
            const bool rel = session.is_relayed(s);
            const size_t cap = std::min<size_t>(rel ? 1500000 : 3000000, rb::xfer::kMaxChunks * rb::xfer::kChunk);   // relay: keep the room's budget
            if (gone || !x.ok || x.z.empty() || x.z.size() > cap) {
                if (!gone) { rlog("spectate: live state for slot %d is %zu bytes compressed (cap %zu): from frame 0 instead", s, x.z.size(), cap); spec_nolive(s, spectate::kRefuseTooBig); }
                it = spec_snap_.erase(it);
                continue;
            }
            rb::xfer::Meta m;
            m.frame = x.frame; m.raw = (uint32_t)x.raw.size(); m.comp = (uint32_t)x.z.size(); m.flags = x.packed ? 1 : 0;
            m.chunks = (uint16_t)((x.z.size() + rb::xfer::kChunk - 1) / rb::xfer::kChunk);
            m.hash = x.raw_hash; m.a = x.ro; m.b = x.ri;
            spec_h()->start_peer_at(s, x.frame, now);   // inputs from F stream right away; the spectator buffers them while the state downloads
            spec_snap_epoch_ = (uint8_t)((spec_snap_epoch_ + 1) & 0x7F);
            x.snd.start((uint8_t)(0x80 | spec_snap_epoch_), m, std::move(x.z), 1u << s, now ? now : 1, rel ? 100.0 : 1200.0);   // direct: ~840 KB/s; relay: the room's frame cap
            x.raw.clear(); x.raw.shrink_to_fit();
            x.sending = true; x.t_send = now;
            rlog("spectate: sending the live state to slot %d: frame %u, %u bytes, %u on the wire in %u chunks (%s), ready %u ms after the join", s, m.frame, m.raw,
                 m.comp, (unsigned)m.chunks, rel ? "relay" : "direct", now - x.t_join);
        }
        if (x.sending) {
            if (gone) { it = spec_snap_.erase(it); continue; }
            x.snd.service(now ? now : 1, a.slot, [&](const uint8_t* d, size_t n) { session.send(s, d, (int)n); });
            if (x.snd.all_done()) {
                rlog("spectate: slot %d loaded the live state %u ms after its join (%zu chunks sent for %u)", s, now - x.t_join, x.snd.chunks_sent(), (unsigned)x.snd.meta().chunks);
                spec_snap_done_[s] = now;
                it = spec_snap_.erase(it);
                continue;
            }
            if (now - x.t_send > 30000) { rlog("spectate: live state transfer to slot %d timed out", s); it = spec_snap_.erase(it); continue; }
        }
        ++it;   // still compressing (a gone spectator's entry waits for its thread instead of joining it here)
    }
}

void Frontend::spec_host_finish() {
    spec_snap_.clear();   // joins a compressing thread, if any (the match is over: nothing waits on us)
    if (!spec_host_ || !session.spectator_mask()) return;
    spec_host_->finish();
    uint32_t t0 = hubnet::now_ms();
    while (hubnet::now_ms() - t0 < 4000 && !session.failed()) {
        session.update();
        int from; std::vector<uint8_t> m;
        while (session.recv(&from, &m)) if (!m.empty() && m[0] == spectate::kPktAck) spec_host_->on_ack(from, m.data(), m.size(), hubnet::now_ms());
        spec_host_service();
        if (spec_host_->all_acked(session.spectator_mask()) && hubnet::now_ms() - t0 > 300) break;
        Sleep(5);
    }
    rlog("spectate: served %u frames; spectator hashes ok %u, different %u", spec_host_->total(), spec_host_->hashes_ok(), spec_host_->hashes_bad());
}

/* Rollback spectator: no engine, no prediction. After the same warm-up as the players it plays the host's confirmed frames in order
 * (far behind the live game it runs without pacing until it has caught up) and checks its rolling output hash against the host's. */
int Frontend::run_rb_spectator() {
    rb::Sim sim = make_rb_sim();
    spectate::Client cli;
    /* live join: ask the host for a snapshot of the running match (spectate.h kPktJoin); an old host, a young match or a core whose
     * state can't be sent answers by streaming from frame 0 (the old replay) */
    spectate::JoinClient jc(!env_is("RETRORUN_NO_LIVE_SPECTATE"));
    spectate::CatchUp cu;
    rb::xfer::Receiver rcv;
    uint32_t dl_progress = 0, done_sent = 0;
    size_t dl_got = 0;
    uint64_t roll = rb::kRollBasis;
    uint32_t f = 0, interval = 120, last_rx_chk = hubnet::now_ms(), t_join = hubnet::now_ms(), f_start = 0;
    if (const char* hi = std::getenv("RB_HASH_INTERVAL")) interval = (uint32_t)std::max(1, atoi(hi));
    uint64_t t_start = SDL_GetTicks64();
    int rc = 0;
    bool catching = false, reported_live = false;
    auto send_done = [&] {
        auto v = rb::xfer::header(rb::xfer::Done, a.slot, rcv.epoch());
        v.push_back(1);
        session.send(0, v.data(), (int)v.size());
        done_sent = hubnet::now_ms() ? hubnet::now_ms() : 1;
    };
    for (;;) {
        if (quit) break;
        if (a.frames >= 0 && f >= (uint32_t)a.frames) break;
        pump_events(false);
        if (quit) break;
        session.update();
        int from; std::vector<uint8_t> m;
        const uint32_t now0 = hubnet::now_ms();
        while (session.recv(&from, &m)) {
            if (from != 0 || m.empty()) continue;
            if (m[0] == spectate::kPktData) {
                if (jc.mode() == spectate::JoinClient::Mode::Knock) {
                    // an old host streams from sample 0 at once; inputs from F > 0 that overtook our Begin are dropped (the host resends them)
                    if (m.size() < 6 || rb::xfer::g32(m.data() + 2) != 0) continue;
                    jc.on_legacy_data();
                }
                if (cli.on_data(m.data(), m.size(), now0)) last_rx_chk = now0;
            } else if (m[0] == spectate::kPktNoLive && m.size() >= 3) {
                if (jc.mode() == spectate::JoinClient::Mode::Knock) {
                    jc.on_nolive(m[2]);
                    rlog("spectate: the host has no live snapshot for us (reason %u): watching from the start, fast-forwarding to the live game", (unsigned)m[2]);
                    if (m[2] != spectate::kRefuseEarly) set_status("Can't join live: replaying from the start");
                }
                last_rx_chk = now0;
            } else if (m[0] == rb::xfer::kPkt && m.size() >= 4 && m[2] == 0) {
                last_rx_chk = now0;
                if (m[1] == rb::xfer::Begin) {
                    rb::xfer::Meta mt;
                    if (rb::xfer::dec_begin(m.data(), m.size(), &mt) && jc.mode() == spectate::JoinClient::Mode::Knock && rcv.on_begin(m[3], mt)) {
                        jc.on_begin(mt.frame, now0);
                        cli.start_at(mt.frame);   // buffer the confirmed inputs from F while the state downloads
                        dl_progress = now0;
                        rlog("spectate: live join: downloading the state of frame %u (%u bytes, %u chunks) %u ms after joining", mt.frame, mt.comp, (unsigned)mt.chunks, now0 - t_join);
                    } else if (jc.live() && m[3] == rcv.epoch() && now0 - done_sent > 100) send_done();   // our Done was lost
                } else if (m[1] == rb::xfer::Chunk && m.size() >= 6) {
                    if (jc.mode() == spectate::JoinClient::Mode::Download) rcv.on_chunk(m[3], rb::xfer::g16(m.data() + 4), m.data() + 6, m.size() - 6);
                    else if (jc.live() && m[3] == rcv.epoch() && now0 - done_sent > 100) send_done();
                }
            }
        }
        const uint32_t now = hubnet::now_ms();
        if (jc.want_knock(now)) { uint8_t k[3] = {spectate::kPktJoin, (uint8_t)a.slot, 1}; session.send(0, k, 3); }
        if (jc.mode() == spectate::JoinClient::Mode::Download) {
            if (rcv.received() != dl_got) { dl_got = rcv.received(); dl_progress = now; }
            uint16_t nx; uint64_t bits;
            if (rcv.ack_due(now, &nx, &bits)) { auto v = rb::xfer::enc_ack(a.slot, rcv.epoch(), nx, bits); session.send(0, v.data(), (int)v.size()); }
            if (rcv.complete()) {
                const rb::xfer::Meta& mt = rcv.meta();
                std::vector<uint8_t> raw(mt.raw);
                bool ok = (mt.flags & 1) ? sim.unpack_state && sim.unpack_state(rcv.payload().data(), rcv.payload().size(), raw.data(), raw.size())
                                         : (raw = rcv.payload(), true);
                if (ok && rb::hash_bytes(raw.data(), raw.size()) != mt.hash) ok = false;
                if (ok) ok = sim.load(raw.data(), raw.size());
                if (!ok) { rlog("spectate: the live state did not load"); rc = 5; break; }
                f = f_start = mt.frame;
                frame_no = f;
                roll = mt.a;   // the confirmed output-hash chain at F, so our periodic hashes compare with the host's
                jc.on_loaded();
                send_done();
                rlog("spectate: live join: state of frame %u loaded %u ms after joining; %u frames buffered, %u behind the host", f, now - t_join,
                     cli.ready(f) ? cli.total() - f : 0, cli.lag());
            } else if (jc.download_stuck(now, dl_progress)) { rlog("spectate: the live state stopped arriving"); rc = lost_code(); break; }
        }
        if (jc.acks_allowed()) cli.service(now, a.slot, [this](int slot, const uint8_t* d, int n) { session.send(slot, d, n); });
        if (!jc.playing()) {
            if (session.failed() || (!session.ended() && now - last_rx_chk > 15000)) { rlog("net: connection lost, leaving"); rc = lost_code(); break; }
            Sleep(1);
            continue;
        }
        if (!cli.ready(f)) {
            if (cli.finished(f)) { rlog("spectate: the match ended"); if (a.frames < 0) rc = 8; break; }
            if (session.ended() && hubnet::now_ms() - last_rx_chk > 1500) { rlog("spectate: the host ended the session"); if (a.frames < 0) rc = 8; break; }
            if (!session.ended() && (session.failed() || hubnet::now_ms() - last_rx_chk > 10000)) { rlog("net: connection lost, leaving"); rc = lost_code(); break; }
            Sleep(1);
            continue;
        }
        last_rx_chk = hubnet::now_ms();
        uint64_t pads[4];
        for (int p = 0; p < 4; p++) pads[p] = owner[p] >= 0 ? cli.get(f, p) : 0;
        bool behind = cu.update(cli.lag(), hubnet::now_ms());
        if (behind != catching) { catching = behind; if (behind) rlog("spectate: catching up, %u frames behind", cli.lag()); next_deadline = 0; }
        if (cu.live_once() && !reported_live) {
            reported_live = true;
            rlog("spectate: LIVE at frame %u, %u frames behind the host, %u ms after joining (%s, %u frames fast-forwarded)", f, cli.lag(), cu.live_at() - t_join,
                 jc.live() ? "live snapshot" : "replay from frame 0", f - f_start);
        }
        if (!catching) do_pacing();
        sim.step(pads, !catching);   // fast-forward: no video, no audio
        roll = rb::roll_step(roll, sim.frame_sum());
        f++;
        frame_no = f;
        cli.consumed(f);
        if (f % interval == 0) {
            cli.add_local_hash(f, roll);
            rlog("sync check frame %u output %016llx", f, (unsigned long long)roll);
        }
        uint32_t ms; uint64_t mine, host;
        while (cli.poll_mismatch(&ms, &mine, &host)) { rlog("DESYNC (spectator) at frame %u: mine %016llx host %016llx", ms, (unsigned long long)mine, (unsigned long long)host); rc = rc ? rc : 5; }
        if (!a.headless && !catching) present();
    }
    out_hash = roll;
    rlog("spectate: done, %u frames in %.2f s, hashes ok %u different %u", f, (SDL_GetTicks64() - t_start) / 1000.0, cli.hashes_ok(), cli.hashes_bad());
    return rc;
}

/* Cores do not restore their very first frames faithfully (BIOS / boot state), so rollback never starts from power-on:
 * every PC first runs the same few neutral frames (deterministic, no network needed). */
void Frontend::rb_warmup() {
    for (int i = 0; i < 12; i++) { memset(frame_pad, 0, sizeof frame_pad); resim = true; core.run(); resim = false; }
}

void Frontend::rb_log_stats() {
    if (!rbe) return;
    const rb::Stats& s = rbe->stats();
    double secs_f = s.frames ? (double)s.frames / 60.0 : 1.0;
    rlog("rollback-stats: input-delay %d frames (range %d-%d, %llu changes, rtt %.0f ms), frames %llu, rollbacks %llu (%.2f/s of game time), resim frames %llu, max depth %d, avg depth %.2f, stalls %llu (%llu ms), sync-waits %llu, max-skew %.2f, hashes compared %llu",
         rbe->delay(), s.delay_min, s.delay_max, (unsigned long long)s.delay_changes, s.rtt_ms, (unsigned long long)s.frames, (unsigned long long)s.rollbacks, s.rollbacks / secs_f, (unsigned long long)s.resim_frames, s.max_depth,
         s.rollbacks ? (double)s.depth_sum / (double)s.rollbacks : 0.0, (unsigned long long)s.stalls, (unsigned long long)s.stalled_ms,
         (unsigned long long)s.sync_waits, s.max_skew, (unsigned long long)s.hashes_compared);
    rlog("rollback-timing: serialize %.1f us avg, unserialize %.1f us avg, retro_run %.1f us avg", s.save_n ? s.save_us / s.save_n : 0.0,
         s.load_n ? s.load_us / s.load_n : 0.0, s.step_n ? s.step_us / s.step_n : 0.0);
}

int Frontend::run_synctest() {
    if (!core.serialize_size()) { rlog("SYNCTEST: core has no save states"); return 2; }
    int frames = a.frames > 0 ? a.frames : 1800;
    rb_warmup();
    if (std::getenv("RB_MEMINFO"))
        for (unsigned k = 0; k < 10; k++)
            for (unsigned t = 0; t < 5; t++) { size_t sz = 0; void* m = core.memory(t | (k << 8), &sz); if (m && sz) rlog("MEMINFO id type %u inst-field %u: %zu bytes", t, k, sz); }
    rb::Sim sim = make_rb_sim();
    int nports = 0;
    int ports[4];
    for (int i = 0; i < 4; i++) if (owner[i] >= 0) ports[nports++] = i;
    if (!nports) { ports[0] = 0; nports = 1; }
    uint64_t seed = 0x1234567;
    auto input = [&](int f, uint64_t pads[4]) {
        for (int i = 0; i < 4; i++) pads[i] = 0;
        for (int k = 0; k < nports; k++) {
            uint32_t x = (uint32_t)(f / 2) * 2654435761u ^ (uint32_t)(ports[k] + 1) * 40503u ^ (uint32_t)seed;
            x ^= x >> 15; x *= 2246822519u; x ^= x >> 13; x *= 3266489917u; x ^= x >> 16;
            uint32_t b = std::getenv("RB_NOINPUT") ? 0 : (x & 0xFF3u & ~0xCu);
            int16_t st[4] = {0, 0, 0, 0};
            if (is_ps1 && !std::getenv("RB_NOINPUT"))
                for (int j = 0; j < 4; j++) { uint32_t y = (x + (uint32_t)j * 0x9E3779B9u) * 2246822519u; y ^= y >> 15; st[j] = (int16_t)((int)(y & 0xFFFF) - 32768); }
            pads[ports[k]] = is_ps1 ? retrorun::pack_pad(b, st) : (uint64_t)b;
        }
        if (is_ps1) {   // QA: RB_SWAPDISC=N[,M] swaps to the next disc at those input frames (exercises the deterministic swap under rollback)
            static std::vector<int> sw;
            static bool sw_init = false;
            if (!sw_init) { sw_init = true; if (const char* e = std::getenv("RB_SWAPDISC")) for (const char* q = e; *q;) { sw.push_back(atoi(q)); const char* c = strchr(q, ','); if (!c) break; q = c + 1; } }
            for (size_t k = 0; k < sw.size(); k++) if (f == sw[k]) pads[ports[0]] |= retrorun::disc_event((unsigned)(k + 1) % std::max(1u, core.disc_count()));
        }
    };
    rlog("SYNCTEST: %d frames, rollback depth %d, state %zu bytes, compare %s", frames, a.synctest, core.serialize_size(),
         "video + memory hash");
    uint64_t t0 = SDL_GetTicks64();
    rb::SyncTestResult r = rb::run_synctest(sim, frames, a.synctest, true, input, [](const std::string& m) { rlog("%s", m.c_str()); });
    rlog("SYNCTEST timing: serialize %.1f us avg (max %.1f), unserialize %.1f us avg (max %.1f), hash %.1f us, retro_run %.1f us avg; %llu ms total",
         r.save_us, r.save_us_max, r.load_us, r.load_us_max, r.hash_us, r.step_us, (unsigned long long)(SDL_GetTicks64() - t0));
    rlog("SYNCTEST frames %llu, re-simulated frames compared %llu, mismatches %llu -> %s", (unsigned long long)r.frames, (unsigned long long)r.checks,
         (unsigned long long)r.mismatches, r.mismatches ? "FAIL" : "PASS");
    return r.mismatches ? 1 : 0;
}

int Frontend::selftest() {
    // 1) in-memory round trip, 2) file round trip through the same code the F5/F7 hotkeys use.
    size_t n = core.serialize_size();
    if (!n) { rlog("SELFTEST: core has no save states"); return 2; }
    int warm = a.frames > 0 ? a.frames : 300;
    for (int i = 0; i < warm; i++) { memset(frame_pad, 0, sizeof frame_pad); core.run(); frame_no++; }
    std::vector<uint8_t> s0(n), s1(n);
    core.serialize(s0.data(), n);
    uint64_t h_s0 = n64run::hash_block(s0.data(), n);
    core.run();
    uint64_t h_a = state_hash();
    bool ok1 = core.unserialize(s0.data(), n);
    core.serialize(s1.data(), n);
    bool same_restore = memcmp(s0.data(), s1.data(), n) == 0;
    core.run();
    uint64_t h_b = state_hash();
    rlog("SELFTEST serialize size %zu, state hash %016llx", n, (unsigned long long)h_s0);
    size_t ndiff = 0;
    for (size_t i = 0; i < n; i++) ndiff += s0[i] != s1[i];
    rlog("SELFTEST restore reproduces serialized bytes: %s (%zu of %zu bytes differ)", same_restore && ok1 ? "yes" : "NO", ndiff, n);
    rlog("SELFTEST next-frame hash original %016llx, after unserialize %016llx -> %s", (unsigned long long)h_a,
         (unsigned long long)h_b, h_a == h_b ? "IDENTICAL" : "DIFFERENT");
    // file round trip
    bool saved = save_state("selftest");
    core.run();
    uint64_t h_c = state_hash();
    bool loaded = load_state("selftest");
    core.run();
    uint64_t h_d = state_hash();
    rlog("SELFTEST file state: save=%d load=%d hash-after-run %016llx vs %016llx -> %s", saved, loaded,
         (unsigned long long)h_c, (unsigned long long)h_d, (h_c == h_d) ? "IDENTICAL" : "DIFFERENT");
    std::error_code ec;
    fs::remove(fs::u8path(state_path("selftest")), ec);
    {   // multi-frame: does load + N frames track the original N frames? (what rollback needs)
        std::vector<uint8_t> s2(n);
        core.serialize(s2.data(), n);
        uint64_t ho[16], hr[16], vo[16], vr[16];
        for (int i = 0; i < 16; i++) { core.run(); ho[i] = state_hash(); vo[i] = n64run::hash_block(core.frame().px.data(), core.frame().px.size() * 4); }
        core.unserialize(s2.data(), n);
        int first_bad = -1, first_bad_v = -1;
        for (int i = 0; i < 16; i++) {
            core.run(); hr[i] = state_hash(); vr[i] = n64run::hash_block(core.frame().px.data(), core.frame().px.size() * 4);
            if (hr[i] != ho[i] && first_bad < 0) first_bad = i;
            if (vr[i] != vo[i] && first_bad_v < 0) first_bad_v = i;
        }
        rlog("SELFTEST 16-frame replay after unserialize: first state divergence at step %d, first video divergence at step %d (-1 = none)", first_bad, first_bad_v);
    }
    bool pass = ok1 && same_restore && h_a == h_b && saved && loaded && h_c == h_d;
    rlog("SELFTEST %s", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

int Frontend::run(const Args& args) {
    a = args;
    g_fe = this;
    muted = env_is("N64RUN_MUTE") || env_is("RETRORUN_MUTE") || env_is("SSB64_MUTE");
    if (!a.log.empty()) rlog_set_file(a.log);
    if (a.save_dir.empty()) a.save_dir = ".\\saves";
    if (a.state_dir.empty()) a.state_dir = a.save_dir + "\\states";
    if (a.headless && a.frames < 0) a.frames = 600;
    if (!a.net.empty() && a.players < 1) a.players = 1;

    // Port ownership: --ports, else (offline) the first --players ports are all local.
    for (int i = 0; i < 4; i++) owner[i] = a.ports[i];
    if (a.ds_link) { for (int i = 0; i < 4; i++) owner[i] = i == 0 ? a.slot : -1; a.ports_given = true; }   // every PC plays the DS's only pad
    if (a.net.empty() && !a.ports_given && !a.ds_link)
        for (int i = 0; i < 4; i++) owner[i] = i < std::max(1, a.players) ? a.slot : -1;
    for (int i = 0; i < 4; i++)
        if (owner[i] == a.slot && owner[i] >= 0) local_ports[n_local++] = i;

    setup_paths();
    rlog("retrorun: %s | %s | system=%s | %s", a.core.c_str(), a.rom.c_str(), a.system.empty() ? "?" : a.system.c_str(),
         a.headless ? "headless" : "windowed");

    Uint32 init = SDL_INIT_TIMER | SDL_INIT_EVENTS;
    if (!a.headless) init |= SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER;
    if (SDL_Init(init) != 0) { rlog("SDL_Init failed: %s", SDL_GetError()); return 1; }
    LARGE_INTEGER fq;
    QueryPerformanceFrequency(&fq);
    qpc_freq = fq.QuadPart;

    core.on_rumble = [this](unsigned port, bool strong, uint16_t str) {   // libretro rumble -> the pad of that local port
        if (a.headless) return;
        for (int i = 0; i < n_local; i++)
            if ((unsigned)local_ports[i] == port && (size_t)i < pads.size()) {
                rumble_lvl[i][strong ? 0 : 1] = str;
                SDL_GameControllerRumble(pads[i], rumble_lvl[i][0], rumble_lvl[i][1], rumble_lvl[i][0] || rumble_lvl[i][1] ? 60000 : 0);
            }
    };
    core.on_input = [this](unsigned port, unsigned dev, unsigned idx, unsigned id) -> int16_t {
        if ((dev & RETRO_DEVICE_MASK) == RETRO_DEVICE_POINTER && is_ds && port == 0 && idx == 0) {
            // Stylus: the mouse over the drawn frame (libretro pointer coordinates span the displayed frame).
            if (a.headless && a.stylus_u >= 0) {
                if (id == RETRO_DEVICE_ID_POINTER_X) return (int16_t)std::lround(a.stylus_u * 65534.0 - 32767.0);
                if (id == RETRO_DEVICE_ID_POINTER_Y) return (int16_t)std::lround(a.stylus_v * 65534.0 - 32767.0);
                return (id == RETRO_DEVICE_ID_POINTER_PRESSED || id == RETRO_DEVICE_ID_POINTER_COUNT) ? 1 : 0;
            }
            if (a.headless || !win) return 0;
            int mx = 0, my = 0;
            Uint32 b = SDL_GetMouseState(&mx, &my);
            if (mx != last_mouse_x || my != last_mouse_y || (b & SDL_BUTTON(SDL_BUTTON_LEFT))) { if (last_mouse_x >= 0) pad_cursor.active = false; last_mouse_x = mx; last_mouse_y = my; }
            if (pad_cursor.active) {
                switch (id) {
                    case RETRO_DEVICE_ID_POINTER_X: return (int16_t)pad_cursor.px();
                    case RETRO_DEVICE_ID_POINTER_Y: return (int16_t)pad_cursor.py();
                    case RETRO_DEVICE_ID_POINTER_COUNT:
                    case RETRO_DEVICE_ID_POINTER_PRESSED: return (int16_t)(pad_touch ? 1 : 0);
                    default: return 0;
                }
            }
            int ww = 0, wh = 0, ow = 0, oh = 0;
            SDL_GetWindowSize(win, &ww, &wh);
            SDL_GetRendererOutputSize(ren, &ow, &oh);
            if (ww > 0 && wh > 0) { mx = mx * ow / ww; my = my * oh / wh; }   // window -> renderer pixels (high DPI)
            int px = 0, py = 0;
            bool in = dsview::pointer_from_window(mx, my, {last_dst.x, last_dst.y, last_dst.w, last_dst.h}, &px, &py);
            switch (id) {
                case RETRO_DEVICE_ID_POINTER_X: return (int16_t)px;
                case RETRO_DEVICE_ID_POINTER_Y: return (int16_t)py;
                case RETRO_DEVICE_ID_POINTER_COUNT: return (int16_t)(in ? 1 : 0);
                case RETRO_DEVICE_ID_POINTER_PRESSED: return (int16_t)(stylus.update((b & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0, in) ? 1 : 0);
                default: return 0;
            }
        }
        if (is_ps2 && (dev & RETRO_DEVICE_MASK) == RETRO_DEVICE_ANALOG && port < 4 && idx == RETRO_DEVICE_INDEX_ANALOG_BUTTON) {   // DualShock 2 pressure
            if (id == RETRO_DEVICE_ID_JOYPAD_L2 || id == RETRO_DEVICE_ID_JOYPAD_R2) {
                uint32_t pr = frame_pad[port] >> (id == RETRO_DEVICE_ID_JOYPAD_L2 ? 16 : 24) & 0xFF;
                return (int16_t)(pr * 32767 / 255);
            }
            return id < 16 ? (int16_t)(((frame_pad[port] >> id) & 1) ? 32767 : 0) : 0;
        }
        if ((is_ps1 || is_ps2) && (dev & RETRO_DEVICE_MASK) == RETRO_DEVICE_ANALOG && port < 4) {   // DualShock sticks (and analog button pressure)
            if (idx == RETRO_DEVICE_INDEX_ANALOG_LEFT || idx == RETRO_DEVICE_INDEX_ANALOG_RIGHT) {
                if (id > RETRO_DEVICE_ID_ANALOG_Y) return 0;
                return frame_stick[port][(idx == RETRO_DEVICE_INDEX_ANALOG_RIGHT ? 2 : 0) + (int)id];
            }
            if (idx == RETRO_DEVICE_INDEX_ANALOG_BUTTON) return id < 16 ? (int16_t)(((frame_pad[port] >> id) & 1) ? 32767 : 0) : 0;
            return 0;
        }
        if ((dev & RETRO_DEVICE_MASK) != RETRO_DEVICE_JOYPAD || port >= 4 || idx != 0) return 0;
        uint32_t m = frame_pad[port];
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)(m & 0xFFFF);
        return id < 16 ? (int16_t)((m >> id) & 1) : 0;
    };
    bool drc = false;
    core.on_audio = [this, &drc](const int16_t* d, size_t n) {
        if (rbe || a.synctest) { if (!link) for (size_t i = 0; i < n * 2; i++) fr_audio = (fr_audio ^ (uint16_t)d[i]) * 0x100000001B3ull; }
        else if (!link) for (size_t i = 0; i < n * 2; i++) out_hash = (out_hash ^ (uint16_t)d[i]) * 0x100000001B3ull;
        if (!ff && !resim && !spec_ && audio.active() && speed <= 2.0) audio.push(d, n, drc && speed == 1.0);   // above 2x the audio is dropped, not squealed
    };

    std::string err;
    if (!core.load_core(a.core, &err)) { rlog("ERROR: %s", err.c_str()); return 1; }
    if (a.link) {
        link = true;
        std::vector<std::string> roms;
        if (!a.link_roms.empty()) {
            size_t p = 0;
            while (p <= a.link_roms.size()) { size_t q = a.link_roms.find(',', p); if (q == std::string::npos) q = a.link_roms.size(); roms.push_back(a.link_roms.substr(p, q - p)); p = q + 1; }
        }
        if (roms.empty()) roms.assign(2, a.rom);
        link_n = (int)roms.size();
        unsigned gt = 0x101;
        for (auto& ss : core.subsystems()) if (ss.num_roms == (unsigned)link_n) { gt = ss.id; break; }
        if (!core.load_game_special(gt, roms, &err)) { rlog("ERROR: %s", err.c_str()); return 1; }
    } else {
        if (is_ps2 && !a.net.empty() && a.share_host_save && !ps2_card_presync()) { rlog("net: memory card sharing failed"); return net_rc; }
        if (!core.load_game(a.rom, &err)) { rlog("ERROR: %s", err.c_str()); return 1; }
    }
    fps = core.av().timing.fps > 1 ? core.av().timing.fps : 60.0;
    if (is_ps1) {   // every port is a DualShock (digital pads work too; games switch the analog mode themselves)
        for (unsigned p = 0; p < 4; p++) core.set_port_device(p, 0x205);   // RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 1)
    }
    if (lower(a.system) == "snes" && !a.no_multitap && (a.multitap || owner[2] >= 0 || owner[3] >= 0 || (!a.headless && SDL_NumJoysticks() >= 3))) {
        core.set_port_device(1, 0x101);   // snes9x RETRO_DEVICE_JOYPAD_MULTITAP ((1 << 8) | RETRO_DEVICE_JOYPAD): players 3-4 (libretro ports 2-3)
        rlog("snes: multitap on port 2 (players 3-4)");
    }
    if (a.rollback && !a.synctest) {
        // Only cores whose savestates were verified exact (SyncTest against a free-running reference) may roll back; others stay lockstep.
        std::string ln = lower(core.info().library_name ? core.info().library_name : "");
        bool safe = false;
        std::string why;
        if (ln.find("mgba link") != std::string::npos) safe = true;
        else if (ln.find("nestopia") != std::string::npos || ln.find("mesen") != std::string::npos) safe = true;   // NES: SyncTest exact (qa/nes_snes_online.ps1)
        else if (ln.find("snes9x") != std::string::npos) safe = true;   // Snes9x (SNES): SyncTest exact
        else if (ln.find("sameboy (rollback)") != std::string::npos) safe = true;   // src/sameboy_rb: joypad-bounce + RNG patched, SyncTest exact
        else if (ln.find("beetle psx (rollback)") != std::string::npos) safe = true;   // src/beetle_psx/patch.cmake: surface + scan-out cache in the state, SyncTest exact
        else if (ln.find("beetle psx") != std::string::npos) why = "stock Beetle PSX: the video surface is not in its savestates (use the RetroHub build)";
        else if (ln.find("sameboy") != std::string::npos)
            why = "stock SameBoy: joypad-bounce state is not in its savestates (use sameboy_rb_libretro)";
        else if (ln.find("gambatte") != std::string::npos) why = "Gambatte states are not exact";
        else why = "core " + ln + " is not verified for rollback";
        if (!safe && std::getenv("RETRORUN_FORCE_ROLLBACK")) { rlog("rollback: forced on an unverified core (RETRORUN_FORCE_ROLLBACK)"); safe = true; }
        if (!safe) { rlog("rollback: %s -> staying on lockstep netplay", why.empty() ? "unsupported core" : why.c_str()); a.rollback = false; }
        else rlog("rollback: core %s verified for rollback netplay", ln.c_str());
    }
    if (is_ds) {   // every option retrorun relies on must exist in the real core with that value, else F9/defaults would silently fail
        int nreq = 0;
        const dsview::ReqOpt* req = dsview::required_options(&nreq);
        for (int i = 0; i < nreq; i++) {
            auto it = core.options().find(req[i].key);
            bool ok = it != core.options().end() &&
                      std::find(it->second.values.begin(), it->second.values.end(), req[i].value) != it->second.values.end();
            if (!ok) rlog("WARNING: DS core option %s=%s is not offered by this core", req[i].key, req[i].value);
        }
    }
    if (a.list_opts) {
        for (auto& kv : core.options()) {
            std::string vals;
            for (auto& v : kv.second.values) vals += (vals.empty() ? "" : "|") + v;
            printf("%s = %s   [%s]  %s\n", kv.first.c_str(), kv.second.def.c_str(), vals.c_str(), kv.second.desc.c_str());
        }
        return 0;
    }

    {
        std::string ln = lower(core.info().library_name ? core.info().library_name : "");
        // Gambatte's retro_serialize output differs run-to-run on identical emulation (see docs/RETRORUN.md),
        // so its state hash would flag false desyncs; fall back to a rolling video+audio output hash.
        // PlayStation: the serialized state holds bytes that are not emulation state and differs between identical runs (docs/CORES.md).
        is_ps2 = is_ps2 || ln.find("ps2") != std::string::npos || ln.find("pcsx2") != std::string::npos;
        use_state_hash = a.sync_hash_mode == "state" || (a.sync_hash_mode == "auto" && ln.find("gambatte") == std::string::npos && !is_ps1);
        if (use_state_hash && !core.serialize_size()) use_state_hash = false;
        rlog("sync hash: %s", use_state_hash ? "serialized state" : "video+audio output");
    }
    audio.set_volume(a.volume);
    if (!a.headless && !muted) audio.open(core.av().timing.sample_rate > 1000 ? core.av().timing.sample_rate : 32768.0);
    if (!setup_video()) return 1;
    drc = pace == Pace::Vsync;
    if (!a.headless && pace == Pace::None) pace = audio.active() ? Pace::Audio : Pace::Timer;
    {
        std::string cf = a.controls;
        if (cf.empty()) { const char* e = std::getenv("RETROHUB_CONTROLS"); if (e && *e) cf = e; }
        const std::string psys = is_ps1 || is_ps2 ? "ps1"  /* PS2 online: DualShock layout of the ps1 profile */ : (lower(a.system) == "nes" || lower(a.system) == "snes") ? lower(a.system) : "retro";
        if (psys != "retro") prof = hubctl::default_profile(psys);
        if (!cf.empty()) {
            hubctl::Profile p;
            if (hubctl::read_profile_file(cf, &p) && p.system == psys) { prof = p; rlog("controls: profile '%s' (%s)", p.name.c_str(), cf.c_str()); }
            else rlog("controls: cannot use %s; built-in mapping", cf.c_str());
        }
        retro_pad.reset(new retrorun::RetroPad(prof));
        if (!a.headless) hubctl::load_bundled_db();
    }
    if (!a.headless) open_pads();
    core.on_geometry = [this]() { fps = core.av().timing.fps > 1 ? core.av().timing.fps : fps; };

    if (link) {
        // Offline (no --net): every console with a local owner loads its own save from disk.
        if (a.net.empty())
            { for (int i = 0; i < link_n; i++) if (owner[i] >= 0) link_apply_blob(i, link_blob_from_disk(i)); link_loaded = true; }
    } else load_srm();
    ra_start();
    if (!link && a.resume && !online && a.net.empty() && !a.ds_link) {
        if (fs::exists(fs::u8path(state_path("auto")))) load_state("auto");
    }
    if (!a.load_state_path.empty() && !link && !online && a.net.empty() && !a.ds_link && ra_.allow(cheevos::Feature::LoadState)) {
        std::vector<uint8_t> sb;
        if (read_file(a.load_state_path, &sb) && core.unserialize(sb.data(), sb.size())) set_status("Resumed from saved state");
        else rlog("load-state: could not restore %s", a.load_state_path.c_str());
    }

    cheats_init();
    if (!a.disc_ctl.empty()) disc_watch = cheats::FileWatch(a.disc_ctl);
    if (!a.control.empty()) ctl_ = runctl::Poller(a.control);
    if (!a.shot_dir.empty()) clips_.set_dir(a.shot_dir);
    if (const char* e = std::getenv("RETRORUN_CLIPS"); e && e[0] == '0') clips_.set_enabled(false);   // opt out of the replay buffer
    if (!a.osd_msg.empty()) osd::push(a.osd_msg, osd::Kind::Info, 3600);
    int rc = 0;
    const uint64_t hashlog_n = std::getenv("RETRORUN_HASHLOG") ? (uint64_t)atoll(std::getenv("RETRORUN_HASHLOG")) : 0;   // QA/determinism spike: log state+output hash every N frames
    if (a.selftest) {
        rc = selftest();
    } else if (a.synctest) {
        rc = run_synctest();
    } else if (a.selftest_rewind_frames || a.selftest_runahead_frames) {
        rc = run_feat_selftest();
    } else if (rbe || (a.rollback && !a.net.empty())) {
        if (!rbe && !setup_net()) { rc = net_rc; goto done; }
        if (a.headless) pace = Pace::Timer;
        if (rb_spectator_) {
            rc = run_rb_spectator();
        } else {
            uint64_t t_start = SDL_GetTicks64(), last_stat = t_start;
            uint64_t last_rb = 0;
            for (;;) {
                if (quit) break;
                if (a.frames >= 0 && rbe->cur() >= (uint32_t)a.frames) break;
                pump_events(false);
                if (quit) break;
                if (session.failed()) { rlog("net: connection lost, leaving"); rc = lost_code(); break; }
                rb::Engine::Status st = rbe->prepare();
                if (rbe->connection_lost()) { rlog("net: connection lost, leaving"); rc = lost_code(); break; }
                if (rbe->desync()) { rc = 5; break; }
                if (a.frames < 0 && rbe->all_remote_left()) { rlog("net: every partner left, game ended"); rc = 8; break; }
                if (st == rb::Engine::Status::Stalled) { Sleep(1); next_deadline = 0; continue; }
                if (st == rb::Engine::Status::SyncWait) { Sleep((DWORD)(1000.0 / (fps > 1 ? fps : 60.0))); next_deadline = 0; continue; }   // re-anchor: otherwise pacing "catches up" the frame we just waited and time sync does nothing
                cheats_poll();
                disc_poll();
                ctl_poll();
                do_pacing();
                rbe->run_frame();
                frame_no = rbe->cur();
                note_frame_gap();
                spec_host_service();
                if (uint32_t dm = rbe->dropped_mask(); dm != rb_dropped_seen_) {
                    for (int sl = 0; sl < 8; sl++) if ((dm & ~rb_dropped_seen_) >> sl & 1u) set_status("Player in slot " + std::to_string(sl) + " disconnected - playing on");
                    rb_dropped_seen_ = dm;
                }
                if (!a.headless) present();
                if (SDL_GetTicks64() - last_stat >= 10000) {
                    last_stat = SDL_GetTicks64();
                    const rb::Stats& s = rbe->stats();
                    rlog("rollback: frame %llu rollbacks %llu (%.1f/s) max depth %d stalls %llu sync-waits %llu", (unsigned long long)frame_no,
                         (unsigned long long)s.rollbacks, (double)s.rollbacks * 1000.0 / (double)(last_stat - t_start), s.max_depth,
                         (unsigned long long)s.stalls, (unsigned long long)s.sync_waits);
                    (void)last_rb;
                }
                if ((frame_no & 63) == 0 && SDL_GetTicks64() - last_srm_check > 30000) {
                    last_srm_check = SDL_GetTicks64();
                    if (link) link_save(false); else save_srm(false);
                }
            }
            double secs = (SDL_GetTicks64() - t_start) / 1000.0;
            if (rc == 0 || rc == 5) {
                bool ok = rbe->finish(8000);
                if (!ok) rlog("net: finish: inputs not fully confirmed before timeout");
                if (rbe->desync()) { rlog("RESULT: DESYNC detected"); rc = 5; }
            }
            rlog("frame-gap: max %.1f ms, %llu gaps over 50 ms", gap_max_ms_, (unsigned long long)gap_over_);
            spec_host_finish();
            out_hash = rbe->confirmed_out_hash();
            in_hash = rbe->confirmed_in_hash();
            rb_log_stats();
            rlog("done: %llu frames in %.2f s (%.1f fps)", (unsigned long long)frame_no, secs, secs > 0 ? frame_no / secs : 0.0);
            uint32_t t0 = hubnet::now_ms();
            while (hubnet::now_ms() - t0 < 400) { session.update(); Sleep(5); }
        }
    } else {
        if (a.ds_link) {
            if (!setup_link()) { rc = 3; goto done; }
            if (a.headless) pace = Pace::Timer;
        } else if (!setup_net()) { rc = net_rc; goto done; }
        if (!online && a.net.empty() && !a.ds_link && !link && !is_ps2) feat_ = make_feat(!a.headless);
        else if (a.rewind_mode == 1 || a.runahead != 0) rlog("rewind / run-ahead are offline only: ignored here");
        {
            uint64_t t_start = SDL_GetTicks64(), last_fstat = 0;
            for (;;) {
                if (quit) break;
                if (a.frames >= 0 && frame_no >= (uint64_t)a.frames) break;
                pump_events(false);
                if (quit) break;
                if (a.ds_link && session.ended()) {   // a partner signed off on purpose: not a failure
                    rlog("link: partner ended the session");
                    link_stop("partner ended the session");
                    if (a.frames < 0) rc = 8;
                    break;
                }
                if (a.ds_link && session.failed()) { rlog("link: connection lost, leaving"); link_stop("connection lost"); rc = lost_code(); break; }

                if (want_save) { want_save = false; save_state("slot" + std::to_string(slot_sel)); }
                if (want_load) { want_load = false; if (!online && !a.ds_link) load_state("slot" + std::to_string(slot_sel)); }

                if (online && !a.spectate) { note_frame_gap(); spec_host_service(); }   // frame boundary: a spectator's live snapshot is taken here
                if (online) {
                    for (int p = 0; p < 4; p++) frame_pad[p] = lockstep->get_keys(p);   // lead port poll advances the sample
                    if (a.spectate) ls_spectator_frame();
                    if (is_ps1 || is_ps2) for (int p = 0; p < 4; p++) retrorun::unpack_sticks(lockstep->aux(p), frame_stick[p]);
                    if (lockstep->connection_lost()) { rlog("net: connection lost, leaving"); rc = lost_code(); break; }
                } else {
                    poll_all_local();
                }

                if (link_active) link_pump();
                if (link_active && link_probe && (frame_no % 20) == 0) {
                    std::vector<uint8_t> pr(2310, 0);
                    pr[9] = 0;
                    memcpy(pr.data() + 10, "RHPROBE", 7);
                    for (size_t i = 17; i < pr.size(); i++) pr[i] = (uint8_t)(i * 31);
                    link_send_probe(pr);
                }
                cheats_poll();
                disc_poll();
                ctl_poll();
                do_pacing();
                run_core_frame();
                if (link_active) {
                    link_pump();
                    if (SDL_GetTicks64() - link_last_log >= 5000) {
                        link_last_log = SDL_GetTicks64();
                        std::string per;
                        for (auto& kv : link_rx_by_slot) per += " slot" + std::to_string(kv.first) + "=" + std::to_string(kv.second);
                        rlog("link: tx %llu (cmd %llu reply %llu) rx %llu (cmd %llu reply %llu)%s", (unsigned long long)link_tx,
                             (unsigned long long)link_tx_cmd, (unsigned long long)link_tx_reply, (unsigned long long)link_rx,
                             (unsigned long long)link_rx_cmd, (unsigned long long)link_rx_reply, per.c_str());
                    }
                }
                fold_frame();
                if (a.headless) clip_frame();
                frame_no++;
                if (a.headless && a.realtime) {   // QA (qa\speed_headless.ps1): frame counter against wall time, ~every 250 ms
                    uint64_t tk = SDL_GetTicks64();
                    if (tk - last_fstat >= 250) { last_fstat = tk; rlog("fstat: frames %llu ms %llu speed %g%s", (unsigned long long)frame_no, (unsigned long long)tk, speed, speed_max ? " max" : ""); }
                }
                if (hashlog_n && frame_no % hashlog_n == 0) rlog("HASHLOG frame %llu state %016llx out %016llx video %016llx", (unsigned long long)frame_no, (unsigned long long)state_hash(), (unsigned long long)out_hash, (unsigned long long)(core.frame().px.empty() ? 0 : n64run::hash_block(core.frame().px.data(), core.frame().px.size() * 4)));

                if (!a.headless && (!ff || (frame_no & 7) == 0) && (speed <= 1.0 || frame_no % (uint64_t)std::ceil(speed) == 0)) present();

                if ((frame_no & 63) == 0 && SDL_GetTicks64() - last_srm_check > 30000) {
                    last_srm_check = SDL_GetTicks64();
                    if (link) link_save(false); else save_srm(false);
                }
            }
            double secs = (SDL_GetTicks64() - t_start) / 1000.0;
            if (online && lockstep) lockstep->log_stall_summary();
            if (online && lockstep && a.spectate)
                rlog("spectate: done, %llu frames (from %u), hashes ok %u different %u", (unsigned long long)frame_no, ls_f_start_, lockstep->spectate_client().hashes_ok(),
                     lockstep->spectate_client().hashes_bad());
            if (online && lockstep && !a.spectate) { rlog("frame-gap: max %.1f ms, %llu gaps over 50 ms", gap_max_ms_, (unsigned long long)gap_over_); spec_snap_.clear(); }
            if (online && lockstep) lockstep->finish_spectators();
            rlog("done: %llu frames in %.2f s (%.1f fps)", (unsigned long long)frame_no, secs, secs > 0 ? frame_no / secs : 0.0);
            if (!a.clip_out.empty()) {   // QA: encode the ring to the named file and decode it again
                clip::Snapshot sn = clips_.ring().snapshot();
                std::string err; clip::Info ci;
                bool wok = clip::write_mp4(sn, a.clip_out, &err);
                bool pok = wok && clip::probe_mp4(a.clip_out, &ci, &err);
                rlog("clip: %s %s (%dx%d, %zu frames in, %d decoded, %.2f s, luma %.1f, %llu bytes) %s", pok ? "OK" : "FAILED", a.clip_out.c_str(), sn.w, sn.h,
                     sn.frames.size(), ci.frames, ci.seconds, ci.mean_luma, (unsigned long long)ci.bytes, pok ? "" : err.c_str());
                if (!pok) rc = 6;
            }
        }
        if (a.ds_link) {
            if (link_probe) rlog("link: PROBE tx %llu rx %llu corrupt %llu", (unsigned long long)probe_tx, (unsigned long long)probe_rx, (unsigned long long)probe_bad);
            rlog("link: final tx %llu (cmd %llu reply %llu) rx %llu (cmd %llu reply %llu)", (unsigned long long)link_tx,
                 (unsigned long long)link_tx_cmd, (unsigned long long)link_tx_reply, (unsigned long long)link_rx,
                 (unsigned long long)link_rx_cmd, (unsigned long long)link_rx_reply);
            link_stop("closing");
            // Sign off so the partners end cleanly instead of treating our exit as a lost connection.
            for (int i = 0; i < 4; i++) {
                session.say_bye();
                uint32_t t0 = hubnet::now_ms();
                while (hubnet::now_ms() - t0 < 80) { session.update(); Sleep(5); }
            }
        }
        // Let the last input packets reach slower peers.
        if (online) {
            uint32_t t0 = hubnet::now_ms();
            while (hubnet::now_ms() - t0 < 400) { session.update(); Sleep(5); }
            if (lockstep->desync_detected()) { rlog("RESULT: DESYNC detected"); rc = rc ? rc : 5; }
            if (rc == 0 && a.frames < 0 && lockstep->peer_left() && !lockstep->desync_detected()) { rlog("net: a partner left, game ended"); rc = 8; }
        }
    }
done:
    {
        for (const auto& d : a.mem_dumps) {
            size_t msz = 0;
            uint8_t* mp = static_cast<uint8_t*>(core.memory(d.id, &msz));
            if (!mp || d.off + d.len > msz) { rlog("dump-mem: block %u unavailable or too small (%zu bytes)", d.id, msz); continue; }
            FILE* df = fopen(d.file.c_str(), "wb");
            if (df && fwrite(mp + d.off, 1, d.len, df) == d.len) rlog("dump-mem: %zu bytes of block %u at 0x%zX -> %s", d.len, d.id, d.off, d.file.c_str());
            else rlog("dump-mem: cannot write %s", d.file.c_str());
            if (df) fclose(df);
        }
        uint64_t h = core.game_loaded() ? state_hash() : 0;
        if (!a.net.empty() || a.selftest || a.headless)
            rlog("final: frame %llu state-hash %016llx output-hash %016llx input-hash %016llx", (unsigned long long)frame_no,
                 (unsigned long long)h, (unsigned long long)out_hash, (unsigned long long)in_hash);
        if (const char* dr = std::getenv("RETRORUN_DUMP_RAM")) {   // QA: system RAM at exit
            size_t rn = 0;
            void* rp = core.game_loaded() ? core.memory(RETRO_MEMORY_SYSTEM_RAM, &rn) : nullptr;
            if (rp && rn) write_atomic(dr, rp, rn);
        }
        if (!a.screenshot.empty()) {
            FrameBuf shot = core.frame();
            if ((fx_filter_ != fx::Filter::Off || bezel_on_) && shot.w > 0 && !shot.px.empty()) {   // QA: the filtered/bezelled window (RETRORUN_FX_SHOT_SIZE=WxH, default 1280x720)
                int sow = 1280, soh = 720;
                if (const char* sz = std::getenv("RETRORUN_FX_SHOT_SIZE")) sscanf(sz, "%dx%d", &sow, &soh);
                shot = fx_compose(shot, std::max(16, sow), std::max(16, soh));
            }
            if (const char* hs = std::getenv("RETRORUN_HINT_SHOT"); hs && hs[0] == '1' && shot.w > 0 && shot.px.size() >= (size_t)shot.w * shot.h) {   // QA: the tips card as seen ~1 s into a real run
                int k = std::max(1, (540 + shot.h - 1) / shot.h);   // integer-upscale small frames to a window-like size so the card has its real proportions
                if (k > 1) {
                    FrameBuf up;
                    up.w = shot.w * k; up.h = shot.h * k; up.px.resize((size_t)up.w * up.h);
                    for (int y = 0; y < up.h; y++) for (int x = 0; x < up.w; x++) up.px[(size_t)y * up.w + x] = shot.px[(size_t)(y / k) * shot.w + x / k];
                    shot = std::move(up);
                }
                starthint::Bitmap hb = starthint::render(starthint::pick_mode(false, true), starthint::scale_for_height(shot.h));
                starthint::composite(shot.px.data(), shot.w, shot.h, shot.w, hb, starthint::alpha_at(1000));
            }
            if (const char* os = std::getenv("RETRORUN_OSD_SHOT"); os && os[0] == '1' && shot.w > 0 && shot.px.size() >= (size_t)shot.w * shot.h) {   // QA: composite the OSD into the frame
                int k = std::max(1, (480 + shot.h - 1) / shot.h);
                if (k > 1) {
                    FrameBuf up;
                    up.w = shot.w * k; up.h = shot.h * k; up.px.resize((size_t)up.w * up.h);
                    for (int y = 0; y < up.h; y++) for (int x = 0; x < up.w; x++) up.px[(size_t)y * up.w + x] = shot.px[(size_t)(y / k) * shot.w + x / k];
                    shot = std::move(up);
                }
                osd::Layer ol; bool oc = false;
                if (osd::render(&ol, shot.w, shot.h, &oc)) osd::blend(shot.px.data(), shot.w, shot.h, shot.w, ol);
            }
            if (write_frame_image(a.screenshot, shot))
                rlog("screenshot: %s (%dx%d)", a.screenshot.c_str(), core.frame().w, core.frame().h);
            else
                rlog("screenshot: FAILED to write %s", a.screenshot.c_str());
        }
        if (is_ps2 && rc != 0 && frame_no == 0) { std::fflush(nullptr); TerminateProcess(GetCurrentProcess(), (UINT)rc); }   // LRPS2 hangs when saving/unloading a VM that never ran a frame (failed setup check)
        finish();
    }
    if (tex) SDL_DestroyTexture(tex);
    if (osd_tex) SDL_DestroyTexture(osd_tex);
    if (fx_tex_) SDL_DestroyTexture(fx_tex_);
    if (bezel_tex_) SDL_DestroyTexture(bezel_tex_);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    audio.close();
    for (auto* p : pads) SDL_GameControllerClose(p);
    ra_.stop();
    session.stop();
    core.unload();
    SDL_Quit();
    return rc;
}

int main() {
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> v;
    for (int i = 1; i < argc; i++) v.push_back(narrow(wargv[i]));
    LocalFree(wargv);
    Args args;
    if (!parse_args(v, &args)) return 2;
    Frontend fe;
    return fe.run(args);
}
