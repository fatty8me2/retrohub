#include "clip.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#include "../retrorun/third_party/stb_image_write.h"

namespace clip {

using Microsoft::WRL::ComPtr;

int sample_every(double src_fps, int target_fps) {
    if (src_fps <= 0 || target_fps <= 0) return 3;
    return std::max(1, (int)std::lround(src_fps / target_fps));
}

void scaled_size(int sw, int sh, int max_w, int* dw, int* dh) {
    int w = std::min(sw, max_w);
    if (w < 16) w = 16;
    int h = sw > 0 ? (int)((long long)sh * w / sw) : 16;
    *dw = std::max(2, w & ~1);
    *dh = std::max(2, h & ~1);
}

size_t capacity_frames(const Config& c, int w, int h) {
    size_t fb = (size_t)w * h * 3 / 2;
    size_t by_time = (size_t)std::max(1, c.seconds) * (size_t)std::max(1, c.fps);
    size_t by_mem = fb ? c.cap_bytes / fb : by_time;
    return std::max<size_t>(1, std::min(by_time, by_mem));
}

/* ---- ring ---- */

bool Ring::offer(const uint8_t* px, int w, int h, int stride, double src_fps, bool bgra, bool bottom_up) {
    if (!px || w <= 0 || h <= 0 || stride < w * 4) return false;
    int every = sample_every(src_fps, cfg_.fps);
    if ((seen_++ % (uint64_t)every) != 0) return false;
    int dw, dh;
    scaled_size(w, h, cfg_.max_w, &dw, &dh);
    auto f = std::make_shared<std::vector<uint8_t>>((size_t)dw * dh * 3 / 2);
    uint8_t* Y = f->data();
    uint8_t* UV = Y + (size_t)dw * dh;
    /* box-ish downscale: up to 3x3 samples per destination pixel, then RGB -> BT.601 NV12 */
    std::vector<int> r(dw), g(dw), b(dw);   // chroma accumulator for the even row
    std::vector<int> cr(dw), cg(dw), cb(dw);
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((long long)y * h / dh), y1 = std::max(y0 + 1, (int)((long long)(y + 1) * h / dh));
        int ys = std::max(1, (y1 - y0 + 2) / 3);
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((long long)x * w / dw), x1 = std::max(x0 + 1, (int)((long long)(x + 1) * w / dw));
            int xs = std::max(1, (x1 - x0 + 2) / 3);
            int ar = 0, ag = 0, ab = 0, n = 0;
            for (int yy = y0; yy < y1 && yy < h; yy += ys) {
                const uint8_t* row = px + (size_t)(bottom_up ? h - 1 - yy : yy) * stride;
                for (int xx = x0; xx < x1 && xx < w; xx += xs) {
                    const uint8_t* s = row + (size_t)xx * 4;
                    if (bgra) { ab += s[0]; ag += s[1]; ar += s[2]; } else { ar += s[0]; ag += s[1]; ab += s[2]; }
                    n++;
                }
            }
            if (!n) n = 1;
            int R = ar / n, G = ag / n, B = ab / n;
            Y[(size_t)y * dw + x] = (uint8_t)(((66 * R + 129 * G + 25 * B + 128) >> 8) + 16);
            if (!(y & 1)) { cr[x] = R; cg[x] = G; cb[x] = B; }
        }
        if (!(y & 1)) continue;
        (void)r; (void)g; (void)b;
        uint8_t* uv = UV + (size_t)(y / 2) * dw;
        for (int x = 0; x + 1 < dw; x += 2) {
            // chroma from the top-left sample of each 2x2 (cheap, fine at 320 px)
            int R = cr[x], G = cg[x], B = cb[x];
            uv[x] = (uint8_t)std::clamp(((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128, 0, 255);
            uv[x + 1] = (uint8_t)std::clamp(((112 * R - 94 * G - 18 * B + 128) >> 8) + 128, 0, 255);
        }
    }
    std::lock_guard<std::mutex> lk(m_);
    if (dw != w_ || dh != h_) { q_.clear(); w_ = dw; h_ = dh; }
    src_fps_ = src_fps;
    q_.push_back(std::move(f));
    size_t cap = capacity_frames(cfg_, dw, dh);
    while (q_.size() > cap) q_.pop_front();
    return true;
}

Snapshot Ring::snapshot() const {
    std::lock_guard<std::mutex> lk(m_);
    Snapshot s;
    s.w = w_; s.h = h_;
    s.fps = src_fps_ / sample_every(src_fps_, cfg_.fps);
    s.frames.assign(q_.begin(), q_.end());
    return s;
}
void Ring::clear() { std::lock_guard<std::mutex> lk(m_); q_.clear(); }
size_t Ring::frames() const { std::lock_guard<std::mutex> lk(m_); return q_.size(); }
size_t Ring::bytes() const { std::lock_guard<std::mutex> lk(m_); return q_.size() * (size_t)w_ * h_ * 3 / 2; }
double Ring::seconds() const { return snapshot().seconds(); }

/* ---- Media Foundation ---- */

namespace {

struct MfScope {
    bool com = false, mf = false;
    MfScope() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com = SUCCEEDED(hr);   // S_FALSE (already initialised) also needs the matching Uninitialize
        mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL));
    }
    ~MfScope() { if (mf) MFShutdown(); if (com) CoUninitialize(); }
};

