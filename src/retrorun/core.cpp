#include "core.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

/* ---------------------------------------------------------------- logging */

static FILE* g_logfile = nullptr;

void rlog_set_file(const std::string& path) {
    if (g_logfile) { fclose(g_logfile); g_logfile = nullptr; }
    if (!path.empty()) g_logfile = _wfopen(fs::u8path(path).c_str(), L"a");
}

static void rlog_v(const char* fmt, va_list ap) {
    char buf[2048];
    vsnprintf(buf, sizeof buf, fmt, ap);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    fprintf(stdout, "[retrorun] %s\n", buf);
    fflush(stdout);
    if (g_logfile) { fprintf(g_logfile, "%s\n", buf); fflush(g_logfile); }
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

void rlog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    rlog_v(fmt, ap);
    va_end(ap);
}

static void RETRO_CALLCONV core_log(enum retro_log_level level, const char* fmt, ...) {
    static const bool debug = getenv("RETRORUN_DEBUG") != nullptr;
    if (level == RETRO_LOG_DEBUG && !debug) return;
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    static const char* names[] = {"debug", "info", "warn", "error"};
    rlog("core %s: %s", level >= 0 && level < 4 ? names[level] : "?", buf);
}

/* ------------------------------------------------------------ trampolines */

static Core* g_core = nullptr;
Core* active_core() { return g_core; }

static bool RETRO_CALLCONV t_env(unsigned cmd, void* data) { return g_core && g_core->environment(cmd, data); }
static void RETRO_CALLCONV t_video(const void* d, unsigned w, unsigned h, size_t p) { if (g_core) g_core->video(d, w, h, p); }
static void RETRO_CALLCONV t_audio(int16_t l, int16_t r) { if (g_core) g_core->audio_one(l, r); }
static size_t RETRO_CALLCONV t_audio_batch(const int16_t* d, size_t n) { if (g_core) g_core->audio_batch(d, n); return n; }
static void RETRO_CALLCONV t_poll() { if (g_core) g_core->poll(); }
static int16_t RETRO_CALLCONV t_input(unsigned p, unsigned d, unsigned i, unsigned id) {
    return g_core ? g_core->input(p, d, i, id) : 0;
}

/* ------------------------------------------------------------------- core */

Core::Core() { g_core = this; }
Core::~Core() { unload(); if (g_core == this) g_core = nullptr; }

template <class T> static bool sym(HMODULE h, const char* name, T* out) {
    *out = reinterpret_cast<T>(GetProcAddress(h, name));
    return *out != nullptr;
}

bool Core::load_core(const std::string& dll_path, std::string* err) {
    core_path_ = dll_path;
    std::wstring w = fs::u8path(dll_path).wstring();
    // Let the core find its sibling DLLs.
    dll_ = LoadLibraryExW(w.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll_) { *err = "LoadLibrary failed for " + dll_path + " (error " + std::to_string(GetLastError()) + ")"; return false; }
    bool ok = true;
#define S(n, f) ok &= sym(dll_, n, &f)
    S("retro_set_environment", fn_set_env_);
    S("retro_set_video_refresh", fn_set_video_);
    S("retro_set_audio_sample", fn_set_audio_);
    S("retro_set_audio_sample_batch", fn_set_audio_batch_);
    S("retro_set_input_poll", fn_set_poll_);
    S("retro_set_input_state", fn_set_input_);
    S("retro_init", fn_init_);
    S("retro_deinit", fn_deinit_);
    S("retro_api_version", fn_api_version_);
    S("retro_get_system_info", fn_get_info_);
    S("retro_get_system_av_info", fn_get_av_);
    S("retro_set_controller_port_device", fn_set_port_dev_);
    S("retro_reset", fn_reset_);
    S("retro_run", fn_run_);
    S("retro_serialize_size", fn_serialize_size_);
    S("retro_serialize", fn_serialize_);
    S("retro_unserialize", fn_unserialize_);
    S("retro_load_game", fn_load_game_);
    S("retro_unload_game", fn_unload_game_);
    S("retro_get_memory_data", fn_get_mem_data_);
    S("retro_get_memory_size", fn_get_mem_size_);
#undef S
    sym(dll_, "retro_load_game_special", &fn_load_game_special_);   // optional
    sym(dll_, "retro_cheat_reset", &fn_cheat_reset_);               // optional
    sym(dll_, "retro_cheat_set", &fn_cheat_set_);                   // optional
    if (!ok) { *err = "not a libretro core (missing retro_* exports): " + dll_path; return false; }
    if (fn_api_version_() != RETRO_API_VERSION) {
        *err = "libretro API version mismatch: core reports " + std::to_string(fn_api_version_());
        return false;
    }
    fn_get_info_(&sysinfo_);
    rlog("core: %s %s (need_fullpath=%d, block_extract=%d, exts=%s)", sysinfo_.library_name ? sysinfo_.library_name : "?",
         sysinfo_.library_version ? sysinfo_.library_version : "?", (int)sysinfo_.need_fullpath,
         (int)sysinfo_.block_extract, sysinfo_.valid_extensions ? sysinfo_.valid_extensions : "");
    fn_set_env_(t_env);
    fn_set_video_(t_video);
    fn_set_audio_(t_audio);
    fn_set_audio_batch_(t_audio_batch);
    fn_set_poll_(t_poll);
    fn_set_input_(t_input);
    fn_init_();
    inited_ = true;
    return true;
}

