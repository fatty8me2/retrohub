#pragma once
/* Windows media layer of streamrun: capture sources, Media Foundation H.264 / AAC codecs, WASAPI process loopback. */
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rplay {

uint64_t steady_us();   // steady_clock microseconds (shared across processes on one machine)

struct BgraFrame {
    int w = 0, h = 0;
    std::vector<uint8_t> px;   // w*h*4, tightly packed
    uint64_t cap_us = 0;
};

/* ---- capture ---- */
class CaptureSource {
  public:
    virtual ~CaptureSource() {}
    /* Wait up to timeout_ms for a frame newer than the last one returned. */
    virtual bool grab(BgraFrame* out, int timeout_ms) = 0;
    virtual std::string name() const = 0;
};
std::unique_ptr<CaptureSource> make_test_pattern(int w, int h, int fps);
/* Window capture: Windows.Graphics.Capture; on failure DXGI desktop duplication cropped to the window; on failure
 * PrintWindow polling (used for hidden desktops where the other two cannot work). err filled when null is returned. */
std::unique_ptr<CaptureSource> make_window_capture(void* hwnd, int fps, std::string* err);
/* "<hwnd|pid>" -> HWND (0 if none). */
void* resolve_window(const std::string& arg, uint32_t* pid_out);

/* ---- video codec ---- */
struct EncodedFrame { std::vector<uint8_t> data; bool key = false; uint64_t cap_us = 0; };
class H264Encoder {
  public:
    ~H264Encoder();
    /* hw_ok: try NVENC/AMF/QSV MFTs first. */
    bool init(int w, int h, int fps, int kbps, bool hw_ok, std::string* err);
    /* bgra: tightly packed w_in x h_in; scaled to the encoder size. Appends finished frames to out. */
    bool encode(const BgraFrame& in, bool force_key, std::vector<EncodedFrame>* out);
    void set_bitrate(int kbps);
    std::string name() const { return name_; }
    int width() const { return w_; }
    int height() const { return h_; }
    bool hardware() const { return hw_; }
    struct Impl;
  private:
    Impl* d_ = nullptr;
    int w_ = 0, h_ = 0; bool hw_ = false; std::string name_;
};

struct DecodedFrame { int w = 0, h = 0; std::vector<uint8_t> nv12; uint64_t cap_us = 0; };
class H264Decoder {
  public:
    H264Decoder();
    ~H264Decoder();
    bool init(bool try_d3d11, std::string* err);
    /* Feed one access unit; decoded pictures are appended to out. */
    bool decode(const uint8_t* au, size_t len, uint64_t cap_us, std::vector<DecodedFrame>* out);
    bool hardware() const;
  private:
    struct Impl;
    Impl* d_;
};

/* ---- audio ---- */
class AudioCapture {
  public:
    virtual ~AudioCapture() {}
    /* Blocks up to timeout_ms; appends 48 kHz stereo s16le interleaved samples. */
    virtual bool read(std::vector<int16_t>* pcm, int timeout_ms) = 0;
    virtual std::string name() const = 0;
};
std::unique_ptr<AudioCapture> make_process_loopback(uint32_t pid, std::string* err);
std::unique_ptr<AudioCapture> make_tone_source();

class AacEncoder {
  public:
    AacEncoder(); ~AacEncoder();
    bool init(std::string* err);
    /* pcm: 48k stereo s16; appends complete ADTS frames to out (each one AAC frame, 1024 samples). */
    bool encode(const int16_t* pcm, size_t samples_per_ch, std::vector<std::vector<uint8_t>>* out);
  private:
    struct Impl; Impl* d_;
};
class AacDecoder {
  public:
    AacDecoder(); ~AacDecoder();
    bool init(std::string* err);
    bool decode(const uint8_t* adts, size_t len, std::vector<int16_t>* pcm);
  private:
    struct Impl; Impl* d_;
};

/* UPnP IGD: map UDP port externally == internally. Returns true on success. Blocks up to ~3 s. */
bool upnp_map_udp(uint16_t port, std::string* info);

} // namespace rplay
