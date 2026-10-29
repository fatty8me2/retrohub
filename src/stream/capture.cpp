// Window capture: Windows.Graphics.Capture, DXGI desktop duplication fallback, PrintWindow fallback.
#include "media.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <inspectable.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wrl/client.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

using Microsoft::WRL::ComPtr;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wdx = winrt::Windows::Graphics::DirectX;
namespace wd3 = winrt::Windows::Graphics::DirectX::Direct3D11;

extern "C" {
HRESULT __stdcall CreateDirect3D11DeviceFromDXGIDevice(::IDXGIDevice*, ::IInspectable**);
}
struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1")) IDirect3DDxgiInterfaceAccess : ::IUnknown {
    virtual HRESULT __stdcall GetInterface(REFIID iid, void** p) = 0;
};

namespace rplay {

namespace {

struct LatestFrame {
    std::mutex m; std::condition_variable cv;
    BgraFrame f; uint64_t seq = 0;
    void publish(const uint8_t* src, int pitch, int w, int h, uint64_t cap_us)
    {
        std::lock_guard<std::mutex> g(m);
        f.w = w; f.h = h; f.cap_us = cap_us; f.px.resize((size_t)w * h * 4);
        for (int y = 0; y < h; y++) memcpy(&f.px[(size_t)y * w * 4], src + (size_t)y * pitch, (size_t)w * 4);
        seq++;
        cv.notify_all();
    }
    bool take(BgraFrame* out, uint64_t* last, int timeout_ms)
    {
        std::unique_lock<std::mutex> g(m);
        if (!cv.wait_for(g, std::chrono::milliseconds(timeout_ms), [&] { return seq != *last; })) return false;
        *out = f; *last = seq; return true;
    }
};

ComPtr<ID3D11Device> make_device()
{
    ComPtr<ID3D11Device> dev;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, nullptr)))
        return nullptr;
    return dev;
}

/* ---------------- WGC ---------------- */
class WgcCapture : public CaptureSource {
  public:
    ~WgcCapture()
    {
        stop_ = true;
        try { if (session_) session_.Close(); if (pool_) { pool_.FrameArrived(token_); pool_.Close(); } } catch (...) {}
    }
    bool init(HWND hwnd, std::string* err)
    {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            if (!wgc::GraphicsCaptureSession::IsSupported()) { *err = "WGC not supported"; return false; }
            dev_ = make_device();
            if (!dev_) { *err = "D3D11 device"; return false; }
            ComPtr<IDXGIDevice> dxgi; dev_.As(&dxgi);
            ComPtr<IInspectable> insp;
            if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), &insp))) { *err = "CreateDirect3D11Device"; return false; }
            winrt::com_ptr<::IInspectable> tmp; tmp.attach(insp.Detach());
            d3ddev_ = tmp.as<wd3::IDirect3DDevice>();
            auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
            HRESULT hr = interop->CreateForWindow(hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item_));
            if (FAILED(hr)) { char b[64]; snprintf(b, sizeof b, "CreateForWindow 0x%08lX", (unsigned long)hr); *err = b; return false; }
            size_ = item_.Size();
            pool_ = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(d3ddev_, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, size_);
            token_ = pool_.FrameArrived([this](wgc::Direct3D11CaptureFramePool const& p, winrt::Windows::Foundation::IInspectable const&) { on_frame(p); });
            session_ = pool_.CreateCaptureSession(item_);
            try { session_.IsCursorCaptureEnabled(false); } catch (...) {}
            try { session_.IsBorderRequired(false); } catch (...) {}
            session_.StartCapture();
            return true;
        } catch (winrt::hresult_error const& e) {
            *err = "WGC: " + winrt::to_string(e.message()); return false;
        } catch (...) { *err = "WGC: exception"; return false; }
    }
    bool grab(BgraFrame* out, int timeout_ms) override { return latest_.take(out, &last_, timeout_ms); }
    std::string name() const override { return "Windows.Graphics.Capture"; }
  private:
    void on_frame(wgc::Direct3D11CaptureFramePool const& p)
    {
        if (stop_) return;
        try {
            auto fr = p.TryGetNextFrame();
            if (!fr) return;
            uint64_t t = steady_us();
            auto cs = fr.ContentSize();
            auto acc = fr.Surface().as<::IInspectable>();
            ComPtr<IDirect3DDxgiInterfaceAccess> ia;
            if (FAILED(acc->QueryInterface(__uuidof(IDirect3DDxgiInterfaceAccess), (void**)ia.GetAddressOf()))) return;
            ComPtr<ID3D11Texture2D> tex; ia->GetInterface(IID_PPV_ARGS(&tex));
            D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
            if (!stage_ || sd_.Width != d.Width || sd_.Height != d.Height) {
                sd_ = d; sd_.Usage = D3D11_USAGE_STAGING; sd_.BindFlags = 0; sd_.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd_.MiscFlags = 0;
                stage_.Reset(); dev_->CreateTexture2D(&sd_, nullptr, &stage_);
            }
            ComPtr<ID3D11DeviceContext> ctx; dev_->GetImmediateContext(&ctx);
            ctx->CopyResource(stage_.Get(), tex.Get());
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx->Map(stage_.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                latest_.publish((const uint8_t*)m.pData, (int)m.RowPitch, (int)std::min<UINT>(cs.Width, d.Width), (int)std::min<UINT>(cs.Height, d.Height), t);
                ctx->Unmap(stage_.Get(), 0);
            }
            if (cs.Width != size_.Width || cs.Height != size_.Height) {   // window resized
                size_ = cs;
                p.Recreate(d3ddev_, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, size_);
            }
        } catch (...) {}
    }
    ComPtr<ID3D11Device> dev_; ComPtr<ID3D11Texture2D> stage_; D3D11_TEXTURE2D_DESC sd_ = {};
    wd3::IDirect3DDevice d3ddev_{nullptr};
    wgc::GraphicsCaptureItem item_{nullptr};
    wgc::Direct3D11CaptureFramePool pool_{nullptr};
    wgc::GraphicsCaptureSession session_{nullptr};
    winrt::event_token token_{};
    winrt::Windows::Graphics::SizeInt32 size_{};
    LatestFrame latest_; uint64_t last_ = 0; std::atomic<bool> stop_{false};
};

