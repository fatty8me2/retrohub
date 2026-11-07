/* RetroHub addition (not part of Hookshot): the build without wgpu-native.

   RetroHub builds Hookshot with the Vulkan back end only (hookshot_vk.dll) and never uses a prebuilt wgpu-native, so
   gpu/render (the WebGPU renderer, its functions renamed wgr_* by rx_wgpu_names.h) is not compiled. host/rx_backend.c
   still names it as the fallback for when hookshot_vk.dll does not load or finds no usable Vulkan device: here that
   fallback logs why and reports failure instead of drawing. The rx_* debug hooks host/nn_hid.c's live input commands
   reach (!dump, !shots, !rdoc, !flasharm) belong to the WebGPU renderer and do nothing. */
#include <stdint.h>
#include "hos.h"
#include "../gpu/vk/vk_api.h"

int wgr_start(void *hwnd) {
    (void)hwnd;
    hos_log("rx: no renderer: the Vulkan back end (hookshot_vk.dll) did not start, and this build has no WebGPU fallback");
    return 0;
}
void wgr_resize(int width, int height) { (void)width, (void)height; }
const char *wgr_describe(void) { return "none (Vulkan only build)"; }
void wgr_stats(int64_t *draws, int64_t *skipped, int64_t *pipelines) {
    if (draws) *draws = 0;
    if (skipped) *skipped = 0;
    if (pipelines) *pipelines = 0;
}
void wgr_nvn_submit(const uint64_t *handles, int n) { (void)handles, (void)n; }
uint64_t wgr_nvn_fence(void) { return 0; }
uint64_t wgr_nvn_pushed(void) { return 0; }
uint64_t wgr_nvn_replayed(void) { return 0; }
int wgr_nvn_wait(uint64_t fence, uint64_t timeout_ns) { (void)fence, (void)timeout_ns; return 1; }
void wgr_wait_writebacks_reading(const void *p, uint64_t n) { (void)p, (void)n; }
void wgr_nvn_finish(void) {}
void wgr_nvn_present(const nvn_tex *t, const int crop[4]) { (void)t, (void)crop; }
void wgr_nvn_register_texture(int id, const nvn_tex *t, const nvn_view *v) { (void)id, (void)t, (void)v; }
void wgr_nvn_register_sampler(int id, const nvn_samp_par *p) { (void)id, (void)p; }
void wgr_nvn_set_texture_pool_size(int n) { (void)n; }
void wgr_nvn_texture_released(const nvn_tex *t) { (void)t; }
void wgr_nvn_program_released(const nvn_prog *p) { (void)p; }

void rx_flash_dark_arm(void) {}
void rx_request_frame_dump(void) {}
void rx_request_rdoc_capture(int frames) { (void)frames; }
void rx_set_screenshot_ms(int64_t ms) { (void)ms; }
