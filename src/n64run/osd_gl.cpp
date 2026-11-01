#include "osd_gl.h"

#include <windows.h>

#include <GL/gl.h>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "osd.h"

namespace n64run {
namespace {

// ---- GL 3.3 entry points (opengl32.dll only exports 1.1) and constants -------------------------------------------------------------
typedef char GLchar_;
typedef GLuint(APIENTRY* PFN_CreateShader)(GLenum);
typedef void(APIENTRY* PFN_ShaderSource)(GLuint, GLsizei, const GLchar_* const*, const GLint*);
typedef void(APIENTRY* PFN_CompileShader)(GLuint);
typedef void(APIENTRY* PFN_GetShaderiv)(GLuint, GLenum, GLint*);
typedef void(APIENTRY* PFN_GetInfoLog)(GLuint, GLsizei, GLsizei*, GLchar_*);
typedef GLuint(APIENTRY* PFN_CreateProgram)(void);
typedef void(APIENTRY* PFN_AttachShader)(GLuint, GLuint);
typedef void(APIENTRY* PFN_LinkProgram)(GLuint);
typedef void(APIENTRY* PFN_GetProgramiv)(GLuint, GLenum, GLint*);
typedef void(APIENTRY* PFN_DeleteShader)(GLuint);
typedef void(APIENTRY* PFN_UseProgram)(GLuint);
typedef GLint(APIENTRY* PFN_GetUniformLocation)(GLuint, const GLchar_*);
typedef void(APIENTRY* PFN_Uniform1i)(GLint, GLint);
typedef void(APIENTRY* PFN_GenVertexArrays)(GLsizei, GLuint*);
typedef void(APIENTRY* PFN_BindVertexArray)(GLuint);
typedef void(APIENTRY* PFN_ActiveTexture)(GLenum);
typedef void(APIENTRY* PFN_BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
typedef void(APIENTRY* PFN_BlendEquationSeparate)(GLenum, GLenum);
typedef void(APIENTRY* PFN_BindFramebuffer)(GLenum, GLuint);
typedef void(APIENTRY* PFN_BindSampler)(GLuint, GLuint);
typedef void(APIENTRY* PFN_BindBuffer)(GLenum, GLuint);

enum : GLenum {
    kFragmentShader = 0x8B30, kVertexShader = 0x8B31, kCompileStatus = 0x8B81, kLinkStatus = 0x8B82, kInfoLogLength = 0x8B84,
    kCurrentProgram = 0x8B8D, kVertexArrayBinding = 0x85B5, kActiveTexture = 0x84E0, kTexture0 = 0x84C0, kTextureBinding2D = 0x8069,
    kSamplerBinding = 0x8919, kDrawFramebufferBinding = 0x8CA6, kDrawFramebuffer = 0x8CA9, kFramebuffer = 0x8D40,
    kPixelUnpackBufferBinding = 0x88EF, kPixelUnpackBuffer = 0x88EC,
    kUnpackRowLength = 0x0CF2, kUnpackSkipRows = 0x0CF3, kUnpackSkipPixels = 0x0CF4, kUnpackAlignment = 0x0CF5,
    kBlendSrcRgb = 0x80C9, kBlendDstRgb = 0x80C8, kBlendSrcAlpha = 0x80CB, kBlendDstAlpha = 0x80CA, kBlendEqRgb = 0x8009, kBlendEqAlpha = 0x883D,
    kFuncAdd = 0x8006, kStencilTest = 0x0B90, kDither = 0x0BD0, kFramebufferSrgb = 0x8DB9, kMultisample = 0x809D,
    kSampleAlphaToCoverage = 0x809E, kColorWritemask = 0x0C23, kClampToEdge = 0x812F, kRgba8 = 0x8058, kBgra = 0x80E1,
    kContextProfileMask = 0x9126
};

struct Gl {
    PFN_CreateShader CreateShader = nullptr;
    PFN_ShaderSource ShaderSource = nullptr;
    PFN_CompileShader CompileShader = nullptr;
    PFN_GetShaderiv GetShaderiv = nullptr;
    PFN_GetInfoLog GetShaderInfoLog = nullptr, GetProgramInfoLog = nullptr;
    PFN_CreateProgram CreateProgram = nullptr;
    PFN_AttachShader AttachShader = nullptr;
    PFN_LinkProgram LinkProgram = nullptr;
    PFN_GetProgramiv GetProgramiv = nullptr;
    PFN_DeleteShader DeleteShader = nullptr;
    PFN_UseProgram UseProgram = nullptr;
    PFN_GetUniformLocation GetUniformLocation = nullptr;
    PFN_Uniform1i Uniform1i = nullptr;
    PFN_GenVertexArrays GenVertexArrays = nullptr;
    PFN_BindVertexArray BindVertexArray = nullptr;
    PFN_ActiveTexture ActiveTexture = nullptr;
    PFN_BlendFuncSeparate BlendFuncSeparate = nullptr;
    PFN_BlendEquationSeparate BlendEquationSeparate = nullptr;
    PFN_BindFramebuffer BindFramebuffer = nullptr;
    PFN_BindSampler BindSampler = nullptr;
    PFN_BindBuffer BindBuffer = nullptr;
};

std::mutex g_mx;                       // guards the info below (osd_gl_info() is read from other threads)
OsdGlInfo g_info;
void (*g_log)(const char*) = nullptr;
Gl g_gl;
GLuint g_prog = 0, g_vao = 0, g_tex = 0;
GLint g_loc_tex = -1;
int g_tex_w = 0, g_tex_h = 0;
bool g_tried = false, g_logged_first = false;
unsigned g_err_logged = 0;
osd::Layer g_layer;

void say(const char* fmt, ...) {
    char b[300];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (g_log) g_log(b);
}

template <class T> bool load(T* out, const char* name) {
    *out = reinterpret_cast<T>(SDL_GL_GetProcAddress(name));
    return *out != nullptr;
}

bool load_gl() {
    return load(&g_gl.CreateShader, "glCreateShader") && load(&g_gl.ShaderSource, "glShaderSource") && load(&g_gl.CompileShader, "glCompileShader") &&
           load(&g_gl.GetShaderiv, "glGetShaderiv") && load(&g_gl.GetShaderInfoLog, "glGetShaderInfoLog") && load(&g_gl.CreateProgram, "glCreateProgram") &&
           load(&g_gl.AttachShader, "glAttachShader") && load(&g_gl.LinkProgram, "glLinkProgram") && load(&g_gl.GetProgramiv, "glGetProgramiv") &&
           load(&g_gl.GetProgramInfoLog, "glGetProgramInfoLog") && load(&g_gl.DeleteShader, "glDeleteShader") && load(&g_gl.UseProgram, "glUseProgram") &&
           load(&g_gl.GetUniformLocation, "glGetUniformLocation") && load(&g_gl.Uniform1i, "glUniform1i") && load(&g_gl.GenVertexArrays, "glGenVertexArrays") &&
           load(&g_gl.BindVertexArray, "glBindVertexArray") && load(&g_gl.ActiveTexture, "glActiveTexture") && load(&g_gl.BlendFuncSeparate, "glBlendFuncSeparate") &&
           load(&g_gl.BlendEquationSeparate, "glBlendEquationSeparate") && load(&g_gl.BindFramebuffer, "glBindFramebuffer") && load(&g_gl.BindSampler, "glBindSampler") &&
           load(&g_gl.BindBuffer, "glBindBuffer");
}

const char* kVertex =
    "#version 330 core\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"   // (0,0) (2,0) (0,2): one triangle covering the screen
    "    uv = vec2(p.x, 1.0 - p.y);\n"                                               // layer row 0 is the TOP row
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
const char* kFragment =
    "#version 330 core\n"
    "in vec2 uv;\n"
    "uniform sampler2D tex;\n"
    "out vec4 color;\n"
    "void main() { color = texture(tex, uv); }\n";

GLuint compile(GLenum type, const char* src) {
    GLuint s = g_gl.CreateShader(type);
    g_gl.ShaderSource(s, 1, &src, nullptr);
    g_gl.CompileShader(s);
    GLint ok = 0;
    g_gl.GetShaderiv(s, kCompileStatus, &ok);
    if (!ok) {
        char log[400] = {0};
        g_gl.GetShaderInfoLog(s, sizeof log - 1, nullptr, log);
        say("osd-gl: %s shader failed: %s", type == kVertexShader ? "vertex" : "fragment", log);
        g_gl.DeleteShader(s);
        return 0;
    }
    return s;
}

void drain_errors() { for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; i++) {} }

/* Everything the overlay changes, put back afterwards (the plugin caches GL state and would misrender the next frame otherwise). */
struct Saved {
    GLint prog = 0, vao = 0, active = 0, tex0 = 0, sampler0 = 0, draw_fb = 0, unpack_buf = 0, row = 0, align = 0, skip_px = 0, skip_rows = 0;
    GLint bsrc_rgb = 0, bdst_rgb = 0, bsrc_a = 0, bdst_a = 0, beq_rgb = 0, beq_a = 0;
    GLboolean blend = 0, depth = 0, cull = 0, scissor = 0, stencil = 0, dither = 0, srgb = 0, msaa = 0, a2c = 0;
    GLboolean mask[4] = {1, 1, 1, 1};
    GLint vp[4] = {0, 0, 0, 0};

