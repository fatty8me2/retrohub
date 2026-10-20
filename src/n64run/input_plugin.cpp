/* mupen64plus input plugin exported by n64run.exe itself (CoreAttachPlugin(M64PLUGIN_INPUT,
 * GetModuleHandle(NULL))). All controller data comes from the lockstep layer.
 *
 * Assumption: the core calls GetKeys(port) once per poll for each Present port, in port order
 * (mupen64plus 2.6.0 joybus: controller_read_buttons -> input.get_input per channel). The lockstep
 * sample index advances on the call for the lowest present port; other ports reuse that sample.
 * Controller paks are disabled (Plugin = PLUGIN_NONE), so ControllerCommand/ReadController (raw
 * mode only) are never used. */

#include <windows.h>

#include <cstdio>
#include <cstring>

// (M64P_PLUGIN_PROTOTYPES is deliberately not defined: we declare our own exports below)
#include "m64p_plugin.h"
#include "m64p_types.h"
#include "m64p_common.h"

#include "runstate.h"

namespace n64run { RunState g_run; }
using namespace n64run;

#define PLUGIN_EXPORT extern "C" __declspec(dllexport)

static void (*s_debug_cb)(void*, int, const char*) = nullptr;
static void* s_debug_ctx = nullptr;

static void plog(int level, const char* msg) {
    if (s_debug_cb) s_debug_cb(s_debug_ctx, level, msg);
}

PLUGIN_EXPORT m64p_error PluginStartup(m64p_dynlib_handle, void* context, void (*debug_cb)(void*, int, const char*)) {
    s_debug_cb = debug_cb;
    s_debug_ctx = context;
    BUTTONS b; std::memset(&b, 0, sizeof b);
    b.A_BUTTON = 1;
    if (b.Value != 0x80u) plog(M64MSG_ERROR, "n64run input: unexpected BUTTONS bit layout");
    return M64ERR_SUCCESS;
}

PLUGIN_EXPORT m64p_error PluginShutdown(void) {
    return M64ERR_SUCCESS;
}

PLUGIN_EXPORT m64p_error PluginGetVersion(m64p_plugin_type* type, int* version, int* api_version,
                                          const char** name, int* caps) {
    if (type) *type = M64PLUGIN_INPUT;
    if (version) *version = 0x010000;
    if (api_version) *api_version = 0x020100;
    if (name) *name = "n64run lockstep input";
    if (caps) *caps = 0;
    return M64ERR_SUCCESS;
}

PLUGIN_EXPORT void InitiateControllers(CONTROL_INFO info) {
    for (int i = 0; i < 4; i++) {
        info.Controls[i].Present = g_run.owner[i] >= 0 ? 1 : 0;
        info.Controls[i].RawData = 0;
        info.Controls[i].Plugin = PLUGIN_NONE;
        info.Controls[i].Type = CONT_TYPE_STANDARD;
    }
    char msg[128];
    std::snprintf(msg, sizeof msg, "n64run input: ports present = %d %d %d %d",
                  g_run.owner[0] >= 0, g_run.owner[1] >= 0, g_run.owner[2] >= 0, g_run.owner[3] >= 0);
    plog(M64MSG_INFO, msg);
}

PLUGIN_EXPORT void GetKeys(int control, BUTTONS* keys) {
    if (!keys) return;
    keys->Value = 0;
    if (control < 0 || control >= 4 || !g_run.lockstep) return;
    keys->Value = g_run.lockstep->get_keys(control);
}

PLUGIN_EXPORT void ControllerCommand(int, unsigned char*) {}
PLUGIN_EXPORT void ReadController(int, unsigned char*) {}
PLUGIN_EXPORT int RomOpen(void) { return 1; }
PLUGIN_EXPORT void RomClosed(void) {}
PLUGIN_EXPORT void SDL_KeyDown(int, int) {}
PLUGIN_EXPORT void SDL_KeyUp(int, int) {}
PLUGIN_EXPORT void RenderCallback(void) {}
