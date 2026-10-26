// Test pattern, BGRA->NV12, Media Foundation H.264 encoder and decoder.
#include "media.h"

#include <windows.h>
#include <d3d11.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>
#include <strmif.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace rplay {

uint64_t steady_us()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/* ------------------------------------------------------------------ test pattern */

namespace {

class TestPattern : public CaptureSource {
  public:
    TestPattern(int w, int h, int fps) : w_(w), h_(h), period_us_(1000000 / std::max(1, fps)) { next_ = steady_us(); }
    bool grab(BgraFrame* out, int) override
    {
        uint64_t now = steady_us();
        if (now < next_) std::this_thread::sleep_for(std::chrono::microseconds(next_ - now));
        now = steady_us();
        next_ += period_us_;
        if (now > next_ + 4 * period_us_) next_ = now + period_us_;
        out->w = w_; out->h = h_; out->px.resize((size_t)w_ * h_ * 4); out->cap_us = steady_us();
        uint32_t f = n_++;
        uint8_t* p = out->px.data();
        int bar = (int)((f * 11) % (uint32_t)w_);
        for (int y = 0; y < h_; y++) {
            for (int x = 0; x < w_; x++) {
                uint8_t* q = p + ((size_t)y * w_ + x) * 4;
                uint32_t noise = (uint32_t)((x * 73856093u) ^ (y * 19349663u) ^ (f * 83492791u));
                q[0] = (uint8_t)(x * 255 / w_); q[1] = (uint8_t)(y * 255 / h_); q[2] = (uint8_t)((f * 3) & 255);
                if (((noise >> 7) & 7) == 0) { q[0] ^= 0x30; q[1] ^= 0x30; }   // texture so the encoder has real work
                if (abs(x - bar) < 24) { q[0] = q[1] = q[2] = 255; }
            }
        }
        return true;
    }
    std::string name() const override { return "test pattern"; }
  private:
    int w_, h_; uint64_t period_us_, next_; uint32_t n_ = 0;
};

void bgra_to_nv12(const BgraFrame& in, int dw, int dh, uint8_t* dst)
{
    uint8_t* Y = dst; uint8_t* UV = dst + (size_t)dw * dh;
    std::vector<int> xmap(dw);
    for (int x = 0; x < dw; x++) xmap[x] = (int)((int64_t)x * in.w / dw) * 4;
    for (int y = 0; y < dh; y++) {
        const uint8_t* row = in.px.data() + (size_t)((int64_t)y * in.h / dh) * in.w * 4;
        uint8_t* yo = Y + (size_t)y * dw;
        for (int x = 0; x < dw; x++) {
            const uint8_t* s = row + xmap[x];
            int B = s[0], G = s[1], R = s[2];
            yo[x] = (uint8_t)(((47 * R + 157 * G + 16 * B + 128) >> 8) + 16);
            if (!(y & 1) && !(x & 1)) {
                uint8_t* uv = UV + (size_t)(y / 2) * dw + x;
                uv[0] = (uint8_t)std::clamp(((-26 * R - 87 * G + 112 * B + 128) >> 8) + 128, 0, 255);
                uv[1] = (uint8_t)std::clamp(((112 * R - 102 * G - 10 * B + 128) >> 8) + 128, 0, 255);
            }
        }
    }
}

std::string hr_str(const char* what, HRESULT hr) { char b[96]; snprintf(b, sizeof b, "%s failed (0x%08lX)", what, (unsigned long)hr); return b; }

bool has_sps(const std::vector<uint8_t>& d)
{
    size_t n = std::min<size_t>(d.size(), 96);
    for (size_t i = 0; i + 4 < n; i++)
        if (d[i] == 0 && d[i + 1] == 0 && (d[i + 2] == 1 || (d[i + 2] == 0 && d[i + 3] == 1))) {
            size_t k = i + (d[i + 2] == 1 ? 3 : 4);
            if (k < d.size() && (d[k] & 0x1F) == 7) return true;
        }
    return false;
}

struct MfInit {
    MfInit() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); MFStartup(MF_VERSION, MFSTARTUP_FULL); }
};
void ensure_mf() { static MfInit once; }

} // namespace

