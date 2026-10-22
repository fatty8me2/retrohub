#pragma once
/* libretro core host: loads a *_libretro.dll, implements the environment/video/audio/input
 * callbacks (software rendering only) and exposes a small C++ API to the frontend. */

#include <windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "third_party/libretro.h"

void rlog(const char* fmt, ...);
void rlog_set_file(const std::string& path);

/* Last video frame, converted to 0xAARRGGBB. */
struct FrameBuf {
    int w = 0, h = 0;
    std::vector<uint32_t> px;
    uint64_t serial = 0;   // increments on every retro_video_refresh with data
};

struct CoreOption {
    std::string key, desc, def;
    std::vector<std::string> values;
};

class Core {
  public:
    Core();
    ~Core();

    /* Directories handed to the core (set before load_core). */
    std::string system_dir, save_dir, content_dir;
    std::string system_tag;                       // gba / gb / gbc (informational)
    std::map<std::string, std::string> overrides; // option key -> value (from ini / --opt)

    /* Frontend hooks. */
    std::function<void()> on_poll;                                            // retro_input_poll
    std::function<int16_t(unsigned, unsigned, unsigned, unsigned)> on_input;  // port, device, index, id
    std::function<void(const int16_t*, size_t)> on_audio;                     // interleaved stereo frames
    std::function<void()> on_geometry;                                        // SET_GEOMETRY / SET_SYSTEM_AV_INFO

    bool load_core(const std::string& dll_path, std::string* err);
    bool load_game(const std::string& rom_path, std::string* err);
    void run() { fn_run_(); }
    void unload();

    const retro_system_info& info() const { return sysinfo_; }
    const retro_system_av_info& av() const { return av_; }
    const FrameBuf& frame() const { return frame_; }
    bool game_loaded() const { return loaded_; }

    /* Memory + states. */
    void* save_ram(size_t* size);
    size_t serialize_size() { return fn_serialize_size_(); }
    bool serialize(void* buf, size_t size) { return fn_serialize_(buf, size); }
    bool unserialize(const void* buf, size_t size) { return fn_unserialize_(buf, size); }

    void set_fast_forwarding(bool ff) { ff_ = ff; }
    const std::map<std::string, CoreOption>& options() const { return opts_; }
    /* Change an option while running (the core sees it on its next GET_VARIABLE_UPDATE poll). */
    void set_option(const std::string& key, const std::string& value) { overrides[key] = value; vars_dirty_ = true; }

    /* Used by the static trampolines. */
    bool environment(unsigned cmd, void* data);
    void video(const void* data, unsigned w, unsigned h, size_t pitch);
    void audio_batch(const int16_t* d, size_t frames);
    void audio_one(int16_t l, int16_t r);
    void poll() { if (on_poll) on_poll(); }
    int16_t input(unsigned port, unsigned dev, unsigned idx, unsigned id) {
        return on_input ? on_input(port, dev, idx, id) : 0;
    }

  private:
    void add_option(const char* key, const char* desc, const char* def, std::vector<std::string> vals);
    void parse_v1_defs(const retro_core_option_definition* d);
    void parse_v2_defs(const retro_core_option_v2_definition* d);

    HMODULE dll_ = nullptr;
    bool loaded_ = false, inited_ = false, ff_ = false;
    std::string core_path_;
    std::string game_path_;
    std::vector<uint8_t> game_data_;

    void (*fn_set_env_)(retro_environment_t) = nullptr;
    void (*fn_set_video_)(retro_video_refresh_t) = nullptr;
    void (*fn_set_audio_)(retro_audio_sample_t) = nullptr;
    void (*fn_set_audio_batch_)(retro_audio_sample_batch_t) = nullptr;
    void (*fn_set_poll_)(retro_input_poll_t) = nullptr;
    void (*fn_set_input_)(retro_input_state_t) = nullptr;
    void (*fn_init_)() = nullptr;
    void (*fn_deinit_)() = nullptr;
    unsigned (*fn_api_version_)() = nullptr;
    void (*fn_get_info_)(retro_system_info*) = nullptr;
    void (*fn_get_av_)(retro_system_av_info*) = nullptr;
    void (*fn_set_port_dev_)(unsigned, unsigned) = nullptr;
    void (*fn_reset_)() = nullptr;
    void (*fn_run_)() = nullptr;
    size_t (*fn_serialize_size_)() = nullptr;
    bool (*fn_serialize_)(void*, size_t) = nullptr;
    bool (*fn_unserialize_)(const void*, size_t) = nullptr;
    bool (*fn_load_game_)(const retro_game_info*) = nullptr;
    void (*fn_unload_game_)() = nullptr;
    void* (*fn_get_mem_data_)(unsigned) = nullptr;
    size_t (*fn_get_mem_size_)(unsigned) = nullptr;

    retro_system_info sysinfo_{};
    retro_system_av_info av_{};
    unsigned pixfmt_ = RETRO_PIXEL_FORMAT_0RGB1555;
    FrameBuf frame_;
    std::map<std::string, CoreOption> opts_;
    bool vars_dirty_ = true;
    std::string lang_dummy_;
};

Core* active_core();
