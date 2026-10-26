// WASAPI process loopback capture, tone source, AAC-LC (ADTS) encoder/decoder through Media Foundation.
#include "media.h"

#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <codecapi.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace rplay {

uint64_t steady_us();

namespace {

void ensure_mf_audio()
{
    static bool once = [] { CoInitializeEx(nullptr, COINIT_MULTITHREADED); MFStartup(MF_VERSION, MFSTARTUP_FULL); return true; }();
    (void)once;
}

class ActivateHandler : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
  public:
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT act_hr = E_FAIL;
    ComPtr<IAudioClient> client;
    ~ActivateHandler() { CloseHandle(ev); }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = --ref_; if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** pp) override
    {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IActivateAudioInterfaceCompletionHandler)) *pp = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        else if (iid == __uuidof(IAgileObject)) *pp = static_cast<IAgileObject*>(this);
        else { *pp = nullptr; return E_NOINTERFACE; }
        AddRef(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override
    {
        ComPtr<IUnknown> u; HRESULT hr = E_FAIL;
        op->GetActivateResult(&hr, &u);
        act_hr = hr;
        if (SUCCEEDED(hr) && u) u.As(&client);
        SetEvent(ev);
        return S_OK;
    }
  private:
    std::atomic<ULONG> ref_{1};
};

class ProcessLoopback : public AudioCapture {
  public:
    ~ProcessLoopback() { if (client_) client_->Stop(); if (ev_) CloseHandle(ev_); }
    bool init(uint32_t pid, std::string* err)
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        AUDIOCLIENT_ACTIVATION_PARAMS ap = {};
        ap.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        ap.ProcessLoopbackParams.TargetProcessId = pid;
        ap.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        PROPVARIANT pv = {}; pv.vt = VT_BLOB; pv.blob.cbSize = sizeof ap; pv.blob.pBlobData = (BYTE*)&ap;
        ActivateHandler* h = new ActivateHandler;
        ComPtr<IActivateAudioInterfaceAsyncOperation> op;
        HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, h, &op);
        if (FAILED(hr)) { h->Release(); *err = "ActivateAudioInterfaceAsync failed"; return false; }
        WaitForSingleObject(h->ev, 5000);
        if (FAILED(h->act_hr) || !h->client) { char b[80]; snprintf(b, sizeof b, "process loopback activation 0x%08lX", (unsigned long)h->act_hr); *err = b; h->Release(); return false; }
        client_ = h->client; h->Release();
        WAVEFORMATEX wf = {}; wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 2; wf.nSamplesPerSec = 48000; wf.wBitsPerSample = 16;
        wf.nBlockAlign = 4; wf.nAvgBytesPerSec = 48000 * 4;
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 200000, 0, &wf, nullptr);
        if (FAILED(hr)) { char b[64]; snprintf(b, sizeof b, "audio Initialize 0x%08lX", (unsigned long)hr); *err = b; return false; }
        ev_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        client_->SetEventHandle(ev_);
        if (FAILED(client_->GetService(IID_PPV_ARGS(&cap_)))) { *err = "GetService(capture)"; return false; }
        if (FAILED(client_->Start())) { *err = "audio Start"; return false; }
        return true;
    }
    bool read(std::vector<int16_t>* pcm, int timeout_ms) override
    {
        WaitForSingleObject(ev_, timeout_ms);
        bool any = false;
        for (;;) {
            UINT32 n = 0;
            if (FAILED(cap_->GetNextPacketSize(&n)) || n == 0) break;
            BYTE* d = nullptr; UINT32 fr = 0; DWORD fl = 0;
            if (FAILED(cap_->GetBuffer(&d, &fr, &fl, nullptr, nullptr))) break;
            size_t o = pcm->size(); pcm->resize(o + (size_t)fr * 2);
            if (fl & AUDCLNT_BUFFERFLAGS_SILENT) memset(&(*pcm)[o], 0, (size_t)fr * 4); else memcpy(&(*pcm)[o], d, (size_t)fr * 4);
            cap_->ReleaseBuffer(fr);
            any = true;
        }
        return any;
    }
    std::string name() const override { return "WASAPI process loopback"; }
  private:
    ComPtr<IAudioClient> client_; ComPtr<IAudioCaptureClient> cap_; HANDLE ev_ = nullptr;
};