std::unique_ptr<CaptureSource> make_test_pattern(int w, int h, int fps) { return std::make_unique<TestPattern>(w, h, fps); }

/* ------------------------------------------------------------------ encoder */

struct H264Encoder::Impl {
    ComPtr<IMFTransform> mft;
    ComPtr<ICodecAPI> api;
    ComPtr<IMFMediaEventGenerator> evg;
    bool async = false, provides = false;
    DWORD out_size = 0;
    int need_input = 0, fps = 60, w = 0, h = 0;
    std::vector<uint8_t> seq_hdr, nv12;
    bool pull(std::vector<EncodedFrame>* out);
    bool feed(const BgraFrame& in, bool key, std::vector<EncodedFrame>* out);
    void set_u32(const GUID& g, uint32_t v)
    {
        if (!api) return;
        VARIANT var; VariantInit(&var); var.vt = VT_UI4; var.ulVal = v;
        api->SetValue(&g, &var);
    }
};

bool H264Encoder::Impl::pull(std::vector<EncodedFrame>* out)
{
    MFT_OUTPUT_DATA_BUFFER ob = {};
    ComPtr<IMFSample> own;
    if (!provides) {
        ComPtr<IMFMediaBuffer> mb; MFCreateMemoryBuffer(out_size ? out_size : (1 << 20), &mb);
        MFCreateSample(&own); own->AddBuffer(mb.Get()); ob.pSample = own.Get();
    }
    DWORD st = 0;
    HRESULT hr = mft->ProcessOutput(0, 1, &ob, &st);
    if (ob.pEvents) ob.pEvents->Release();
    if (FAILED(hr)) return false;
    IMFSample* s = ob.pSample;
    if (s) {
        ComPtr<IMFMediaBuffer> mb; s->ConvertToContiguousBuffer(&mb);
        BYTE* p = nullptr; DWORD len = 0;
        if (mb && SUCCEEDED(mb->Lock(&p, nullptr, &len))) {
            EncodedFrame ef; ef.data.assign(p, p + len); mb->Unlock();
            LONGLONG t = 0; s->GetSampleTime(&t); ef.cap_us = (uint64_t)t / 10;
            UINT32 clean = 0; s->GetUINT32(MFSampleExtension_CleanPoint, &clean);
            ef.key = clean != 0;
            if (ef.key && !has_sps(ef.data) && !seq_hdr.empty()) ef.data.insert(ef.data.begin(), seq_hdr.begin(), seq_hdr.end());
            out->push_back(std::move(ef));
        }
        if (provides) s->Release();
    }
    return true;
}

bool H264Encoder::Impl::feed(const BgraFrame& in, bool key, std::vector<EncodedFrame>* out)
{
    nv12.resize((size_t)w * h * 3 / 2);
    bgra_to_nv12(in, w, h, nv12.data());
    ComPtr<IMFMediaBuffer> mb;
    if (FAILED(MFCreateMemoryBuffer((DWORD)nv12.size(), &mb))) return false;
    BYTE* p = nullptr;
    mb->Lock(&p, nullptr, nullptr); memcpy(p, nv12.data(), nv12.size()); mb->Unlock();
    mb->SetCurrentLength((DWORD)nv12.size());
    ComPtr<IMFSample> sm; MFCreateSample(&sm); sm->AddBuffer(mb.Get());
    sm->SetSampleTime((LONGLONG)in.cap_us * 10); sm->SetSampleDuration(10000000LL / fps);
    if (key) set_u32(CODECAPI_AVEncVideoForceKeyFrame, 1);
    size_t before = out->size();
    if (!async) {
        HRESULT hr = mft->ProcessInput(0, sm.Get(), 0);
        if (FAILED(hr)) return false;
        while (pull(out)) {}
        return true;
    }
    bool fed = false;
    uint64_t t0 = steady_us();
    while (steady_us() - t0 < 60000) {
        ComPtr<IMFMediaEvent> ev;
        HRESULT hr = evg->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
        if (hr == MF_E_NO_EVENTS_AVAILABLE) {
            if (need_input > 0 && !fed) { }   // fall through to feed
            else { if (fed && out->size() > before) break; std::this_thread::sleep_for(std::chrono::microseconds(300)); continue; }
        } else if (FAILED(hr)) return false;
        else {
            MediaEventType t; ev->GetType(&t);
            if (t == METransformNeedInput) need_input++;
            else if (t == METransformHaveOutput) pull(out);
        }
        if (need_input > 0 && !fed) {
            if (FAILED(mft->ProcessInput(0, sm.Get(), 0))) return false;
            need_input--; fed = true;
        }
        if (fed && out->size() > before) break;
    }
    return fed;
}