    void save() {
        glGetIntegerv(kCurrentProgram, &prog);
        glGetIntegerv(kVertexArrayBinding, &vao);
        glGetIntegerv(kDrawFramebufferBinding, &draw_fb);
        glGetIntegerv(kActiveTexture, &active);
        g_gl.ActiveTexture(kTexture0);
        glGetIntegerv(kTextureBinding2D, &tex0);
        glGetIntegerv(kSamplerBinding, &sampler0);
        glGetIntegerv(kPixelUnpackBufferBinding, &unpack_buf);
        glGetIntegerv(kUnpackRowLength, &row);
        glGetIntegerv(kUnpackAlignment, &align);
        glGetIntegerv(kUnpackSkipPixels, &skip_px);
        glGetIntegerv(kUnpackSkipRows, &skip_rows);
        glGetIntegerv(kBlendSrcRgb, &bsrc_rgb); glGetIntegerv(kBlendDstRgb, &bdst_rgb);
        glGetIntegerv(kBlendSrcAlpha, &bsrc_a); glGetIntegerv(kBlendDstAlpha, &bdst_a);
        glGetIntegerv(kBlendEqRgb, &beq_rgb); glGetIntegerv(kBlendEqAlpha, &beq_a);
        blend = glIsEnabled(GL_BLEND); depth = glIsEnabled(GL_DEPTH_TEST); cull = glIsEnabled(GL_CULL_FACE); scissor = glIsEnabled(GL_SCISSOR_TEST);
        stencil = glIsEnabled(kStencilTest); dither = glIsEnabled(kDither); srgb = glIsEnabled(kFramebufferSrgb); msaa = glIsEnabled(kMultisample);
        a2c = glIsEnabled(kSampleAlphaToCoverage);
        glGetBooleanv(kColorWritemask, mask);
        glGetIntegerv(GL_VIEWPORT, vp);
    }
    void restore() const {
        g_gl.UseProgram((GLuint)prog);
        g_gl.BindVertexArray((GLuint)vao);
        g_gl.BindFramebuffer(kDrawFramebuffer, (GLuint)draw_fb);
        g_gl.ActiveTexture(kTexture0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)tex0);
        g_gl.BindSampler(0, (GLuint)sampler0);
        g_gl.ActiveTexture((GLenum)active);
        g_gl.BindBuffer(kPixelUnpackBuffer, (GLuint)unpack_buf);
        glPixelStorei(kUnpackRowLength, row); glPixelStorei(kUnpackAlignment, align);
        glPixelStorei(kUnpackSkipPixels, skip_px); glPixelStorei(kUnpackSkipRows, skip_rows);
        g_gl.BlendFuncSeparate((GLenum)bsrc_rgb, (GLenum)bdst_rgb, (GLenum)bsrc_a, (GLenum)bdst_a);
        g_gl.BlendEquationSeparate((GLenum)beq_rgb, (GLenum)beq_a);
        auto set = [](GLenum cap, GLboolean on) { if (on) glEnable(cap); else glDisable(cap); };
        set(GL_BLEND, blend); set(GL_DEPTH_TEST, depth); set(GL_CULL_FACE, cull); set(GL_SCISSOR_TEST, scissor);
        set(kStencilTest, stencil); set(kDither, dither); set(kFramebufferSrgb, srgb); set(kMultisample, msaa); set(kSampleAlphaToCoverage, a2c);
        glColorMask(mask[0], mask[1], mask[2], mask[3]);
        glViewport(vp[0], vp[1], vp[2], vp[3]);
    }
};

bool init_gl() {
    g_tried = true;
    drain_errors();
    const char* ver = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const char* ren = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    GLint profile = 0;
    glGetIntegerv(kContextProfileMask, &profile);
    char gl[200];
    std::snprintf(gl, sizeof gl, "%s | %s | profile mask 0x%x", ver ? ver : "?", ren ? ren : "?", (unsigned)profile);
    { std::lock_guard<std::mutex> lk(g_mx); g_info.gl = gl; }
    if (!load_gl()) { say("osd-gl: GL 3.3 entry points missing (%s); the caption carries the OSD", gl); return false; }
    GLuint vs = compile(kVertexShader, kVertex), fs = vs ? compile(kFragmentShader, kFragment) : 0;
    if (!vs || !fs) { say("osd-gl: shaders do not compile on this context (%s); the caption carries the OSD", gl); return false; }
    g_prog = g_gl.CreateProgram();
    g_gl.AttachShader(g_prog, vs);
    g_gl.AttachShader(g_prog, fs);
    g_gl.LinkProgram(g_prog);
    g_gl.DeleteShader(vs);
    g_gl.DeleteShader(fs);
    GLint ok = 0;
    g_gl.GetProgramiv(g_prog, kLinkStatus, &ok);
    if (!ok) {
        char log[400] = {0};
        g_gl.GetProgramInfoLog(g_prog, sizeof log - 1, nullptr, log);
        say("osd-gl: program link failed: %s", log);
        return false;
    }
    g_loc_tex = g_gl.GetUniformLocation(g_prog, "tex");
    g_gl.GenVertexArrays(1, &g_vao);   // generating binds nothing: the plugin's own VAO stays current
    glGenTextures(1, &g_tex);
    if (glGetError() != GL_NO_ERROR) { say("osd-gl: GL error while creating the overlay objects (%s)", gl); return false; }
    say("osd-gl: ready on %s", gl);
    return true;
}

}  // namespace