class ToneSource : public AudioCapture {
  public:
    ToneSource() { next_ = steady_us(); }
    bool read(std::vector<int16_t>* pcm, int) override
    {
        uint64_t now = steady_us();
        if (now < next_) std::this_thread::sleep_for(std::chrono::microseconds(next_ - now));
        next_ += 10000;
        for (int i = 0; i < 480; i++) {
            int16_t s = (int16_t)(6000 * std::sin(ph_)); ph_ += 2 * 3.14159265 * 440.0 / 48000.0;
            pcm->push_back(s); pcm->push_back(s);
        }
        return true;
    }
    std::string name() const override { return "440 Hz tone"; }
  private:
    uint64_t next_; double ph_ = 0;
};

} // namespace

std::unique_ptr<AudioCapture> make_process_loopback(uint32_t pid, std::string* err)
{
    auto p = std::make_unique<ProcessLoopback>();
    if (!p->init(pid, err)) return nullptr;
    return p;
}
std::unique_ptr<AudioCapture> make_tone_source() { return std::make_unique<ToneSource>(); }

/* ---------------- AAC ---------------- */

struct AacEncoder::Impl { ComPtr<IMFTransform> mft; DWORD out_size = 8192; LONGLONG pos = 0; };
AacEncoder::AacEncoder() : d_(new Impl) {}
AacEncoder::~AacEncoder() { delete d_; }

bool AacEncoder::init(std::string* err)
{
    ensure_mf_audio();
    HRESULT hr = CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d_->mft));
    if (FAILED(hr)) { *err = "AAC encoder MFT unavailable"; return false; }
    ComPtr<IMFMediaType> ot; MFCreateMediaType(&ot);
    ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio); ot->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    ot->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16); ot->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
    ot->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2); ot->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
    ot->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 1); ot->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
    if (FAILED(hr = d_->mft->SetOutputType(0, ot.Get(), 0))) { *err = "AAC SetOutputType"; return false; }
    ComPtr<IMFMediaType> it; MFCreateMediaType(&it);
    it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio); it->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    it->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16); it->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
    it->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2); it->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    it->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192000);
    if (FAILED(hr = d_->mft->SetInputType(0, it.Get(), 0))) { *err = "AAC SetInputType"; return false; }
    MFT_OUTPUT_STREAM_INFO si = {}; d_->mft->GetOutputStreamInfo(0, &si); if (si.cbSize) d_->out_size = si.cbSize;
    d_->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    d_->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

bool AacEncoder::encode(const int16_t* pcm, size_t n, std::vector<std::vector<uint8_t>>* out)
{
    ComPtr<IMFMediaBuffer> mb; if (FAILED(MFCreateMemoryBuffer((DWORD)(n * 4), &mb))) return false;
    BYTE* p; mb->Lock(&p, nullptr, nullptr); memcpy(p, pcm, n * 4); mb->Unlock(); mb->SetCurrentLength((DWORD)(n * 4));
    ComPtr<IMFSample> s; MFCreateSample(&s); s->AddBuffer(mb.Get());
    s->SetSampleTime(d_->pos); LONGLONG dur = (LONGLONG)n * 10000000LL / 48000; s->SetSampleDuration(dur); d_->pos += dur;
    HRESULT hr = d_->mft->ProcessInput(0, s.Get(), 0);
    if (FAILED(hr) && hr != MF_E_NOTACCEPTING) { fprintf(stderr, "AAC enc ProcessInput 0x%08lX\n", (unsigned long)hr); return false; }
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER ob = {};
        ComPtr<IMFMediaBuffer> b2; MFCreateMemoryBuffer(d_->out_size, &b2);
        ComPtr<IMFSample> os; MFCreateSample(&os); os->AddBuffer(b2.Get()); ob.pSample = os.Get();
        DWORD st = 0;
        hr = d_->mft->ProcessOutput(0, 1, &ob, &st);
        if (ob.pEvents) ob.pEvents->Release();
        if (FAILED(hr)) { static int lg = 0; if (hr != MF_E_TRANSFORM_NEED_MORE_INPUT && lg++ < 3) fprintf(stderr, "AAC enc ProcessOutput 0x%08lX\n", (unsigned long)hr); break; }
        ComPtr<IMFMediaBuffer> cb; os->ConvertToContiguousBuffer(&cb);
        BYTE* q; DWORD l = 0;
        if (cb && SUCCEEDED(cb->Lock(&q, nullptr, &l))) { out->emplace_back(q, q + l); cb->Unlock(); }
    }
    return true;
}

