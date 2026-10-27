#include "ext_media.h"

#include <windows.h>
#include <audiopolicy.h>
#include <d3d11.h>
#include <dxgi.h>
#include <mmdeviceapi.h>
#include <unknwn.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "../retrorun/third_party/stb_image_write.h"

namespace extmedia {
namespace {

std::atomic<int> g_state{0};
std::mutex g_mu;
std::string g_msg;
std::thread g_th;

void put_png(void* ctx, void* data, int size) { static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), (size_t)size); }

bool write_png(const std::string& path_utf8, const uint8_t* bgra, int w, int h, int stride, std::string* err) {
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = bgra + (size_t)y * stride;
        uint8_t* d = rgba.data() + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x) { d[x * 4] = s[x * 4 + 2]; d[x * 4 + 1] = s[x * 4 + 1]; d[x * 4 + 2] = s[x * 4]; d[x * 4 + 3] = 255; }
    }
    std::string png;
    if (!stbi_write_png_to_func(put_png, &png, w, h, 4, rgba.data(), w * 4)) { *err = "could not encode the picture"; return false; }
    std::error_code ec;
    auto p = std::filesystem::u8path(path_utf8);
    std::filesystem::create_directories(p.parent_path(), ec);
    HANDLE f = CreateFileW(p.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { *err = "could not write the file"; return false; }
    DWORD wr = 0;
    bool ok = WriteFile(f, png.data(), (DWORD)png.size(), &wr, nullptr) && wr == png.size();
    CloseHandle(f);
    if (!ok) *err = "could not write the file";
    return ok;
}

bool all_black(const uint8_t* bgra, int w, int h, int stride) {
    for (int y = 0; y < h; y += 7) {
        const uint8_t* s = bgra + (size_t)y * stride;
        for (int x = 0; x < w; x += 5) if (s[x * 4] | s[x * 4 + 1] | s[x * 4 + 2]) return false;
    }
    return true;
}

/* Windows Graphics Capture of one window: works for D3D swap chains and for windows partly covered by others. */
bool capture_wgc(HWND hwnd, const std::string& out, std::string* err) {
    using namespace winrt::Windows::Graphics::Capture;
    using namespace winrt::Windows::Graphics::DirectX;
    using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
    if (!GraphicsCaptureSession::IsSupported()) { *err = "Windows Graphics Capture is not supported here"; return false; }
    winrt::com_ptr<ID3D11Device> dev;
    winrt::com_ptr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, dev.put(), nullptr, ctx.put()))) {
        *err = "no Direct3D device"; return false;
    }
    auto dxgi = dev.as<IDXGIDevice>();
    winrt::com_ptr<::IInspectable> insp;
    if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put()))) { *err = "no capture device"; return false; }
    auto d3d = insp.as<IDirect3DDevice>();
    auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{nullptr};
    if (FAILED(interop->CreateForWindow(hwnd, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)))) { *err = "cannot capture that window"; return false; }
    auto size = item.Size();
    if (size.Width <= 0 || size.Height <= 0) { *err = "the window has no size"; return false; }
    auto pool = Direct3D11CaptureFramePool::CreateFreeThreaded(d3d, DirectXPixelFormat::B8G8R8A8UIntNormalized, 1, size);
    auto session = pool.CreateCaptureSession(item);
    try { session.IsCursorCaptureEnabled(false); } catch (...) {}
    try { session.IsBorderRequired(false); } catch (...) {}
    session.StartCapture();
    Direct3D11CaptureFrame frame{nullptr};
    for (int i = 0; i < 150 && !frame; ++i) {   // up to ~2.2 s for the first frame
        frame = pool.TryGetNextFrame();
        if (!frame) Sleep(15);
    }
    bool ok = false;
    if (!frame) *err = "the game window produced no frame";
    else {
        auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> tex;
        if (FAILED(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), tex.put_void()))) *err = "cannot read the frame";
        else {
            D3D11_TEXTURE2D_DESC d{};
            tex->GetDesc(&d);
            d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
            winrt::com_ptr<ID3D11Texture2D> st;
            if (FAILED(dev->CreateTexture2D(&d, nullptr, st.put()))) *err = "cannot copy the frame";
            else {
                ctx->CopyResource(st.get(), tex.get());
                D3D11_MAPPED_SUBRESOURCE m{};
                if (FAILED(ctx->Map(st.get(), 0, D3D11_MAP_READ, 0, &m))) *err = "cannot read the frame";
                else {
                    if (all_black((const uint8_t*)m.pData, (int)d.Width, (int)d.Height, (int)m.RowPitch)) *err = "the picture was black";
                    else ok = write_png(out, (const uint8_t*)m.pData, (int)d.Width, (int)d.Height, (int)m.RowPitch, err);
                    ctx->Unmap(st.get(), 0);
                }
            }
        }
        frame.Close();
    }
    session.Close();
    pool.Close();
    return ok;
}