H264Encoder::~H264Encoder()
{
    if (d_) {
        if (d_->mft) d_->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        delete d_;
    }
}

static bool setup_encoder(H264Encoder::Impl& d, IMFActivate* act, bool hw, int w, int h, int fps, int kbps, std::string* err)
{
    HRESULT hr;
    if (act) hr = act->ActivateObject(IID_PPV_ARGS(&d.mft));
    else hr = CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d.mft));
    if (FAILED(hr)) { *err = hr_str("activate encoder", hr); return false; }
    ComPtr<IMFAttributes> at; d.mft->GetAttributes(&at);
    UINT32 a = 0;
    if (at && SUCCEEDED(at->GetUINT32(MF_TRANSFORM_ASYNC, &a)) && a) {
        d.async = true; at->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        d.mft.As(&d.evg);
    }
    if (at) at->SetUINT32(MF_LOW_LATENCY, TRUE);
    d.mft.As(&d.api);
    d.w = w; d.h = h; d.fps = fps;
    // codec settings first (some encoders latch them at type negotiation)
    d.set_u32(CODECAPI_AVLowLatencyMode, 1);
    d.set_u32(CODECAPI_AVEncCommonRealTime, 1);
    d.set_u32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
    d.set_u32(CODECAPI_AVEncCommonMeanBitRate, (uint32_t)kbps * 1000);
    d.set_u32(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    d.set_u32(CODECAPI_AVEncMPVGOPSize, (uint32_t)fps * 2);
    ComPtr<IMFMediaType> ot; MFCreateMediaType(&ot);
    ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); ot->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    ot->SetUINT32(MF_MT_AVG_BITRATE, (uint32_t)kbps * 1000);
    MFSetAttributeSize(ot.Get(), MF_MT_FRAME_SIZE, w, h); MFSetAttributeRatio(ot.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(ot.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    ot->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    ot->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
    hr = d.mft->SetOutputType(0, ot.Get(), 0);
    if (FAILED(hr)) { *err = hr_str("SetOutputType", hr); return false; }
    ComPtr<IMFMediaType> it; MFCreateMediaType(&it);
    it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); it->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(it.Get(), MF_MT_FRAME_SIZE, w, h); MFSetAttributeRatio(it.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(it.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    it->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = d.mft->SetInputType(0, it.Get(), 0);
    if (FAILED(hr)) { *err = hr_str("SetInputType", hr); return false; }
    ComPtr<IMFMediaType> cur;
    if (SUCCEEDED(d.mft->GetOutputCurrentType(0, &cur))) {
        UINT32 n = 0;
        if (SUCCEEDED(cur->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &n)) && n) {
            d.seq_hdr.resize(n); cur->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, d.seq_hdr.data(), n, nullptr);
        }
    }
    MFT_OUTPUT_STREAM_INFO si = {};
    d.mft->GetOutputStreamInfo(0, &si);
    d.provides = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    d.out_size = si.cbSize;
    d.mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    d.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    d.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    (void)hw;
    return true;
}