struct AacDecoder::Impl { ComPtr<IMFTransform> mft; DWORD out_size = 16384; };
AacDecoder::AacDecoder() : d_(new Impl) {}
AacDecoder::~AacDecoder() { delete d_; }

bool AacDecoder::init(std::string* err)
{
    ensure_mf_audio();
    HRESULT hr = CoCreateInstance(CLSID_CMSAACDecMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d_->mft));
    if (FAILED(hr)) { *err = "AAC decoder MFT unavailable"; return false; }
    ComPtr<IMFMediaType> it; MFCreateMediaType(&it);
    it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio); it->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    it->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16); it->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
    it->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2); it->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 1);
    it->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
    const BYTE ud[14] = {1, 0, 0x29, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x11, 0x90};   // HEAACWAVEINFO tail + AudioSpecificConfig (LC, 48k, stereo)
    it->SetBlob(MF_MT_USER_DATA, ud, sizeof ud);
    if (FAILED(hr = d_->mft->SetInputType(0, it.Get(), 0))) { *err = "AAC dec SetInputType"; return false; }
    for (DWORD i = 0;; i++) {
        ComPtr<IMFMediaType> t; if (FAILED(d_->mft->GetOutputAvailableType(0, i, &t))) { *err = "AAC dec: no PCM output type"; return false; }
        GUID sub; t->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub == MFAudioFormat_PCM && SUCCEEDED(d_->mft->SetOutputType(0, t.Get(), 0))) break;
    }
    MFT_OUTPUT_STREAM_INFO si = {}; d_->mft->GetOutputStreamInfo(0, &si); if (si.cbSize) d_->out_size = si.cbSize;
    d_->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    d_->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

bool AacDecoder::decode(const uint8_t* adts, size_t len, std::vector<int16_t>* pcm)
{
    ComPtr<IMFMediaBuffer> mb; if (FAILED(MFCreateMemoryBuffer((DWORD)len, &mb))) return false;
    BYTE* p; mb->Lock(&p, nullptr, nullptr); memcpy(p, adts, len); mb->Unlock(); mb->SetCurrentLength((DWORD)len);
    ComPtr<IMFSample> s; MFCreateSample(&s); s->AddBuffer(mb.Get());
    if (FAILED(d_->mft->ProcessInput(0, s.Get(), 0))) return false;
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER ob = {};
        ComPtr<IMFMediaBuffer> b2; MFCreateMemoryBuffer(d_->out_size, &b2);
        ComPtr<IMFSample> os; MFCreateSample(&os); os->AddBuffer(b2.Get()); ob.pSample = os.Get();
        DWORD st = 0;
        HRESULT hr = d_->mft->ProcessOutput(0, 1, &ob, &st);
        if (ob.pEvents) ob.pEvents->Release();
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) continue;
        if (FAILED(hr)) break;
        ComPtr<IMFMediaBuffer> cb; os->ConvertToContiguousBuffer(&cb);
        BYTE* q; DWORD l = 0;
        if (cb && SUCCEEDED(cb->Lock(&q, nullptr, &l))) { size_t o = pcm->size(); pcm->resize(o + l / 2); memcpy(&(*pcm)[o], q, l / 2 * 2); cb->Unlock(); }
    }
    return true;
}

} // namespace rplay
