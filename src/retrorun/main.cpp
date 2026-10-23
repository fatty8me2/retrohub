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
#include <string>
#include <vector>

#include "controls_sdl.h"
#include "core.h"
#include "ds_link.h"
#include "retro_pad.h"
#include "dsview.h"
#include "image.h"
#include "link_sync.h"
#include "rollback.h"
#include "lockstep.h"
#include "log.h"
#include "session.h"
#include "save_sync.h"

namespace fs = std::filesystem;

/* ------------------------------------------------------------------ args */

struct Args {
    std::string core, rom, save_dir, state_dir, system_dir, system, screenshot, log, ini;
    std::string scale_filter;   // --filter nearest|linear (default: nearest, DS linear)
    int integer_scale = -1;     // --integer-scale 0|1 (default: on, DS off)
    std::string controls;         // --controls FILE / RETROHUB_CONTROLS: resolved controller profile JSON (src/common/controls.h)
    std::string net, room, title, save_stem;   // save_stem: file stem for <stem>.srm (zipped ROMs); "" = rom file stem
    std::string sync_hash_mode = "auto", pace_override;
    bool share_host_save = false; // --share-host-save: lockstep session boots from the host's save (+ clock) on every PC
    bool link = false;            // two linked Game Boys in one SameBoy instance (see docs/RETRORUN.md)
    std::string link_roms;        // optional "romP0,romP1" (default: --rom for both)
    bool list_opts = false, headless = false, fullscreen = false, resume = false, fuzz = false, selftest = false;
    double stylus_u = -1, stylus_v = -1;   // headless DS test: hold the stylus down at this fraction of the frame
    int frames = -1, scale = 3, slot = 0, players = 1, delay = -1;   // delay -1 = auto
    int ports[4] = {0, -1, -1, -1};
    bool ports_given = false;
    bool rollback = false;        // --rollback: GGPO-style rollback netplay instead of lockstep (see rollback.h)
    int synctest = 0;             // --synctest N: offline, every frame roll back N frames and re-simulate, compare
    int rb_pred = 8;              // --rb-pred N: max predicted frames
    bool ds_link = false;   // DS local wireless over the room (netpacket interface), no lockstep
    std::vector<std::string> opts;
};

static std::string narrow(const wchar_t* w) { return winutil::to_utf8(w); }