bool Core::load_game(const std::string& rom_path, std::string* err) {
    game_path_ = rom_path;
    retro_game_info gi{};
    gi.path = game_path_.c_str();
    if (!sysinfo_.need_fullpath) {
        std::ifstream f(fs::u8path(rom_path), std::ios::binary);
        if (!f) { *err = "cannot read ROM " + rom_path; return false; }
        game_data_.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        gi.data = game_data_.data();
        gi.size = game_data_.size();
    } else if (!fs::exists(fs::u8path(rom_path))) {
        *err = "ROM not found: " + rom_path;
        return false;
    }
    if (!fn_load_game_(&gi)) { *err = "retro_load_game failed for " + rom_path; return false; }
    loaded_ = true;
    fn_get_av_(&av_);
    rlog("av: %ux%u (max %ux%u) aspect %.4f, %.4f fps, %.1f Hz audio", av_.geometry.base_width, av_.geometry.base_height,
         av_.geometry.max_width, av_.geometry.max_height, (double)av_.geometry.aspect_ratio, av_.timing.fps,
         av_.timing.sample_rate);
    return true;
}

bool Core::load_game_special(unsigned game_type, const std::vector<std::string>& rom_paths, std::string* err) {
    if (!fn_load_game_special_) { *err = "core has no retro_load_game_special"; return false; }
    std::vector<std::string> paths = rom_paths;
    std::vector<retro_game_info> gi(paths.size());
    game_datas_.assign(paths.size(), {});
    for (size_t i = 0; i < paths.size(); i++) {
        gi[i] = retro_game_info{};
        gi[i].path = paths[i].c_str();
        if (!sysinfo_.need_fullpath) {
            std::ifstream f(fs::u8path(paths[i]), std::ios::binary);
            if (!f) { *err = "cannot read ROM " + paths[i]; return false; }
            game_datas_[i].assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            gi[i].data = game_datas_[i].data();
            gi[i].size = game_datas_[i].size();
        } else if (!fs::exists(fs::u8path(paths[i]))) {
            *err = "ROM not found: " + paths[i];
            return false;
        }
    }
    game_path_ = paths.empty() ? "" : paths[0];
    if (!fn_load_game_special_(game_type, gi.data(), gi.size())) { *err = "retro_load_game_special failed"; return false; }
    loaded_ = true;
    fn_get_av_(&av_);
    rlog("av: %ux%u (max %ux%u) aspect %.4f, %.4f fps, %.1f Hz audio (special game type 0x%x, %zu contents)", av_.geometry.base_width,
         av_.geometry.base_height, av_.geometry.max_width, av_.geometry.max_height, (double)av_.geometry.aspect_ratio, av_.timing.fps,
         av_.timing.sample_rate, game_type, paths.size());
    return true;
}

void* Core::memory(unsigned id, size_t* size) {
    *size = fn_get_mem_size_(id);
    return *size ? fn_get_mem_data_(id) : nullptr;
}

void Core::unload() {
    if (loaded_) { fn_unload_game_(); loaded_ = false; }
    if (inited_) { fn_deinit_(); inited_ = false; }
    if (dll_) { FreeLibrary(dll_); dll_ = nullptr; }
}

void* Core::save_ram(size_t* size) {
    *size = fn_get_mem_size_(RETRO_MEMORY_SAVE_RAM);
    return *size ? fn_get_mem_data_(RETRO_MEMORY_SAVE_RAM) : nullptr;
}

