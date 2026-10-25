#include "headless.h"

#include <windows.h>

#include <SDL.h>

#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <vector>

#include "m64p_types.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "../retrorun/third_party/stb_image_write.h"

namespace n64run {
namespace {

SDL_Window* g_win = nullptr;
SDL_GLContext g_ctx = nullptr;
int g_w = 640, g_h = 480;

ShotMode g_mode = ShotMode::Off;
unsigned g_swaps = 0;
int g_shot_w = 0, g_shot_h = 0;
unsigned g_grabs = 0;
std::vector<uint8_t> g_shot;   // RGBA, top-down

typedef void(__stdcall* PFN_ReadPixels)(int, int, int, int, unsigned, unsigned, void*);
typedef void(__stdcall* PFN_ReadBuffer)(unsigned);
typedef void(__stdcall* PFN_PixelStorei)(unsigned, int);
typedef void(__stdcall* PFN_GetIntegerv)(unsigned, int*);
typedef void(__stdcall* PFN_BindFramebuffer)(unsigned, unsigned);

m64p_error vx_init() {
    if (!SDL_WasInit(SDL_INIT_VIDEO) && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) return M64ERR_SYSTEM_FAIL;
    return M64ERR_SUCCESS;
}
m64p_error vx_init_mode(m64p_render_mode m) { return m == M64P_RENDER_OPENGL ? vx_init() : M64ERR_UNSUPPORTED; }

m64p_error vx_quit() {
    if (g_ctx) { SDL_GL_DeleteContext(g_ctx); g_ctx = nullptr; }
    if (g_win) { SDL_DestroyWindow(g_win); g_win = nullptr; }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return M64ERR_SUCCESS;
}

m64p_error vx_list_modes(m64p_2d_size* sizes, int* n) {
    if (!sizes || !n || *n < 1) return M64ERR_INPUT_ASSERT;
    sizes[0].uiWidth = (unsigned)g_w; sizes[0].uiHeight = (unsigned)g_h; *n = 1;
    return M64ERR_SUCCESS;
}
m64p_error vx_list_rates(m64p_2d_size, int* n, int* rates) {
    if (!n || !rates || *n < 1) return M64ERR_INPUT_ASSERT;
    rates[0] = 60; *n = 1;
    return M64ERR_SUCCESS;
}

m64p_error vx_set_mode(int w, int h, int, int, int) {
    if (w < 16) w = 16;
    if (h < 16) h = 16;
    g_w = w; g_h = h;
    if (g_win) { SDL_SetWindowSize(g_win, w, h); return M64ERR_SUCCESS; }
    // HIDDEN: the window is never shown, so nothing appears on the desktop or taskbar.
    g_win = SDL_CreateWindow("n64run headless", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w, h,
                             SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!g_win) { std::fprintf(stderr, "[headless] SDL_CreateWindow: %s\n", SDL_GetError()); return M64ERR_SYSTEM_FAIL; }
    g_ctx = SDL_GL_CreateContext(g_win);
    if (!g_ctx) {
        std::fprintf(stderr, "[headless] SDL_GL_CreateContext: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_win); g_win = nullptr;
        return M64ERR_SYSTEM_FAIL;
    }
    SDL_GL_MakeCurrent(g_win, g_ctx);
    SDL_GL_SetSwapInterval(0);   // never wait for vsync: run as fast as possible
    return M64ERR_SUCCESS;
}
m64p_error vx_set_mode_rate(int w, int h, int, int bpp, int mode, int flags) { return vx_set_mode(w, h, bpp, mode, flags); }

m64p_function vx_gl_proc(const char* name) { return (m64p_function)SDL_GL_GetProcAddress(name); }

bool map_attr(m64p_GLattr a, SDL_GLattr* out) {
    switch (a) {
        case M64P_GL_DOUBLEBUFFER: *out = SDL_GL_DOUBLEBUFFER; return true;
        case M64P_GL_BUFFER_SIZE: *out = SDL_GL_BUFFER_SIZE; return true;
        case M64P_GL_DEPTH_SIZE: *out = SDL_GL_DEPTH_SIZE; return true;
        case M64P_GL_RED_SIZE: *out = SDL_GL_RED_SIZE; return true;
        case M64P_GL_GREEN_SIZE: *out = SDL_GL_GREEN_SIZE; return true;
        case M64P_GL_BLUE_SIZE: *out = SDL_GL_BLUE_SIZE; return true;
        case M64P_GL_ALPHA_SIZE: *out = SDL_GL_ALPHA_SIZE; return true;
        case M64P_GL_MULTISAMPLEBUFFERS: *out = SDL_GL_MULTISAMPLEBUFFERS; return true;
        case M64P_GL_MULTISAMPLESAMPLES: *out = SDL_GL_MULTISAMPLESAMPLES; return true;
        case M64P_GL_CONTEXT_MAJOR_VERSION: *out = SDL_GL_CONTEXT_MAJOR_VERSION; return true;
        case M64P_GL_CONTEXT_MINOR_VERSION: *out = SDL_GL_CONTEXT_MINOR_VERSION; return true;
        case M64P_GL_CONTEXT_PROFILE_MASK: *out = SDL_GL_CONTEXT_PROFILE_MASK; return true;
        default: return false;   // SWAP_CONTROL is handled by us (always 0)
    }
}

m64p_error vx_gl_set_attr(m64p_GLattr a, int v) {
    if (a == M64P_GL_SWAP_CONTROL) return M64ERR_SUCCESS;
    SDL_GLattr sa;
    if (!map_attr(a, &sa)) return M64ERR_INPUT_INVALID;
    if (a == M64P_GL_CONTEXT_PROFILE_MASK) {
        v = v == M64P_GL_CONTEXT_PROFILE_CORE ? SDL_GL_CONTEXT_PROFILE_CORE
          : v == M64P_GL_CONTEXT_PROFILE_ES ? SDL_GL_CONTEXT_PROFILE_ES : SDL_GL_CONTEXT_PROFILE_COMPATIBILITY;
    }
    return SDL_GL_SetAttribute(sa, v) == 0 ? M64ERR_SUCCESS : M64ERR_SYSTEM_FAIL;
}

m64p_error vx_gl_get_attr(m64p_GLattr a, int* v) {
    if (!v) return M64ERR_INPUT_ASSERT;
    if (a == M64P_GL_SWAP_CONTROL) { *v = 0; return M64ERR_SUCCESS; }
    SDL_GLattr sa;
    if (!map_attr(a, &sa)) return M64ERR_INPUT_INVALID;
    int r = 0;
    if (SDL_GL_GetAttribute(sa, &r) != 0) return M64ERR_SYSTEM_FAIL;
    if (a == M64P_GL_CONTEXT_PROFILE_MASK)
        r = r == SDL_GL_CONTEXT_PROFILE_CORE ? M64P_GL_CONTEXT_PROFILE_CORE
          : r == SDL_GL_CONTEXT_PROFILE_ES ? M64P_GL_CONTEXT_PROFILE_ES : M64P_GL_CONTEXT_PROFILE_COMPATIBILITY;
    *v = r;
    return M64ERR_SUCCESS;
}

void grab_frame() {
    auto glReadPixels_ = (PFN_ReadPixels)SDL_GL_GetProcAddress("glReadPixels");
    auto glReadBuffer_ = (PFN_ReadBuffer)SDL_GL_GetProcAddress("glReadBuffer");
    auto glPixelStorei_ = (PFN_PixelStorei)SDL_GL_GetProcAddress("glPixelStorei");
    auto glGetIntegerv_ = (PFN_GetIntegerv)SDL_GL_GetProcAddress("glGetIntegerv");
    auto glBindFramebuffer_ = (PFN_BindFramebuffer)SDL_GL_GetProcAddress("glBindFramebuffer");
    if (!glReadPixels_ || !glReadBuffer_ || !glPixelStorei_) return;
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(g_win, &w, &h);
    if (w <= 0 || h <= 0) return;
    int prev_fb = 0;
    if (glGetIntegerv_ && glBindFramebuffer_) {
        glGetIntegerv_(0x8CAA /*READ_FRAMEBUFFER_BINDING*/, &prev_fb);
        glBindFramebuffer_(0x8CA8 /*READ_FRAMEBUFFER*/, 0);
    }
    int prev_align = 4;
    if (glGetIntegerv_) glGetIntegerv_(0x0D05 /*PACK_ALIGNMENT*/, &prev_align);
    glPixelStorei_(0x0D05, 1);
    glReadBuffer_(0x0405 /*BACK*/);
    std::vector<uint8_t> raw((size_t)w * h * 4);
    glReadPixels_(0, 0, w, h, 0x1908 /*RGBA*/, 0x1401 /*UNSIGNED_BYTE*/, raw.data());
    glPixelStorei_(0x0D05, prev_align);
    if (glBindFramebuffer_ && glGetIntegerv_) glBindFramebuffer_(0x8CA8, (unsigned)prev_fb);
    g_shot.resize(raw.size());
    for (int y = 0; y < h; y++)   // GL origin is bottom-left; store top-down
        std::memcpy(&g_shot[(size_t)y * w * 4], &raw[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
    g_shot_w = w; g_shot_h = h;
    g_grabs++;
}

m64p_error vx_gl_swap() {
    if (!g_win) return M64ERR_NOT_INIT;
    g_swaps++;
    if (g_mode == ShotMode::Armed) { grab_frame(); g_mode = ShotMode::Off; }
    else if (g_mode == ShotMode::Periodic && g_swaps % 30 == 0) grab_frame();
    SDL_GL_SwapWindow(g_win);
    return M64ERR_SUCCESS;
}

m64p_error vx_caption(const char*) { return M64ERR_SUCCESS; }
m64p_error vx_toggle_fs() { return M64ERR_SUCCESS; }
m64p_error vx_resize(int w, int h) { g_w = w; g_h = h; if (g_win) SDL_SetWindowSize(g_win, w, h); return M64ERR_SUCCESS; }
uint32_t vx_default_fb() { return 0; }
m64p_error vx_vk_surface(void**, void*) { return M64ERR_UNSUPPORTED; }
m64p_error vx_vk_ext(const char**[], uint32_t*) { return M64ERR_UNSUPPORTED; }

}  // namespace

bool headless_install_vidext(ptr_CoreOverrideVidExt override_fn) {
    if (!override_fn) return false;
    m64p_video_extension_functions f;
    std::memset(&f, 0, sizeof f);
    f.Functions = 17;
    f.VidExtFuncInit = vx_init;
    f.VidExtFuncQuit = vx_quit;
    f.VidExtFuncListModes = vx_list_modes;
    f.VidExtFuncListRates = vx_list_rates;
    f.VidExtFuncSetMode = vx_set_mode;
    f.VidExtFuncSetModeWithRate = vx_set_mode_rate;
    f.VidExtFuncGLGetProc = vx_gl_proc;
    f.VidExtFuncGLSetAttr = vx_gl_set_attr;
    f.VidExtFuncGLGetAttr = vx_gl_get_attr;
    f.VidExtFuncGLSwapBuf = vx_gl_swap;
    f.VidExtFuncSetCaption = vx_caption;
    f.VidExtFuncToggleFS = vx_toggle_fs;
    f.VidExtFuncResizeWindow = vx_resize;
    f.VidExtFuncGLGetDefaultFramebuffer = vx_default_fb;
    f.VidExtFuncInitWithRenderMode = vx_init_mode;
    f.VidExtFuncVKGetSurface = vx_vk_surface;
    f.VidExtFuncVKGetInstanceExtensions = vx_vk_ext;
    return override_fn(&f) == M64ERR_SUCCESS;
}

void headless_set_shot_mode(ShotMode m) { g_mode = m; }

unsigned headless_shot_serial() { return g_grabs; }

bool headless_write_screenshot(const std::string& path, int max_w) {
    if (g_shot.empty() || g_shot_w <= 0 || g_shot_h <= 0) return false;
    int ow = g_shot_w, oh = g_shot_h;
    if (max_w > 0 && g_shot_w > max_w) { ow = max_w; oh = std::max(1, (int)((long long)g_shot_h * max_w / g_shot_w)); }
    std::vector<uint8_t> rgb((size_t)ow * oh * 3);
    for (int y = 0; y < oh; y++) {
        int y0 = (int)((long long)y * g_shot_h / oh), y1 = std::max(y0 + 1, (int)((long long)(y + 1) * g_shot_h / oh));
        for (int x = 0; x < ow; x++) {
            int x0 = (int)((long long)x * g_shot_w / ow), x1 = std::max(x0 + 1, (int)((long long)(x + 1) * g_shot_w / ow));
            unsigned acc[3] = {0, 0, 0}, n = 0;
            for (int yy = y0; yy < y1 && yy < g_shot_h; yy++)
                for (int xx = x0; xx < x1 && xx < g_shot_w; xx++) {
                    const uint8_t* q = &g_shot[((size_t)yy * g_shot_w + xx) * 4];
                    acc[0] += q[0]; acc[1] += q[1]; acc[2] += q[2]; n++;
                }
            if (!n) n = 1;
            uint8_t* d = &rgb[((size_t)y * ow + x) * 3];
            d[0] = (uint8_t)(acc[0] / n); d[1] = (uint8_t)(acc[1] / n); d[2] = (uint8_t)(acc[2] / n);
        }
    }
    std::filesystem::path fp = std::filesystem::u8path(path);
    std::error_code ec;
    if (fp.has_parent_path()) std::filesystem::create_directories(fp.parent_path(), ec);
    FILE* fh = _wfopen(fp.c_str(), L"wb");
    if (!fh) return false;
    auto to_file = [](void* ctx, void* data, int size) { fwrite(data, 1, (size_t)size, (FILE*)ctx); };
    bool bmp = fp.extension() == ".bmp" || fp.extension() == ".BMP";
    int ok = bmp ? stbi_write_bmp_to_func(to_file, fh, ow, oh, 3, rgb.data())
                 : stbi_write_png_to_func(to_file, fh, ow, oh, 3, rgb.data(), ow * 3);
    fclose(fh);
    return ok != 0;
}

}  // namespace n64run
