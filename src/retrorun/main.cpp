/* retrorun: minimal libretro frontend for RetroHub (GBA / GB / GBC).
 * See docs/RETRORUN.md. */

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
#include <memory>
#include <string>
#include <vector>

#include "core.h"
#include "image.h"
#include "lockstep.h"
#include "log.h"
#include "session.h"

namespace fs = std::filesystem;

/* ------------------------------------------------------------------ args */

struct Args {
    std::string core, rom, save_dir, state_dir, system_dir, system, screenshot, log, ini;
    std::string net, room, title;
    std::string sync_hash_mode = "auto", pace_override;
    bool list_opts = false, headless = false, fullscreen = false, resume = false, fuzz = false, selftest = false;
    int frames = -1, scale = 3, slot = 0, players = 1, delay = -1;   // delay -1 = auto
    int ports[4] = {0, -1, -1, -1};
    bool ports_given = false;
    std::vector<std::string> opts;
};

static std::string narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

static void usage() {
    fputs("usage: retrorun --core <x_libretro.dll> --rom <file> --save-dir <dir> --state-dir <dir>\n"
          "  [--system gba|gb|gbc] [--system-dir DIR] [--scale 3] [--fullscreen] [--resume] [--title TEXT]\n"
          "  [--opt key=value]... [--ini FILE] [--log FILE]\n"
          "  [--headless [--frames N] [--screenshot out.png|out.bmp]] [--selftest] [--fuzz-input] [--sync-hash auto|state|output] [--pace vsync|audio|timer] [--list-options]\n"
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
        else if (k == "--headless") o->headless = true;
        else if (k == "--fullscreen") o->fullscreen = true;
        else if (k == "--resume") o->resume = true;
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
    Pace pace = Pace::None;
    uint64_t frame_no = 0;
    uint64_t out_hash = 0xCBF29CE484222325ull, in_hash = 0xCBF29CE484222325ull;   // rolling hash of video+audio output / inputs
    bool use_state_hash = true;
    void fold_frame();
    uint64_t sync_hash();
    std::vector<SDL_GameController*> pads;

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
    uint32_t slot_mask = 1;
    double fps = 60.0;
    int64_t qpc_freq = 1, next_deadline = 0;

    int run(const Args& args);

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
    void save_srm(bool force);
    std::string state_path(const std::string& name) const { return a.state_dir + "\\" + name + ".state"; }
    bool save_state(const std::string& name);
    bool load_state(const std::string& name);
    uint64_t state_hash();
    bool setup_net();
    int net_rc = 3;
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
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        a.system_dir = narrow((fs::path(exe).parent_path() / L"system").wstring().c_str());
    }
    fs::create_directories(fs::u8path(a.system_dir), ec);
    core.system_dir = a.system_dir;
    core.save_dir = a.save_dir;
    core.content_dir = narrow(fs::u8path(a.rom).parent_path().wstring().c_str());
    core.system_tag = a.system;
    rom_stem = fs::u8path(a.rom).stem().u8string();
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
    double ks = std::min(ow / (f.h * aspect), (double)oh / f.h);
    double k = ks >= 1.0 ? std::floor(ks) : ks;   // integer scale when it fits
    dst->h = (int)std::lround(f.h * k);
    dst->w = (int)std::lround(f.h * k * aspect);
    dst->x = (ow - dst->w) / 2;
    dst->y = (oh - dst->h) / 2;
}

void Frontend::present() {
    const FrameBuf& f = core.frame();
    if (!ren || f.px.empty()) return;
    if (!tex || tex_w != f.w || tex_h != f.h) {
        if (tex) SDL_DestroyTexture(tex);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
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
    SDL_RenderCopy(ren, tex, nullptr, &dst);
    SDL_RenderPresent(ren);
}

bool Frontend::setup_video() {
    if (a.headless) return true;
    const retro_game_geometry& g = core.av().geometry;
    double aspect = g.aspect_ratio > 0 ? g.aspect_ratio : (double)g.base_width / g.base_height;
    int w = (int)std::lround(g.base_height * a.scale * aspect), h = (int)(g.base_height * a.scale);
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
    SDL_ShowCursor(fullscreen ? SDL_DISABLE : SDL_ENABLE);
}

/* ------------------------------------------------------------------ input */

void Frontend::open_pads() {
    for (auto* p : pads) SDL_GameControllerClose(p);
    pads.clear();
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i))
            if (auto* c = SDL_GameControllerOpen(i)) {
                rlog("controller %zu: %s", pads.size(), SDL_GameControllerName(c));
                pads.push_back(c);
            }
}