bool H264Encoder::init(int w, int h, int fps, int kbps, bool hw_ok, std::string* err)
{
    ensure_mf();
    w &= ~1; h &= ~1; std::string e;
    auto probe = [&](Impl& d) {
        BgraFrame f; f.w = 320; f.h = 180; f.px.assign((size_t)320 * 180 * 4, 90);
        std::vector<EncodedFrame> o;
        for (int i = 0; i < 12 && o.empty(); i++) { f.cap_us = (uint64_t)i * 16000; if (!d.feed(f, i == 0, &o)) return false; }
        return !o.empty();
    };
    if (hw_ok) {
        MFT_REGISTER_TYPE_INFO oi = { MFMediaType_Video, MFVideoFormat_H264 };
        IMFActivate** acts = nullptr; UINT32 n = 0;
        if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, nullptr, &oi, &acts, &n))) {
            for (UINT32 i = 0; i < n && !d_; i++) {
                Impl* d = new Impl;
                std::string e2;
                bool ok = setup_encoder(*d, acts[i], true, w, h, fps, kbps, &e2);
                if (ok) {
                    // probe at the real size would be slow; probe with a tiny frame scaled up by the converter
                    d->w = w; d->h = h;
                    ok = probe(*d);
                    if (!ok) e2 = "no output from encoder";
                }
                if (ok) {
                    d_ = d; hw_ = true;
                    LPWSTR nm = nullptr; UINT32 nl = 0;
                    if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &nm, &nl))) {
                        char b[128]; WideCharToMultiByte(CP_UTF8, 0, nm, -1, b, sizeof b, nullptr, nullptr); name_ = b; CoTaskMemFree(nm);
                    }
                } else { delete d; e += e2 + "; "; }
            }
            for (UINT32 i = 0; i < n; i++) acts[i]->Release();
            CoTaskMemFree(acts);
        }
    }
    if (!d_) {
        Impl* d = new Impl; std::string e2;
        if (setup_encoder(*d, nullptr, false, w, h, fps, kbps, &e2) && probe(*d)) { d_ = d; hw_ = false; name_ = "Microsoft H.264 (software)"; }
        else { delete d; e += e2; }
    }
    if (!d_) { if (err) *err = "no H.264 encoder: " + e; return false; }
    w_ = w; h_ = h;
    d_->set_u32(CODECAPI_AVEncVideoForceKeyFrame, 1);
    return true;
}

bool H264Encoder::encode(const BgraFrame& in, bool force_key, std::vector<EncodedFrame>* out)
{
    return d_ && d_->feed(in, force_key, out);
}

void H264Encoder::set_bitrate(int kbps) { if (d_) d_->set_u32(CODECAPI_AVEncCommonMeanBitRate, (uint32_t)kbps * 1000); }

/* ------------------------------------------------------------------ decoder */

struct H264Decoder::Impl {
    ComPtr<IMFTransform> mft;
    ComPtr<ID3D11Device> dev;
    ComPtr<IMFDXGIDeviceManager> mgr;
    bool hw = false, provides = false;
    DWORD out_size = 0;
    int w = 0, h = 0;
    bool set_output_type()
    {
        for (DWORD i = 0;; i++) {
            ComPtr<IMFMediaType> t;
            if (FAILED(mft->GetOutputAvailableType(0, i, &t))) return false;
            GUID sub; t->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub == MFVideoFormat_NV12 && SUCCEEDED(mft->SetOutputType(0, t.Get(), 0))) {
                UINT32 ww = 0, hh = 0; MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &ww, &hh); w = (int)ww; h = (int)hh;
                MFT_OUTPUT_STREAM_INFO si = {}; mft->GetOutputStreamInfo(0, &si);
                provides = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
                out_size = si.cbSize;
                return true;
            }
        }
    }
};

H264Decoder::H264Decoder() : d_(new Impl) {}
H264Decoder::~H264Decoder() { delete d_; }
bool H264Decoder::hardware() const { return d_->hw; }