void osd_gl_configure(bool windowed) {
    const char* e = std::getenv("N64RUN_OSD_GL");
    bool on = windowed;                                         // default: a real window
    if (e && e[0] == '0') on = false;
    else if (e && e[0] == '1') on = true;                       // QA: --headless reads the overlay back from the screenshot
    std::lock_guard<std::mutex> lk(g_mx);
    g_info.enabled = on;
}

void osd_gl_set_logger(void (*log)(const char* line)) { g_log = log; }

bool osd_gl_enabled() { std::lock_guard<std::mutex> lk(g_mx); return g_info.enabled && !g_info.failed; }

OsdGlInfo osd_gl_info() { std::lock_guard<std::mutex> lk(g_mx); return g_info; }

void osd_gl_render() {
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (!g_info.enabled || g_info.failed) return;
    }
    if (!SDL_GL_GetCurrentContext() && !wglGetCurrentContext()) return;
    // The window's drawable size: SDL knows the window the core created for the plugin; the GL viewport is the fallback.
    int ww = 0, wh = 0;
    if (SDL_Window* w = SDL_GL_GetCurrentWindow()) SDL_GL_GetDrawableSize(w, &ww, &wh);
    if (ww <= 0 || wh <= 0) {
        GLint vp[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_VIEWPORT, vp);
        ww = vp[0] + vp[2]; wh = vp[1] + vp[3];
    }
    if (ww < 16 || wh < 16 || ww > 16384 || wh > 16384) return;
    const int s = std::max(1, wh / 240);
    const int lw = std::max(1, ww / s), lh = std::max(1, wh / s);
    bool changed = false;
    if (!osd::render(&g_layer, lw, lh, &changed)) return;   // nothing on screen: the common case costs one mutex and a few compares

    if (!g_tried) {
        if (!init_gl()) {
            std::lock_guard<std::mutex> lk(g_mx);
            g_info.failed = true;
            return;
        }
    }
    drain_errors();
    Saved st;
    st.save();
    g_gl.BindFramebuffer(kDrawFramebuffer, 0);
    glViewport(0, 0, ww, wh);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST); glDisable(kStencilTest);
    glDisable(kDither); glDisable(kFramebufferSrgb); glDisable(kMultisample); glDisable(kSampleAlphaToCoverage);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_BLEND);
    g_gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    g_gl.BlendEquationSeparate(kFuncAdd, kFuncAdd);
    g_gl.BindBuffer(kPixelUnpackBuffer, 0);
    glPixelStorei(kUnpackRowLength, 0); glPixelStorei(kUnpackAlignment, 4);
    glPixelStorei(kUnpackSkipPixels, 0); glPixelStorei(kUnpackSkipRows, 0);
    g_gl.ActiveTexture(kTexture0);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    g_gl.BindSampler(0, 0);
    bool uploaded = false;
    if (g_tex_w != lw || g_tex_h != lh) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kClampToEdge);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kClampToEdge);
        glTexImage2D(GL_TEXTURE_2D, 0, kRgba8, lw, lh, 0, kBgra, GL_UNSIGNED_BYTE, g_layer.px.data());   // ARGB8888 little-endian = BGRA bytes
        g_tex_w = lw; g_tex_h = lh;
        uploaded = true;
    } else if (changed) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, lw, lh, kBgra, GL_UNSIGNED_BYTE, g_layer.px.data());
        uploaded = true;
    }
    g_gl.UseProgram(g_prog);
    g_gl.Uniform1i(g_loc_tex, 0);
    g_gl.BindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    st.restore();
    GLenum err = glGetError();
    int errors = 0;
    for (; err != GL_NO_ERROR && errors < 16; err = glGetError()) { errors++; if (g_err_logged++ < 3) say("osd-gl: GL error 0x%x while drawing the overlay", (unsigned)err); }
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_info.draws++;
        if (uploaded) g_info.uploads++;
        g_info.win_w = ww; g_info.win_h = wh; g_info.layer_w = lw; g_info.layer_h = lh;
    }
    if (!g_logged_first) { g_logged_first = true; say("osd-gl: first overlay frame drawn (window %dx%d, layer %dx%d, x%d)", ww, wh, lw, lh, s); }
}

}  // namespace n64run