/* ---------------------------------------------------------------- options */

void Core::add_option(const char* key, const char* desc, const char* def, std::vector<std::string> vals) {
    if (!key) return;
    CoreOption& o = opts_[key];
    o.key = key;
    o.desc = desc ? desc : "";
    o.values = std::move(vals);
    o.def = def ? def : (o.values.empty() ? "" : o.values[0]);
}

void Core::parse_v1_defs(const retro_core_option_definition* d) {
    for (; d && d->key; d++) {
        std::vector<std::string> v;
        for (int i = 0; i < RETRO_NUM_CORE_OPTION_VALUES_MAX && d->values[i].value; i++) v.push_back(d->values[i].value);
        add_option(d->key, d->desc, d->default_value, v);
    }
}

void Core::parse_v2_defs(const retro_core_option_v2_definition* d) {
    for (; d && d->key; d++) {
        std::vector<std::string> v;
        for (int i = 0; i < RETRO_NUM_CORE_OPTION_VALUES_MAX && d->values[i].value; i++) v.push_back(d->values[i].value);
        add_option(d->key, d->desc, d->default_value, v);
    }
}

/* ------------------------------------------------------------ environment */

static bool rumble_cb(unsigned port, enum retro_rumble_effect effect, uint16_t strength) {
    if (!g_core || !g_core->on_rumble) return false;
    g_core->on_rumble(port, effect == RETRO_RUMBLE_STRONG, strength);
    return true;
}

bool Core::environment(unsigned cmd, void* data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
            if (!data || !on_rumble) return false;
            ((struct retro_rumble_interface*)data)->set_rumble_state = &rumble_cb;
            return true;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
            *(const char**)data = system_dir.c_str();
            return true;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *(const char**)data = save_dir.c_str();
            return true;
        case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:   // == GET_CONTENT_DIRECTORY
            *(const char**)data = content_dir.c_str();
            return true;
        case RETRO_ENVIRONMENT_GET_LIBRETRO_PATH:
            *(const char**)data = core_path_.c_str();
            return true;
        case RETRO_ENVIRONMENT_GET_USERNAME:
            *(const char**)data = "player";
            return true;
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
            unsigned f = *(const unsigned*)data;
            if (f == RETRO_PIXEL_FORMAT_0RGB1555 || f == RETRO_PIXEL_FORMAT_XRGB8888 || f == RETRO_PIXEL_FORMAT_RGB565) {
                pixfmt_ = f;
                return true;
            }
            rlog("rejected pixel format %u", f);
            return false;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            auto* v = (retro_variable*)data;
            if (!v || !v->key) return false;
            auto it = opts_.find(v->key);
            auto ov = overrides.find(v->key);
            if (ov != overrides.end()) { v->value = ov->second.c_str(); return true; }
            if (it == opts_.end()) { v->value = nullptr; return false; }
            v->value = it->second.def.c_str();
            return true;
        }
        case RETRO_ENVIRONMENT_SET_VARIABLES: {
            for (auto* v = (const retro_variable*)data; v && v->key; v++) {
                std::string val = v->value ? v->value : "";
                std::string desc;
                std::vector<std::string> vals;
                size_t semi = val.find("; ");
                if (semi != std::string::npos) {
                    desc = val.substr(0, semi);
                    std::string rest = val.substr(semi + 2);
                    size_t p = 0;
                    while (p <= rest.size()) {
                        size_t q = rest.find('|', p);
                        if (q == std::string::npos) q = rest.size();
                        if (q > p) vals.push_back(rest.substr(p, q - p));
                        p = q + 1;
                    }
                }
                add_option(v->key, desc.c_str(), nullptr, vals);
            }
            vars_dirty_ = true;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            *(bool*)data = vars_dirty_;
            vars_dirty_ = false;
            return true;
        case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
            *(unsigned*)data = 2;
            return true;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
            parse_v1_defs((const retro_core_option_definition*)data);
            vars_dirty_ = true;
            return true;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
            if (data) parse_v1_defs(((const retro_core_options_intl*)data)->us);
            vars_dirty_ = true;
            return true;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
            if (data) parse_v2_defs(((const retro_core_options_v2*)data)->definitions);
            vars_dirty_ = true;
            return true;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
            if (data && ((const retro_core_options_v2_intl*)data)->us)
                parse_v2_defs(((const retro_core_options_v2_intl*)data)->us->definitions);
            vars_dirty_ = true;
            return true;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
            return true;
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            ((retro_log_callback*)data)->log = core_log;
            return true;
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
        case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
        case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
        case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY:
        case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
        case RETRO_ENVIRONMENT_SET_ROTATION:
            return true;
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *(bool*)data = true;
            return true;
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
            return true;
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
            return true;   // we never start without content; the flag is simply ignored
        case RETRO_ENVIRONMENT_SET_GEOMETRY:
            av_.geometry = *(const retro_game_geometry*)data;
            if (on_geometry) on_geometry();
            return true;
        case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
            av_ = *(const retro_system_av_info*)data;
            if (on_geometry) on_geometry();
            return true;
        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
            if (data) *(bool*)data = true;
            return true;
        case RETRO_ENVIRONMENT_GET_LANGUAGE:
            *(unsigned*)data = RETRO_LANGUAGE_ENGLISH;
            return true;
        case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
            *(int*)data = 3;   // video + audio on
            return true;
        case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
            *(bool*)data = ff_;
            return true;
        case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
            *(unsigned*)data = 0;
            return true;
        case RETRO_ENVIRONMENT_SET_MESSAGE:
            if (data && ((const retro_message*)data)->msg) rlog("core message: %s", ((const retro_message*)data)->msg);
            return true;
        case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
            if (data && ((const retro_message_ext*)data)->msg) rlog("core message: %s", ((const retro_message_ext*)data)->msg);
            return true;
        case RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE:
            if (data) np_ = *(const retro_netpacket_callback*)data;
            rlog("core registered a netpacket interface (protocol %s)", np_.protocol_version ? np_.protocol_version : "-");
            return true;
        case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO: {
            subsystems_.clear();
            for (auto* si = (const retro_subsystem_info*)data; si && si->ident; si++) {
                Subsystem s;
                s.ident = si->ident;
                s.desc = si->desc ? si->desc : "";
                s.id = si->id;
                s.num_roms = si->num_roms;
                subsystems_.push_back(s);
            }
            return true;
        }
        case RETRO_ENVIRONMENT_SET_HW_RENDER:
            rlog("core requested hardware rendering: not supported (software cores only)");
            return false;
        default:
            return false;   // unknown / unsupported: tell the core so
    }
}