bool H264Decoder::init(bool try_d3d, std::string* err)
{
    ensure_mf();
    Impl& d = *d_;
    HRESULT hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d.mft));
    if (FAILED(hr)) { if (err) *err = hr_str("create decoder", hr); return false; }
    ComPtr<IMFAttributes> at; d.mft->GetAttributes(&at);
    if (at) {
        at->SetUINT32(MF_LOW_LATENCY, TRUE);
        UINT32 aware = 0;
        if (try_d3d && SUCCEEDED(at->GetUINT32(MF_SA_D3D11_AWARE, &aware)) && aware) {
            UINT tok = 0;
            if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                    D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d.dev, nullptr, nullptr))) {
                ComPtr<ID3D10Multithread> mt; if (SUCCEEDED(d.dev.As(&mt))) mt->SetMultithreadProtected(TRUE);
                if (SUCCEEDED(MFCreateDXGIDeviceManager(&tok, &d.mgr)) && SUCCEEDED(d.mgr->ResetDevice(d.dev.Get(), tok)) &&
                    SUCCEEDED(d.mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)d.mgr.Get())))
                    d.hw = true;
            }
        }
    }
    ComPtr<IMFMediaType> it; MFCreateMediaType(&it);
    it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); it->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    it->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = d.mft->SetInputType(0, it.Get(), 0);
    if (FAILED(hr)) { if (err) *err = hr_str("decoder SetInputType", hr); return false; }
    d.set_output_type();   // may only succeed after the first SPS (handled in decode)
    d.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    d.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

bool H264Decoder::decode(const uint8_t* au, size_t len, uint64_t cap_us, std::vector<DecodedFrame>* out)
{
    Impl& d = *d_;
    ComPtr<IMFMediaBuffer> mb;
    if (FAILED(MFCreateMemoryBuffer((DWORD)len, &mb))) return false;
    BYTE* p = nullptr; mb->Lock(&p, nullptr, nullptr); memcpy(p, au, len); mb->Unlock(); mb->SetCurrentLength((DWORD)len);
    ComPtr<IMFSample> sm; MFCreateSample(&sm); sm->AddBuffer(mb.Get()); sm->SetSampleTime((LONGLONG)cap_us * 10);
    HRESULT hr = d.mft->ProcessInput(0, sm.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) { /* drain first */ }
    else if (FAILED(hr)) return false;
    for (int guard = 0; guard < 8; guard++) {
        MFT_OUTPUT_DATA_BUFFER ob = {};
        ComPtr<IMFSample> own;
        if (!d.provides) {
            ComPtr<IMFMediaBuffer> ob2; MFCreateMemoryBuffer(d.out_size ? d.out_size : (1920 * 1088 * 3 / 2), &ob2);
            MFCreateSample(&own); own->AddBuffer(ob2.Get()); ob.pSample = own.Get();
        }
        DWORD st = 0;
        hr = d.mft->ProcessOutput(0, 1, &ob, &st);
        if (ob.pEvents) ob.pEvents->Release();
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) { d.set_output_type(); continue; }
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
        if (FAILED(hr)) return false;
        IMFSample* s = ob.pSample;
        if (!s) continue;
        ComPtr<IMFMediaBuffer> buf; s->GetBufferByIndex(0, &buf);
        ComPtr<IMF2DBuffer> b2;
        BYTE* sc = nullptr; LONG pitch = 0; bool locked2d = false; BYTE* lin = nullptr;
        if (buf && SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2D(&sc, &pitch))) locked2d = true;
        else if (buf) { DWORD l = 0; buf->Lock(&lin, nullptr, &l); sc = lin; pitch = d.w; }
        if (sc && d.w > 0 && d.h > 0) {
            DecodedFrame f; f.w = d.w; f.h = d.h; f.cap_us = 0;
            LONGLONG t = 0; s->GetSampleTime(&t); f.cap_us = (uint64_t)t / 10;
            f.nv12.resize((size_t)d.w * d.h * 3 / 2);
            int ah = (d.h + 15) & ~15;
            for (int y = 0; y < d.h; y++) memcpy(&f.nv12[(size_t)y * d.w], sc + (size_t)y * pitch, d.w);
            for (int y = 0; y < d.h / 2; y++) memcpy(&f.nv12[(size_t)d.w * d.h + (size_t)y * d.w], sc + (size_t)(ah + y) * pitch, d.w);
            out->push_back(std::move(f));
        }
        if (locked2d) b2->Unlock2D(); else if (lin) buf->Unlock();
        if (d.provides) s->Release();
    }
    return true;
}

} // namespace rplay