/* Fallback: PrintWindow with PW_RENDERFULLCONTENT (DWM composition of that window alone). */
bool capture_printwindow(HWND hwnd, const std::string& out, std::string* err) {
    RECT r{};
    if (!GetClientRect(hwnd, &r) || r.right <= 0 || r.bottom <= 0) { *err = "the window has no size"; return false; }
    int w = r.right, h = r.bottom;
    HDC sdc = GetDC(nullptr);
    HDC mdc = CreateCompatibleDC(sdc);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bm = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (bm && bits) {
        HGDIOBJ old = SelectObject(mdc, bm);
        BOOL pw = PrintWindow(hwnd, mdc, 0x4 /* PW_RENDERFULLCONTENT */ | 0x1 /* PW_CLIENTONLY */);
        SelectObject(mdc, old);
        if (!pw) *err = "PrintWindow failed";
        else if (all_black((const uint8_t*)bits, w, h, w * 4)) *err = "the picture was black";
        else ok = write_png(out, (const uint8_t*)bits, w, h, w * 4, err);
    } else *err = "out of memory";
    if (bm) DeleteObject(bm);
    DeleteDC(mdc);
    ReleaseDC(nullptr, sdc);
    return ok;
}

void run_capture(HWND hwnd, std::string out) {
    std::string err1, err2;
    bool ok = false;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);   // this worker owns its apartment
        ok = IsWindow(hwnd) && capture_wgc(hwnd, out, &err1);
    } catch (const winrt::hresult_error& e) { err1 = winrt::to_string(e.message()); }
    catch (...) { err1 = "capture error"; }
    if (!ok && IsWindow(hwnd)) ok = capture_printwindow(hwnd, out, &err2);
    std::lock_guard<std::mutex> lk(g_mu);
    if (ok) g_msg = out;
    else g_msg = !IsWindow(hwnd) ? "The game window is gone." : (err2.empty() ? err1 : err2 + " (" + err1 + ")");
    g_state = ok ? 2 : 3;
}

template <class T> void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

}  // namespace

bool capture_start(void* hwnd, const std::string& out_png_utf8) {
    if (!hwnd || g_state.load() == 1) return false;
    if (g_th.joinable()) g_th.join();
    g_state = 1;
    g_th = std::thread(run_capture, (HWND)hwnd, out_png_utf8);
    return true;
}

int capture_poll(std::string* msg) {
    int s = g_state.load();
    if (s == 2 || s == 3) {
        if (g_th.joinable()) g_th.join();
        std::lock_guard<std::mutex> lk(g_mu);
        if (msg) *msg = g_msg;
        g_state = 0;
    }
    return s;
}

int set_audio(const std::vector<unsigned long>& pids, float scalar, bool mute) {
    int changed = 0;
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioSessionManager2* mgr = nullptr;
    IAudioSessionEnumerator* ses = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en)) &&
        SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev)) &&
        SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr)) &&
        SUCCEEDED(mgr->GetSessionEnumerator(&ses))) {
        int n = 0;
        ses->GetCount(&n);
        for (int i = 0; i < n; ++i) {
            IAudioSessionControl* c = nullptr;
            if (FAILED(ses->GetSession(i, &c)) || !c) continue;
            IAudioSessionControl2* c2 = nullptr;
            if (SUCCEEDED(c->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&c2))) {
                DWORD pid = 0;
                if (SUCCEEDED(c2->GetProcessId(&pid))) {
                    for (unsigned long p : pids) if (p == pid) {
                        ISimpleAudioVolume* v = nullptr;
                        if (SUCCEEDED(c->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&v))) {
                            v->SetMasterVolume(scalar < 0.f ? 0.f : scalar > 1.f ? 1.f : scalar, nullptr);
                            v->SetMute(mute ? TRUE : FALSE, nullptr);
                            ++changed;
                            rel(v);
                        }
                        break;
                    }
                }
                rel(c2);
            }
            rel(c);
        }
    }
    rel(ses); rel(mgr); rel(dev); rel(en);
    if (SUCCEEDED(co)) CoUninitialize();
    return changed;
}

}  // namespace extmedia