static uint32_t kbd_mask() {
    const Uint8* k = SDL_GetKeyboardState(nullptr);
    uint32_t m = 0;
    auto b = [&](SDL_Scancode s, int id) { if (k[s]) m |= 1u << id; };
    b(SDL_SCANCODE_UP, RETRO_DEVICE_ID_JOYPAD_UP);
    b(SDL_SCANCODE_DOWN, RETRO_DEVICE_ID_JOYPAD_DOWN);
    b(SDL_SCANCODE_LEFT, RETRO_DEVICE_ID_JOYPAD_LEFT);
    b(SDL_SCANCODE_RIGHT, RETRO_DEVICE_ID_JOYPAD_RIGHT);
    b(SDL_SCANCODE_Z, RETRO_DEVICE_ID_JOYPAD_B);
    b(SDL_SCANCODE_X, RETRO_DEVICE_ID_JOYPAD_A);
    b(SDL_SCANCODE_A, RETRO_DEVICE_ID_JOYPAD_Y);
    b(SDL_SCANCODE_S, RETRO_DEVICE_ID_JOYPAD_X);
    b(SDL_SCANCODE_Q, RETRO_DEVICE_ID_JOYPAD_L);
    b(SDL_SCANCODE_W, RETRO_DEVICE_ID_JOYPAD_R);
    b(SDL_SCANCODE_RETURN, RETRO_DEVICE_ID_JOYPAD_START);
    b(SDL_SCANCODE_KP_ENTER, RETRO_DEVICE_ID_JOYPAD_START);
    b(SDL_SCANCODE_RSHIFT, RETRO_DEVICE_ID_JOYPAD_SELECT);
    return m;
}

static uint32_t pad_mask(SDL_GameController* c) {
    uint32_t m = 0;
    auto b = [&](SDL_GameControllerButton s, int id) { if (SDL_GameControllerGetButton(c, s)) m |= 1u << id; };
    // RetroPad B is the bottom face button (SDL A), RetroPad A the right one (SDL B), etc.
    b(SDL_CONTROLLER_BUTTON_A, RETRO_DEVICE_ID_JOYPAD_B);
    b(SDL_CONTROLLER_BUTTON_B, RETRO_DEVICE_ID_JOYPAD_A);
    b(SDL_CONTROLLER_BUTTON_X, RETRO_DEVICE_ID_JOYPAD_Y);
    b(SDL_CONTROLLER_BUTTON_Y, RETRO_DEVICE_ID_JOYPAD_X);
    b(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, RETRO_DEVICE_ID_JOYPAD_L);
    b(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, RETRO_DEVICE_ID_JOYPAD_R);
    b(SDL_CONTROLLER_BUTTON_START, RETRO_DEVICE_ID_JOYPAD_START);
    b(SDL_CONTROLLER_BUTTON_BACK, RETRO_DEVICE_ID_JOYPAD_SELECT);
    b(SDL_CONTROLLER_BUTTON_LEFTSTICK, RETRO_DEVICE_ID_JOYPAD_L3);
    b(SDL_CONTROLLER_BUTTON_RIGHTSTICK, RETRO_DEVICE_ID_JOYPAD_R3);
    b(SDL_CONTROLLER_BUTTON_DPAD_UP, RETRO_DEVICE_ID_JOYPAD_UP);
    b(SDL_CONTROLLER_BUTTON_DPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_DOWN);
    b(SDL_CONTROLLER_BUTTON_DPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_LEFT);
    b(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_RIGHT);
    int lx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX), ly = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
    const int dz = 16000;
    if (lx < -dz) m |= 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
    if (lx > dz) m |= 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
    if (ly < -dz) m |= 1u << RETRO_DEVICE_ID_JOYPAD_UP;
    if (ly > dz) m |= 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;
    if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) m |= 1u << RETRO_DEVICE_ID_JOYPAD_L2;
    if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) m |= 1u << RETRO_DEVICE_ID_JOYPAD_R2;
    return m;
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
    if (i == 0) m |= kbd_mask();
    if ((size_t)i < pads.size()) m |= pad_mask(pads[i]);
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
        case SDLK_F5: want_save = true; break;
        case SDLK_F7:
            if (online) set_status("Loading states is disabled during netplay");
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
    ff = !online && k[SDL_SCANCODE_TAB];
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
    } else {
        rlog("save: no existing %s (battery RAM %zu bytes)", srm_path.c_str(), n);
    }
    srm_saved.assign((uint8_t*)mem, (uint8_t*)mem + n);
}