static void usage() {
    fputs("usage: retrorun --core <x_libretro.dll> --rom <file> --save-dir <dir> --state-dir <dir>\n"
          "  [--system gba|gb|gbc|nds] [--system-dir DIR] [--scale 3] [--filter nearest|linear] [--integer-scale 0|1] [--fullscreen] [--resume] [--title TEXT]\n"
          "  [--opt key=value]... [--ini FILE] [--log FILE]\n"
          "  [--headless [--frames N] [--screenshot out.png|out.bmp]] [--stylus U,V] [--selftest] [--fuzz-input] [--sync-hash auto|state|output] [--rollback] [--rb-pred N] [--synctest N] [--pace vsync|audio|timer] [--list-options]\n"
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
        else if (k == "--save-dir") { if (!val(&o->save_dir)) return false; }
        else if (k == "--state-dir") { if (!val(&o->state_dir)) return false; }
        else if (k == "--system-dir") { if (!val(&o->system_dir)) return false; }
        else if (k == "--system") { if (!val(&o->system)) return false; }
        else if (k == "--screenshot") { if (!val(&o->screenshot)) return false; }
        else if (k == "--log") { if (!val(&o->log)) return false; }
        else if (k == "--ini") { if (!val(&o->ini)) return false; }
        else if (k == "--net") { if (!val(&o->net)) return false; }
        else if (k == "--room") { if (!val(&o->room)) return false; }
        else if (k == "--title") { if (!val(&o->title)) return false; }
        else if (k == "--save-stem") { if (!val(&o->save_stem)) return false; }
        else if (k == "--filter") { if (!val(&v)) return false; if (v != "nearest" && v != "linear") { fprintf(stderr, "bad --filter\n"); return false; } o->scale_filter = v; }
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
        else if (k == "--no-rollback") o->rollback = false;
        else if (k == "--synctest") { if (!val(&v)) return false; o->synctest = std::max(1, atoi(v.c_str())); }
        else if (k == "--rb-pred") { if (!val(&v)) return false; o->rb_pred = std::max(1, std::min(10, atoi(v.c_str()))); }
        else if (k == "--link") o->link = true;
        else if (k == "--share-host-save") o->share_host_save = true;
        else if (k == "--host-save-dir") { std::string ignored; if (!val(&ignored)) return false; }   // accepted for hub symmetry with n64run
        else if (k == "--link-roms") { if (!val(&o->link_roms)) return false; }
        else if (k == "--headless") o->headless = true;
        else if (k == "--fullscreen") o->fullscreen = true;
        else if (k == "--resume") o->resume = true;
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

    void push(const int16_t* in, size_t frames, bool drc) {
        if (!dev_) return;
        double ratio = 1.0;
        if (drc) {
            double err = ((double)target_ - (double)fill()) / (double)target_;
            ratio = 1.0 + std::max(-1.0, std::min(1.0, err)) * 0.005;
        }
        const double step = 1.0 / ratio;
        uint64_t w = wr_.load(std::memory_order_relaxed);
        const uint64_t r = rd_.load(std::memory_order_acquire);
        for (size_t i = 0; i < frames; i++) {
            const double cl = in[i * 2], cr = in[i * 2 + 1];
            while (phase_ < 1.0) {
                if (w - r >= kCap) { phase_ = 0; break; }   // full: drop
                size_t idx = (size_t)(w & (kCap - 1)) * 2;
                ring_[idx] = (int16_t)std::lround(prev_[0] + (cl - prev_[0]) * phase_);
                ring_[idx + 1] = (int16_t)std::lround(prev_[1] + (cr - prev_[1]) * phase_);
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
    bool fullscreen = false, quit = false, muted = false, online = false, ff = false;
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
    bool is_ds = false;        // Nintendo DS: two-screen frame, fractional+linear scaling, stylus via the mouse
    int ds_layout = 0;         // dsview::layout_name index (F9 cycles)
    SDL_Rect last_dst{0, 0, 0, 0};   // where the frame was last drawn (stylus mapping)
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
    std::unique_ptr<rb::Engine> rbe;   // rollback netplay (--rollback); replaces lockstep
    bool resim = false;                // re-simulating: drop audio
    uint64_t fr_audio = 0xCBF29CE484222325ull;   // hash of the audio of the frame being stepped
    rb::Sim make_rb_sim();
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
    bool link_active = false;
    uint16_t link_msg_id = 0;
    dslink::Reassembler link_reasm;
    uint64_t link_tx = 0, link_rx = 0, link_tx_cmd = 0, link_tx_reply = 0, link_rx_cmd = 0, link_rx_reply = 0, link_last_log = 0;
    std::map<int, uint64_t> link_rx_by_slot;
    bool link_probe = false;   // RETRORUN_LINK_PROBE=1: synthetic 2.3 KB frames, a transport check without a game in wireless mode
    uint64_t probe_tx = 0, probe_rx = 0, probe_bad = 0;
    void finish();
    void do_pacing();
    int selftest();

    uint32_t local_pad_cache_[4] = {0, 0, 0, 0};
};

static Frontend* g_fe = nullptr;

void Frontend::set_status(const std::string& s) {
    rlog("%s", s.c_str());
    status_msg = s;
    status_until = SDL_GetTicks64() + 2500;
    if (win) SDL_SetWindowTitle(win, (window_base() + "  |  " + s).c_str());
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
    scale_filter_ = a.scale_filter;
    if (a.integer_scale >= 0) integer_scale_ = a.integer_scale != 0;
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
    if (a.link) {
        // Deterministic link defaults (ini / --opt below may still override): cycle-based RTC, side-by-side layout, my console's audio.
        int mine = 0;
        for (int i = 0; i < 4; i++) if (owner[i] == a.slot) { mine = i; break; }
        core.overrides["sameboy_link"] = "enabled";
        core.overrides["sameboy_rtc"] = "accurate";
        core.overrides["sameboy_screen_layout"] = "left-right";
        core.overrides["sameboy_audio_output"] = "Game Boy #" + std::to_string(std::min(1, mine) + 1);
    }
    if (!a.net.empty() && !a.link && !is_ds) core.overrides["sameboy_rtc"] = "accurate";   // cycle-based clock: wall time would desync lockstep
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
    return true;
}

/* ------------------------------------------------------------------ video */

void Frontend::layout(SDL_Rect* dst) {
    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    const FrameBuf& f = core.frame();
    double aspect = core.av().geometry.aspect_ratio;
    if (aspect <= 0.f) aspect = (double)f.w / std::max(1, f.h);
    int fh = f.h;
    if (link) { SDL_Rect v = view_src(); aspect = (double)v.w / std::max(1, v.h); fh = v.h; }
    dsview::Rect r = dsview::fit(ow, oh, fh, aspect, !is_ds && integer_scale_);   // integer scale when it fits (not for DS)
    dst->x = r.x; dst->y = r.y; dst->w = r.w; dst->h = r.h;
}

void Frontend::present() {
    const FrameBuf& f = core.frame();
    if (!ren || f.px.empty()) return;
    if (!tex || tex_w != f.w || tex_h != f.h) {
        if (tex) SDL_DestroyTexture(tex);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, (scale_filter_.empty() ? is_ds : scale_filter_ == "linear") ? "linear" : "nearest");
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
    if (link) { SDL_Rect v = view_src(); SDL_RenderCopy(ren, tex, &v, &dst); }
    else SDL_RenderCopy(ren, tex, nullptr, &dst);
    SDL_RenderPresent(ren);
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
uint32_t Frontend::poll_local(int i) {
    int port = local_ports[i];
    if (a.fuzz) {
        uint32_t x = (uint32_t)(frame_no / 3) * 2654435761u ^ (uint32_t)(port + 1) * 40503u ^ 0x9E3779B9u;
        x ^= x >> 15; x *= 2246822519u; x ^= x >> 13;
        return x & 0xFF3u & ~0xCu;   // never Start/Select
    }
    if (a.headless) return 0;
    uint32_t m = 0;
    if (i == 0 && retro_pad) m |= retro_pad->key_mask(SDL_GetKeyboardState(nullptr));
    if ((size_t)i < pads.size()) {
        hubctl::PadState st;
        hubctl::read_state(pads[i], &st);
        m |= retro_pad->mask(st);
    }
    return m;
}

void Frontend::poll_all_local() {
    for (int i = 0; i < n_local; i++) local_pad_cache_[i] = poll_local(i);
    if (!online)
        for (int i = 0; i < n_local; i++) frame_pad[local_ports[i]] = local_pad_cache_[i];
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
    ff = !online && !a.ds_link && k[SDL_SCANCODE_TAB];
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
    set_status("Saved state " + name);
    return true;
}

bool Frontend::load_state(const std::string& name) {
    std::vector<uint8_t> buf;
    if (!read_file(state_path(name), &buf)) { set_status("No state " + name); return false; }
    if (!core.unserialize(buf.data(), buf.size())) { set_status("Load state failed (core error)"); return false; }
    set_status("Loaded state " + name);
    return true;
}

void Frontend::fold_frame() {
    const FrameBuf& f = core.frame();
    uint64_t h = f.px.empty() ? 0 : n64run::hash_block(f.px.data(), f.px.size() * 4);
    out_hash = (out_hash ^ h) * 0x100000001B3ull;
    for (int p = 0; p < 4; p++) in_hash = (in_hash ^ frame_pad[p]) * 0x100000001B3ull;
}

/* Hash used for the periodic desync check. */
uint64_t Frontend::sync_hash() { return use_state_hash ? state_hash() : out_hash; }

uint64_t Frontend::state_hash() {
    size_t n = core.serialize_size();
    if (!n) return 0;
    std::vector<uint8_t> buf(n);
    if (!core.serialize(buf.data(), n)) return 0;
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
    {
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
    if (a.share_host_save && !link && !is_ds && !share_host_save_run()) { net_rc = 6; return false; }
    if (link) {
        std::vector<std::vector<uint8_t>> blobs;
        std::string lerr;
        int mine = link_port_of_slot(a.slot);
        if (mine < 0) { rlog("link: this slot owns no console"); return false; }
        if (!rlink::run_link_exchange(session, a.slot, slot_mask, link_blob_from_disk(mine), 60000, &blobs, [this] { pump_events(false); }, &lerr)) {
            rlog("link: save exchange failed: %s", lerr.c_str());
            net_rc = 4;
            return false;
        }
        for (int i = 0; i < link_n; i++)
            if (owner[i] >= 0 && owner[i] < 8 && (slot_mask & (1u << owner[i]))) link_apply_blob(i, blobs[owner[i]]);
        link_loaded = true;
        rlog("link: saves exchanged and loaded for %d consoles", link_n);
    }
    if (a.rollback) {
        a.delay = a.delay >= 0 ? std::min(a.delay, 4) : 1;
        rb::Config rc;
        for (int i = 0; i < 4; i++) rc.owner[i] = owner[i];
        rc.my_slot = a.slot;
        rc.slot_mask = slot_mask;
        rc.delay = a.delay;
        rc.max_pred = a.rb_pred;
        rc.use_state_hash = use_state_hash;
        rb_warmup();
        rb::Transport t;
        t.pump = [this] { session.update(); };
        t.send = [this](int to, const void* d, int n) { session.send(to, d, n); };
        t.recv = [this](int* from, std::vector<uint8_t>* o) { return session.recv(from, o); };
        t.now_ms = [] { return hubnet::now_ms(); };
        t.log = [](const std::string& s) { rlog("%s", s.c_str()); };
        rbe = std::make_unique<rb::Engine>(rc, make_rb_sim(), t);
        rlog("net: ROLLBACK netplay, input delay %d, max prediction %d, state %zu bytes, sync hash %s", a.delay, rc.max_pred,
             core.serialize_size(), use_state_hash ? "state" : "output");
        if (!rbe->start_barrier(30000)) { rlog("net: start barrier failed"); net_rc = 4; return false; }
        rlog("net: all %d players ready, starting", a.players);
        return true;
    }
    {
        n64run::DelayNegotiation dn;
        dn.fixed_delay = a.delay;
        dn.my_slot = a.slot;
        dn.slot_mask = slot_mask;
        dn.frame_ms = 1000.0 / (fps > 1 ? fps : 60.0);
        int d = 2;
        if (!n64run::negotiate_delay(session, dn, [this] { pump_events(false); }, [](const std::string& m) { rlog("%s", m.c_str()); }, &d)) {
            rlog("net: input delay negotiation failed (%s)", session.status().c_str());
            net_rc = 4;
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
    o.hash_label = use_state_hash ? "state" : "output";
    o.delay = a.delay;
    o.online = true;
    o.hash_interval = 600;
    n64run::LockstepHooks h;
    h.read_local = [this](int i) { return local_pad_cache_[i]; };
    h.update_local = [this]() { pump_events(true); poll_all_local(); };
    h.idle = [this]() { pump_events(true); };
    h.hash_rdram = [this](uint64_t* out) {
        uint64_t v = sync_hash();
        if (!v) return false;
        *out = v;
        return true;
    };
    h.request_stop = [this]() { quit = true; };
    h.log = [](const std::string& s) { rlog("%s", s.c_str()); };
    lockstep = std::make_unique<n64run::Lockstep>(o, &session, h);
    if (!lockstep->start_barrier(30000)) { rlog("net: start barrier failed"); net_rc = 4; return false; }
    rlog("net: all %d players ready, starting", a.players);
    return true;
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
    if (core.netpacket().connected && a.slot == 0)
        for (int s = 1; s < std::max(1, a.players); s++) core.netpacket().connected((uint16_t)s);
    rlog("link: wireless started as client id %d", a.slot);
    return true;
}

/* ------------------------------------------------------------------ main loop */

void Frontend::do_pacing() {
    if (ff || (a.headless && !a.ds_link && !rbe)) return;
    switch (pace) {
        case Pace::Audio:
            while (audio.fill() > audio.target() && !quit) {
                SDL_Delay(1);
                pump_events(false);
            }
            break;
        case Pace::Timer: {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            int64_t period = (int64_t)((double)qpc_freq / fps);
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

void Frontend::finish() {
    static bool done = false;
    if (done) return;
    done = true;
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
    s.step = [this](const uint32_t pads[4], bool visible) {
        resim = !visible;
        fr_audio = 0xCBF29CE484222325ull;
        for (int p = 0; p < 4; p++) frame_pad[p] = pads[p];
        core.run();
        resim = false;
    };
    s.frame_sum = [this] {
        const FrameBuf& f = core.frame();
        uint64_t h = f.px.empty() ? 0 : n64run::hash_block(f.px.data(), f.px.size() * 4);
        return h;   // video only: audio resampler state is not part of any core's savestate, and audio never feeds back into the game
    };
    s.poll_local = [this] { poll_all_local(); };
    s.read_local = [this](int port) -> uint32_t {
        for (int i = 0; i < n_local; i++) if (local_ports[i] == port) return local_pad_cache_[i];
        return 0;
    };
    s.idle = [this] { pump_events(true); };
    return s;
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
    rlog("rollback-stats: input-delay %d frames, frames %llu, rollbacks %llu (%.2f/s of game time), resim frames %llu, max depth %d, avg depth %.2f, stalls %llu (%llu ms), sync-waits %llu, max-skew %.2f, hashes compared %llu",
         rbe->delay(), (unsigned long long)s.frames, (unsigned long long)s.rollbacks, s.rollbacks / secs_f, (unsigned long long)s.resim_frames, s.max_depth,
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
    auto input = [&](int f, uint32_t pads[4]) {
        for (int i = 0; i < 4; i++) pads[i] = 0;
        for (int k = 0; k < nports; k++) {
            uint32_t x = (uint32_t)(f / 2) * 2654435761u ^ (uint32_t)(ports[k] + 1) * 40503u ^ (uint32_t)seed;
            x ^= x >> 15; x *= 2246822519u; x ^= x >> 13; x *= 3266489917u; x ^= x >> 16;
            pads[ports[k]] = std::getenv("RB_NOINPUT") ? 0 : (x & 0xFF3u & ~0xCu);
        }
    };
    rlog("SYNCTEST: %d frames, rollback depth %d, state %zu bytes, compare %s", frames, a.synctest, core.serialize_size(),
         use_state_hash ? "output + state hash" : "output (video+audio) only");
    uint64_t t0 = SDL_GetTicks64();
    rb::SyncTestResult r = rb::run_synctest(sim, frames, a.synctest, use_state_hash, input, [](const std::string& m) { rlog("%s", m.c_str()); });
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
        if ((dev & RETRO_DEVICE_MASK) != RETRO_DEVICE_JOYPAD || port >= 4 || idx != 0) return 0;
        uint32_t m = frame_pad[port];
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)(m & 0xFFFF);
        return id < 16 ? (int16_t)((m >> id) & 1) : 0;
    };
    bool drc = false;
    core.on_audio = [this, &drc](const int16_t* d, size_t n) {
        if (rbe || a.synctest) { if (!link) for (size_t i = 0; i < n * 2; i++) fr_audio = (fr_audio ^ (uint16_t)d[i]) * 0x100000001B3ull; }
        else if (!link) for (size_t i = 0; i < n * 2; i++) out_hash = (out_hash ^ (uint16_t)d[i]) * 0x100000001B3ull;
        if (!ff && !resim && audio.active()) audio.push(d, n, drc);
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
    } else if (!core.load_game(a.rom, &err)) { rlog("ERROR: %s", err.c_str()); return 1; }
    fps = core.av().timing.fps > 1 ? core.av().timing.fps : 60.0;
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
        use_state_hash = a.sync_hash_mode == "state" || (a.sync_hash_mode == "auto" && ln.find("gambatte") == std::string::npos);
        if (use_state_hash && !core.serialize_size()) use_state_hash = false;
        rlog("sync hash: %s", use_state_hash ? "serialized state" : "video+audio output");
    }
    if (!a.headless && !muted) audio.open(core.av().timing.sample_rate > 1000 ? core.av().timing.sample_rate : 32768.0);
    if (!setup_video()) return 1;
    drc = pace == Pace::Vsync;
    if (!a.headless && pace == Pace::None) pace = audio.active() ? Pace::Audio : Pace::Timer;
    {
        std::string cf = a.controls;
        if (cf.empty()) { const char* e = std::getenv("RETROHUB_CONTROLS"); if (e && *e) cf = e; }
        if (!cf.empty()) {
            hubctl::Profile p;
            if (hubctl::read_profile_file(cf, &p) && p.system == "retro") { prof = p; rlog("controls: profile '%s' (%s)", p.name.c_str(), cf.c_str()); }
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
    if (!link && a.resume && !online && a.net.empty() && !a.ds_link) {
        if (fs::exists(fs::u8path(state_path("auto")))) load_state("auto");
    }

    int rc = 0;
    if (a.selftest) {
        rc = selftest();
    } else if (a.synctest) {
        rc = run_synctest();
    } else if (rbe || (a.rollback && !a.net.empty())) {
        if (!rbe && !setup_net()) { rc = net_rc; goto done; }
        if (a.headless) pace = Pace::Timer;
        {
            uint64_t t_start = SDL_GetTicks64(), last_stat = t_start;
            uint64_t last_rb = 0;
            for (;;) {
                if (quit) break;
                if (a.frames >= 0 && rbe->cur() >= (uint32_t)a.frames) break;
                pump_events(false);
                if (quit) break;
                if (session.failed()) { rlog("net: connection lost, leaving"); rc = 4; break; }
                rb::Engine::Status st = rbe->prepare();
                if (rbe->connection_lost()) { rlog("net: connection lost, leaving"); rc = 4; break; }
                if (rbe->desync()) { rc = 5; break; }
                if (st == rb::Engine::Status::Stalled) { Sleep(1); continue; }
                if (st == rb::Engine::Status::SyncWait) { Sleep((DWORD)(1000.0 / (fps > 1 ? fps : 60.0))); continue; }
                do_pacing();
                rbe->run_frame();
                frame_no = rbe->cur();
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
        {
            uint64_t t_start = SDL_GetTicks64();
            for (;;) {
                if (quit) break;
                if (a.frames >= 0 && frame_no >= (uint64_t)a.frames) break;
                pump_events(false);
                if (quit) break;
                if (a.ds_link && session.ended()) {   // a partner signed off on purpose: not a failure
                    rlog("link: partner ended the session");
                    link_stop("partner ended the session");
                    break;
                }

                if (want_save) { want_save = false; save_state("slot" + std::to_string(slot_sel)); }
                if (want_load) { want_load = false; if (!online && !a.ds_link) load_state("slot" + std::to_string(slot_sel)); }

                if (online) {
                    for (int p = 0; p < 4; p++) frame_pad[p] = lockstep->get_keys(p);   // lead port poll advances the sample
                    if (lockstep->connection_lost()) { rlog("net: connection lost, leaving"); rc = 4; break; }
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
                do_pacing();
                core.run();
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
                frame_no++;

                if (!a.headless && (!ff || (frame_no & 7) == 0)) present();

                if ((frame_no & 63) == 0 && SDL_GetTicks64() - last_srm_check > 30000) {
                    last_srm_check = SDL_GetTicks64();
                    if (link) link_save(false); else save_srm(false);
                }
            }
            double secs = (SDL_GetTicks64() - t_start) / 1000.0;
            if (online && lockstep) lockstep->log_stall_summary();
            rlog("done: %llu frames in %.2f s (%.1f fps)", (unsigned long long)frame_no, secs, secs > 0 ? frame_no / secs : 0.0);
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
        }
    }
done:
    {
        uint64_t h = core.game_loaded() ? state_hash() : 0;
        if (!a.net.empty() || a.selftest || a.headless)
            rlog("final: frame %llu state-hash %016llx output-hash %016llx input-hash %016llx", (unsigned long long)frame_no,
                 (unsigned long long)h, (unsigned long long)out_hash, (unsigned long long)in_hash);
        if (!a.screenshot.empty()) {
            if (write_frame_image(a.screenshot, core.frame()))
                rlog("screenshot: %s (%dx%d)", a.screenshot.c_str(), core.frame().w, core.frame().h);
            else
                rlog("screenshot: FAILED to write %s", a.screenshot.c_str());
        }
        finish();
    }
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    audio.close();
    for (auto* p : pads) SDL_GameControllerClose(p);
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