/* ---------------- DXGI desktop duplication (cropped to the window) ---------------- */
class DxgiCapture : public CaptureSource {
  public:
    bool init(HWND hwnd, std::string* err)
    {
        hwnd_ = hwnd;
        dev_ = make_device();
        if (!dev_) { *err = "D3D11 device"; return false; }
        RECT wr; GetWindowRect(hwnd, &wr);
        ComPtr<IDXGIDevice> dd; dev_.As(&dd); ComPtr<IDXGIAdapter> ad; dd->GetAdapter(&ad);
        // the output must belong to the device's adapter; try every output of every adapter via a factory
        ComPtr<IDXGIFactory1> fac; CreateDXGIFactory1(IID_PPV_ARGS(&fac));
        for (UINT a = 0;; a++) {
            ComPtr<IDXGIAdapter1> ad1; if (fac->EnumAdapters1(a, &ad1) == DXGI_ERROR_NOT_FOUND) break;
            for (UINT o = 0;; o++) {
                ComPtr<IDXGIOutput> out; if (ad1->EnumOutputs(o, &out) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_OUTPUT_DESC od; out->GetDesc(&od);
                RECT ix; if (!IntersectRect(&ix, &od.DesktopCoordinates, &wr)) continue;
                ComPtr<ID3D11Device> d2;
                if (FAILED(D3D11CreateDevice(ad1.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d2, nullptr, nullptr))) continue;
                ComPtr<IDXGIOutput1> o1; out.As(&o1);
                if (o1 && SUCCEEDED(o1->DuplicateOutput(d2.Get(), &dup_))) { dev_ = d2; org_ = od.DesktopCoordinates; return true; }
            }
        }
        *err = "DXGI duplication unavailable"; return false;
    }
    bool grab(BgraFrame* out, int timeout_ms) override
    {
        DXGI_OUTDUPL_FRAME_INFO fi; ComPtr<IDXGIResource> res;
        HRESULT hr = dup_->AcquireNextFrame(timeout_ms, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
        if (FAILED(hr)) { lost_ = true; return false; }
        uint64_t t = steady_us();
        ComPtr<ID3D11Texture2D> tex; res.As(&tex);
        D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
        if (!stage_) { D3D11_TEXTURE2D_DESC s = d; s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ; s.MiscFlags = 0; dev_->CreateTexture2D(&s, nullptr, &stage_); }
        ComPtr<ID3D11DeviceContext> ctx; dev_->GetImmediateContext(&ctx);
        ctx->CopyResource(stage_.Get(), tex.Get());
        dup_->ReleaseFrame();
        RECT wr; GetWindowRect(hwnd_, &wr);
        int x0 = std::max<int>(0, wr.left - org_.left), y0 = std::max<int>(0, wr.top - org_.top);
        int x1 = std::min<int>(d.Width, wr.right - org_.left), y1 = std::min<int>(d.Height, wr.bottom - org_.top);
        int w = (x1 - x0) & ~1, h = (y1 - y0) & ~1;
        if (w < 16 || h < 16) return false;
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx->Map(stage_.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        out->w = w; out->h = h; out->cap_us = t; out->px.resize((size_t)w * h * 4);
        for (int y = 0; y < h; y++) memcpy(&out->px[(size_t)y * w * 4], (const uint8_t*)m.pData + (size_t)(y0 + y) * m.RowPitch + (size_t)x0 * 4, (size_t)w * 4);
        ctx->Unmap(stage_.Get(), 0);
        return true;
    }
    std::string name() const override { return "DXGI desktop duplication"; }
    bool lost() const { return lost_; }
  private:
    HWND hwnd_ = nullptr; RECT org_ = {}; bool lost_ = false;
    ComPtr<ID3D11Device> dev_; ComPtr<IDXGIOutputDuplication> dup_; ComPtr<ID3D11Texture2D> stage_;
};

/* ---------------- WGC with a DXGI fallback while the window owns the screen ----------------
 * A focused fullscreen game (Cemu on OpenGL above all) is presented straight to the display (independent flip), and WGC's
 * window capture then delivers no frames until the window loses focus: the guest's picture froze while the host played.
 * While the captured window is the foreground window and WGC has been silent for kStallUs, frames come from desktop
 * duplication cropped to the window (it covers the screen there, so nothing else of the desktop is sent). As soon as WGC
 * delivers again, or the window is no longer in front, WGC is used again. */
class HybridCapture : public CaptureSource {
  public:
    HybridCapture(std::unique_ptr<WgcCapture> w, HWND h) : wgc_(std::move(w)), hwnd_(h) { last_wgc_ = steady_us(); }
    bool grab(BgraFrame* out, int timeout_ms) override
    {
        const bool front = GetForegroundWindow() == hwnd_ && !IsIconic(hwnd_);
        if (dxgi_ && (!front)) dxgi_.reset();   // back to WGC: the window is behind something again
        if (!dxgi_) {
            if (wgc_->grab(out, timeout_ms)) { last_wgc_ = steady_us(); return true; }
            if (front && steady_us() - last_wgc_ > kStallUs && steady_us() >= retry_at_) {
                auto d = std::make_unique<DxgiCapture>();
                std::string e;
                if (d->init(hwnd_, &e)) { dxgi_ = std::move(d); ++switches_; }
                else retry_at_ = steady_us() + 2000000;   // duplication unavailable right now: try again in 2 s
            }
            return false;
        }
        BgraFrame w;
        if (wgc_->grab(&w, 0)) { dxgi_.reset(); last_wgc_ = steady_us(); *out = std::move(w); return true; }   // WGC is back
        if (dxgi_->grab(out, timeout_ms)) return true;
        if (dxgi_->lost()) dxgi_.reset();   // mode change / access lost: re-created on the next stall
        return false;
    }
    std::string name() const override { return dxgi_ ? "Windows.Graphics.Capture (DXGI while focused fullscreen)" : "Windows.Graphics.Capture"; }
  private:
    static constexpr uint64_t kStallUs = 400000;
    std::unique_ptr<WgcCapture> wgc_;
    std::unique_ptr<DxgiCapture> dxgi_;
    HWND hwnd_;
    uint64_t last_wgc_ = 0, retry_at_ = 0;
    int switches_ = 0;
};

/* ---------------- PrintWindow polling (works on a non-input desktop) ---------------- */
class PrintWindowCapture : public CaptureSource {
  public:
    PrintWindowCapture(HWND h, int fps) : hwnd_(h), period_(1000000 / (fps > 0 ? fps : 30)) { next_ = steady_us(); }
    ~PrintWindowCapture() { if (bmp_) DeleteObject(bmp_); if (dc_) DeleteDC(dc_); }
    bool grab(BgraFrame* out, int) override
    {
        uint64_t now = steady_us();
        if (now < next_) std::this_thread::sleep_for(std::chrono::microseconds(next_ - now));
        next_ = std::max(next_ + period_, steady_us());
        RECT r; if (!GetClientRect(hwnd_, &r)) return false;
        int w = (r.right & ~1), h = (r.bottom & ~1);
        if (w < 16 || h < 16) return false;
        if (!dc_ || w != w_ || h != h_) {
            if (bmp_) DeleteObject(bmp_);
            if (!dc_) dc_ = CreateCompatibleDC(nullptr);
            BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
            bmp_ = CreateDIBSection(dc_, &bi, DIB_RGB_COLORS, (void**)&bits_, nullptr, 0);
            SelectObject(dc_, bmp_); w_ = w; h_ = h;
        }
        if (!PrintWindow(hwnd_, dc_, PW_CLIENTONLY | 0x2 /*PW_RENDERFULLCONTENT*/)) return false;
        out->w = w; out->h = h; out->cap_us = steady_us(); out->px.assign(bits_, bits_ + (size_t)w * h * 4);
        for (size_t i = 3; i < out->px.size(); i += 4) out->px[i] = 255;
        return true;
    }
    std::string name() const override { return "PrintWindow"; }
  private:
    HWND hwnd_; uint64_t period_, next_; HDC dc_ = nullptr; HBITMAP bmp_ = nullptr; uint8_t* bits_ = nullptr; int w_ = 0, h_ = 0;
};

struct FindCtx { DWORD pid; HWND best; long area; };
BOOL CALLBACK enum_cb(HWND h, LPARAM l)
{
    FindCtx* c = (FindCtx*)l;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    RECT r; GetClientRect(h, &r);
    long a = r.right * r.bottom;
    if (a > c->area) { c->area = a; c->best = h; }
    return TRUE;
}

} // namespace

void* resolve_window(const std::string& arg, uint32_t* pid_out)
{
    unsigned long long v = strtoull(arg.c_str(), nullptr, 0);
    if (!v) return nullptr;
    HWND h = (HWND)(uintptr_t)v;
    if (IsWindow(h)) { DWORD pid = 0; GetWindowThreadProcessId(h, &pid); if (pid_out) *pid_out = pid; return h; }
    FindCtx c{(DWORD)v, nullptr, 0};
    // EnumWindows only sees the calling thread's desktop; EnumDesktopWindows on the current desktop is the same set.
    EnumWindows(enum_cb, (LPARAM)&c);
    if (pid_out) *pid_out = (DWORD)v;
    return c.best;
}

std::unique_ptr<CaptureSource> make_window_capture(void* hwnd, int fps, std::string* err)
{
    std::string e1, e2;
    { auto w = std::make_unique<WgcCapture>(); if (w->init((HWND)hwnd, &e1)) return std::make_unique<HybridCapture>(std::move(w), (HWND)hwnd); }
    { auto d = std::make_unique<DxgiCapture>(); if (d->init((HWND)hwnd, &e2)) return d; }
    // PrintWindow works everywhere but is slow and may miss GPU-composed content
    auto p = std::make_unique<PrintWindowCapture>((HWND)hwnd, fps);
    if (err) *err = "WGC: " + e1 + "; DXGI: " + e2 + "; falling back to PrintWindow";
    return p;
}

} // namespace rplay