void Frontend::save_srm(bool force) {
    size_t n = 0;
    void* mem = core.save_ram(&n);
    if (!mem || !n) return;
    if (!force && srm_saved.size() == n && memcmp(srm_saved.data(), mem, n) == 0) return;
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

/* ------------------------------------------------------------------ main loop */

void Frontend::do_pacing() {
    if (ff || a.headless) return;
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
    if (core.game_loaded()) {
        save_srm(false);
        if (!a.selftest && core.serialize_size()) {
            size_t n = core.serialize_size();
            std::vector<uint8_t> buf(n);
            if (core.serialize(buf.data(), n) && write_atomic(state_path("auto"), buf.data(), n))
                rlog("state: wrote auto.state (%zu bytes)", n);
        }
    }
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
    if (a.net.empty() && !a.ports_given)
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
        if ((dev & RETRO_DEVICE_MASK) != RETRO_DEVICE_JOYPAD || port >= 4 || idx != 0) return 0;
        uint32_t m = frame_pad[port];
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)(m & 0xFFFF);
        return id < 16 ? (int16_t)((m >> id) & 1) : 0;
    };
    bool drc = false;
    core.on_audio = [this, &drc](const int16_t* d, size_t n) {
        for (size_t i = 0; i < n * 2; i++) out_hash = (out_hash ^ (uint16_t)d[i]) * 0x100000001B3ull;
        if (!ff && audio.active()) audio.push(d, n, drc);
    };

    std::string err;
    if (!core.load_core(a.core, &err)) { rlog("ERROR: %s", err.c_str()); return 1; }
    if (!core.load_game(a.rom, &err)) { rlog("ERROR: %s", err.c_str()); return 1; }
    fps = core.av().timing.fps > 1 ? core.av().timing.fps : 60.0;
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
    if (!a.headless) open_pads();
    core.on_geometry = [this]() { fps = core.av().timing.fps > 1 ? core.av().timing.fps : fps; };

    load_srm();
    if (a.resume && !online && a.net.empty()) {
        if (fs::exists(fs::u8path(state_path("auto")))) load_state("auto");
    }

    int rc = 0;
    if (a.selftest) {
        rc = selftest();
    } else {
        if (!setup_net()) { rc = net_rc; goto done; }
        {
            uint64_t t_start = SDL_GetTicks64();
            for (;;) {
                if (quit) break;
                if (a.frames >= 0 && frame_no >= (uint64_t)a.frames) break;
                pump_events(false);
                if (quit) break;

                if (want_save) { want_save = false; save_state("slot" + std::to_string(slot_sel)); }
                if (want_load) { want_load = false; if (!online) load_state("slot" + std::to_string(slot_sel)); }

                if (online) {
                    for (int p = 0; p < 4; p++) frame_pad[p] = lockstep->get_keys(p);   // lead port poll advances the sample
                    if (lockstep->connection_lost()) { rlog("net: connection lost, leaving"); rc = 4; break; }
                } else {
                    poll_all_local();
                }

                do_pacing();
                core.run();
                fold_frame();
                frame_no++;

                if (!a.headless && (!ff || (frame_no & 7) == 0)) present();

                if ((frame_no & 63) == 0 && SDL_GetTicks64() - last_srm_check > 30000) {
                    last_srm_check = SDL_GetTicks64();
                    save_srm(false);
                }
            }
            double secs = (SDL_GetTicks64() - t_start) / 1000.0;
            if (online && lockstep) lockstep->log_stall_summary();
            rlog("done: %llu frames in %.2f s (%.1f fps)", (unsigned long long)frame_no, secs, secs > 0 ? frame_no / secs : 0.0);
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