std::string hr_str(const char* what, HRESULT hr) {
    char b[96];
    std::snprintf(b, sizeof b, "%s failed (0x%08lX)", what, (unsigned long)hr);
    return b;
}

void to_mem(void* ctx, void* data, int size) { static_cast<std::string*>(ctx)->append((const char*)data, (size_t)size); }

bool write_thumb(const Snapshot& s, const std::string& path) {
    if (s.frames.empty()) return false;
    const auto& f = *s.frames[std::min(s.frames.size() - 1, (size_t)(s.frames.size() * 0.85))];
    std::vector<uint8_t> rgb((size_t)s.w * s.h * 3);
    const uint8_t* Y = f.data();
    const uint8_t* UV = Y + (size_t)s.w * s.h;
    for (int y = 0; y < s.h; y++)
        for (int x = 0; x < s.w; x++) {
            int c = ((int)Y[(size_t)y * s.w + x] - 16) * 298;
            const uint8_t* uv = UV + (size_t)(y / 2) * s.w + (x & ~1);
            int d = (int)uv[0] - 128, e = (int)uv[1] - 128;
            uint8_t* o = &rgb[((size_t)y * s.w + x) * 3];
            o[0] = (uint8_t)std::clamp((c + 409 * e + 128) >> 8, 0, 255);
            o[1] = (uint8_t)std::clamp((c - 100 * d - 208 * e + 128) >> 8, 0, 255);
            o[2] = (uint8_t)std::clamp((c + 516 * d + 128) >> 8, 0, 255);
        }
    std::string jpg;
    if (!stbi_write_jpg_to_func(to_mem, &jpg, s.w, s.h, 3, rgb.data(), 85)) return false;
    FILE* fh = _wfopen(std::filesystem::u8path(path).c_str(), L"wb");
    if (!fh) return false;
    fwrite(jpg.data(), 1, jpg.size(), fh);
    fclose(fh);
    return true;
}

}  // namespace

bool write_mp4(const Snapshot& snap, const std::string& path, std::string* err) {
    auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
    if (snap.frames.empty() || snap.w < 2 || snap.h < 2) return fail("nothing recorded yet");
    MfScope mf;
    if (!mf.mf) return fail("Media Foundation is not available");
    std::filesystem::path fp = std::filesystem::u8path(path);
    std::error_code ec;
    if (fp.has_parent_path()) std::filesystem::create_directories(fp.parent_path(), ec);
    std::filesystem::path part = fp; part += L".part";
    std::filesystem::remove(part, ec);

    const UINT32 fps = (UINT32)std::max(1.0, std::round(snap.fps));
    const UINT32 bitrate = (UINT32)std::clamp<long long>((long long)snap.w * snap.h * 20, 600000, 6000000);
    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 2);
    attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    ComPtr<IMFSinkWriter> sw;
    HRESULT hr = MFCreateSinkWriterFromURL(part.c_str(), nullptr, attrs.Get(), &sw);
    if (FAILED(hr)) return fail(hr_str("MFCreateSinkWriterFromURL", hr));

    ComPtr<IMFMediaType> out, in;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    out->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, snap.w, snap.h);
    MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    DWORD stream = 0;
    hr = sw->AddStream(out.Get(), &stream);
    if (FAILED(hr)) return fail(hr_str("AddStream", hr));
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, snap.w, snap.h);
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = sw->SetInputMediaType(stream, in.Get(), nullptr);
    if (FAILED(hr)) return fail(hr_str("SetInputMediaType", hr));
    hr = sw->BeginWriting();
    if (FAILED(hr)) return fail(hr_str("BeginWriting", hr));

    const LONGLONG dur = 10000000LL / fps;
    const DWORD fb = (DWORD)((size_t)snap.w * snap.h * 3 / 2);
    LONGLONG t = 0;
    for (auto& f : snap.frames) {
        if (!f || f->size() < fb) continue;
        ComPtr<IMFMediaBuffer> mb;
        if (FAILED(MFCreateMemoryBuffer(fb, &mb))) return fail("MFCreateMemoryBuffer failed");
        BYTE* dst = nullptr;
        mb->Lock(&dst, nullptr, nullptr);
        std::memcpy(dst, f->data(), fb);
        mb->Unlock();
        mb->SetCurrentLength(fb);
        ComPtr<IMFSample> smp;
        MFCreateSample(&smp);
        smp->AddBuffer(mb.Get());
        smp->SetSampleTime(t);
        smp->SetSampleDuration(dur);
        hr = sw->WriteSample(stream, smp.Get());
        if (FAILED(hr)) return fail(hr_str("WriteSample", hr));
        t += dur;
    }
    hr = sw->Finalize();
    sw.Reset();
    if (FAILED(hr)) { std::filesystem::remove(part, ec); return fail(hr_str("Finalize", hr)); }
    std::filesystem::remove(fp, ec);
    std::filesystem::rename(part, fp, ec);
    if (ec) return fail("could not move the clip into place");
    write_thumb(snap, path + ".jpg");
    return true;
}