/* ------------------------------------------------------------------ video */

void Core::video(const void* data, unsigned w, unsigned h, size_t pitch) {
    if (!data || data == RETRO_HW_FRAME_BUFFER_VALID) return;   // duped frame: keep the previous one
    if (frame_.w != (int)w || frame_.h != (int)h) {
        frame_.w = (int)w;
        frame_.h = (int)h;
    }
    frame_.px.resize((size_t)w * h);
    const uint8_t* src = (const uint8_t*)data;
    uint32_t* dst = frame_.px.data();
    switch (pixfmt_) {
        case RETRO_PIXEL_FORMAT_XRGB8888:
            for (unsigned y = 0; y < h; y++) {
                const uint32_t* s = (const uint32_t*)(src + y * pitch);
                for (unsigned x = 0; x < w; x++) *dst++ = s[x] | 0xFF000000u;
            }
            break;
        case RETRO_PIXEL_FORMAT_RGB565:
            for (unsigned y = 0; y < h; y++) {
                const uint16_t* s = (const uint16_t*)(src + y * pitch);
                for (unsigned x = 0; x < w; x++) {
                    uint32_t v = s[x], r = v >> 11, g = (v >> 5) & 63, b = v & 31;
                    r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
                    *dst++ = 0xFF000000u | (r << 16) | (g << 8) | b;
                }
            }
            break;
        default:   // 0RGB1555
            for (unsigned y = 0; y < h; y++) {
                const uint16_t* s = (const uint16_t*)(src + y * pitch);
                for (unsigned x = 0; x < w; x++) {
                    uint32_t v = s[x], r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
                    r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
                    *dst++ = 0xFF000000u | (r << 16) | (g << 8) | b;
                }
            }
    }
    frame_.serial++;
}

void Core::audio_batch(const int16_t* d, size_t frames) { if (on_audio && frames) on_audio(d, frames); }
void Core::audio_one(int16_t l, int16_t r) {
    int16_t s[2] = {l, r};
    if (on_audio) on_audio(s, 1);
}