bool probe_mp4(const std::string& path, Info* out, std::string* err) {
    auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
    MfScope mf;
    if (!mf.mf) return fail("Media Foundation is not available");
    std::filesystem::path fp = std::filesystem::u8path(path);
    std::error_code ec;
    Info info;
    info.bytes = std::filesystem::exists(fp, ec) ? (uint64_t)std::filesystem::file_size(fp, ec) : 0;
    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 1);
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    ComPtr<IMFSourceReader> rd;
    HRESULT hr = MFCreateSourceReaderFromURL(fp.c_str(), attrs.Get(), &rd);
    if (FAILED(hr)) return fail(hr_str("MFCreateSourceReaderFromURL", hr));
    ComPtr<IMFMediaType> want;
    MFCreateMediaType(&want);
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    hr = rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, want.Get());
    if (FAILED(hr)) return fail(hr_str("SetCurrentMediaType(RGB32)", hr));
    ComPtr<IMFMediaType> got;
    if (SUCCEEDED(rd->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &got))) {
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(got.Get(), MF_MT_FRAME_SIZE, &w, &h);
        info.w = (int)w; info.h = (int)h;
    }
    LONGLONG last_ts = 0, last_dur = 0;
    for (;;) {
        DWORD flags = 0; LONGLONG ts = 0;
        ComPtr<IMFSample> smp;
        hr = rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &smp);
        if (FAILED(hr)) return fail(hr_str("ReadSample", hr));
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!smp) continue;
        info.frames++;
        last_ts = ts;
        smp->GetSampleDuration(&last_dur);
        ComPtr<IMFMediaBuffer> mb;
        smp->ConvertToContiguousBuffer(&mb);
        BYTE* p = nullptr; DWORD len = 0;
        if (mb && SUCCEEDED(mb->Lock(&p, nullptr, &len))) {
            double acc = 0; size_t n = 0;
            for (DWORD i = 0; i + 3 < len; i += 4 * 7) { acc += 0.114 * p[i] + 0.587 * p[i + 1] + 0.299 * p[i + 2]; n++; }
            info.mean_luma = n ? acc / n : 0;
            mb->Unlock();
        }
    }
    info.seconds = (double)(last_ts + last_dur) / 1e7;
    if (out) *out = info;
    if (info.frames == 0) return fail("no frames decoded");
    return true;
}

/* ---- recorder ---- */

std::string clip_name(std::time_t t) {
    std::tm tm{};
    localtime_s(&tm, &t);
    char b[40];
    std::strftime(b, sizeof b, "%Y%m%d-%H%M%S.mp4", &tm);
    return b;
}

bool Recorder::request_save(std::time_t t, std::string* msg) {
    auto fail = [&](const char* m) { if (msg) *msg = m; return false; };
    if (dir_.empty()) return fail("Clips are not available here");
    if (busy_.load()) return fail("Already saving a clip");
    if (th_.joinable()) th_.join();
    Snapshot s = ring_.snapshot();
    if (s.frames.size() < (size_t)std::max(1.0, s.fps)) return fail("Not enough footage yet");
    std::string path = dir_ + "\\" + clip_name(t);
    busy_ = true; done_ = false;
    th_ = std::thread([this, s = std::move(s), path]() mutable {
        std::string e;
        ok_ = write_mp4(s, path, &e);
        msg_ = ok_ ? path : e;
        busy_ = false;
        done_ = true;
    });
    return true;
}

bool Recorder::poll(bool* ok, std::string* msg) {
    if (!done_.load()) return false;
    done_ = false;
    if (th_.joinable()) th_.join();
    if (ok) *ok = ok_;
    if (msg) *msg = msg_;
    return true;
}

void Recorder::wait() { if (th_.joinable()) th_.join(); }

}  // namespace clip
