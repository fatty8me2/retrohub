/* mgba_link_libretro: N (2..4) Game Boy Advance consoles wired together by mGBA's own lockstep SIO driver, inside ONE
 * deterministic libretro core. retrorun treats it like SameBoy's dual-GB mode (retrorun --link): the frontend lockstep-syncs
 * the inputs, every PC simulates all consoles, so the emulated link cable is part of the shared simulation.
 *
 * Copyright: this file is MPL-2.0 (it links mGBA, (c) Jeffrey Pfau, MPL-2.0 - see THIRD_PARTY.md). It borrows the structure
 * of mGBA's libretro.c and the scheduling rules of mGBA's Qt MultiplayerController, which drives GBASIOLockstep from one
 * thread per console; here the consoles are stepped cooperatively from a single thread in a fixed order, which makes the
 * whole thing deterministic (no wall clock, no thread races).
 *
 * Contents / ports: content i = console i = input port i.  Memory ids use SameBoy's layout so retrorun's link code needs no
 * change: console n (1-based) save RAM = ((2n-1)<<8)|0, RTC = (2n<<8)|1.  Video is the consoles side by side (240*N x 160),
 * audio is the console picked by option mgbalink_audio_output. */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <mgba-util/common.h>
#include <mgba-util/vfs.h>
#include <mgba/core/blip_buf.h>
#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/interface.h>
#include <mgba/core/log.h>
#include <mgba/core/timing.h>
#include <mgba/gba/core.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/renderers/video-software.h>
#include <mgba/internal/gba/sio.h>
#include <mgba/internal/gba/sio/lockstep.h>

#include <stdio.h>
#include "libretro.h"

static int dbg;
static unsigned long long n_wait, n_transfers;

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif

#define MAXN 4
#define W 240
#define H 160
#define STRIDE 256            /* pixels per row in each console's own buffer */
#define SAMPLE_RATE 32768

static retro_environment_t env_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_cb;
static retro_audio_sample_t audio1_cb;
static retro_input_poll_t poll_cb;
static retro_input_state_t input_cb;

static int n_cons;
static struct mCore* cores[MAXN];
static color_t* vbuf[MAXN];             /* per-console 256x160 (mGBA writes STRIDE pixels per row) */
static uint16_t* out_frame;             /* side by side composite, RGB565 */
static uint8_t* sav[MAXN];              /* per-console save buffer (SIZE_CART_FLASH1M) */
static bool deferred_setup;
static int audio_sel;
static int16_t* abuf;
static size_t abuf_cap;
static struct VFile* roms[MAXN];

/* ---- lockstep scheduling state (cooperative port of mGBA Qt's MultiplayerController) ---- */
static struct GBASIOLockstep lockstep;
static struct GBASIOLockstepNode nodes[MAXN];
struct Player { int32_t cycles_posted; int awake; unsigned wait_mask; };
static struct Player pl[MAXN];
static unsigned long long frame_no;

static const unsigned keymap[10] = {
    RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_START,
    RETRO_DEVICE_ID_JOYPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN,
    RETRO_DEVICE_ID_JOYPAD_R, RETRO_DEVICE_ID_JOYPAD_L,
};

static void ls_lock(struct mLockstep* l) { (void) l; }
static void ls_unlock(struct mLockstep* l) { (void) l; }

/* Wake/sleep = a flag only; the scheduler below honours it after the console's current event returns (same as the Qt
 * version, where mCoreThreadWaitFromThread only raises a request the thread sees once ARMRunLoop exits). */
static bool ls_signal(struct mLockstep* l, unsigned mask) {
    (void) l;
    struct Player* p = &pl[0];
    bool woke = false;
    p->wait_mask &= ~mask;
    if (!p->wait_mask && p->awake < 1) {
        p->awake = 1;
        woke = true;
    }
    return woke;
}

static bool ls_wait(struct mLockstep* l, unsigned mask) {
    (void) l;
    struct Player* p = &pl[0];
    p->wait_mask |= mask; n_wait++;
    if (p->awake > 0) {
        p->awake = 0;
    }
    return true;
}

static void ls_add_cycles(struct mLockstep* l, int id, int32_t cycles) {
    (void) l;
    if (cycles < 0) {
        abort();
    }
    if (!id) {
        for (int i = 1; i < n_cons; ++i) {
            struct Player* p = &pl[i];
            if (nodes[i].d.p->mode > SIO_MULTI) {
                continue;
            }
            p->cycles_posted += cycles;
            if (p->awake < 1) {
                nodes[i].nextEvent += p->cycles_posted;
            }
            p->awake = 1;
        }
    } else {
        pl[id].cycles_posted += cycles;
    }
}

static int32_t ls_use_cycles(struct mLockstep* l, int id, int32_t cycles) {
    (void) l;
    struct Player* p = &pl[id];
    p->cycles_posted -= cycles;
    if (p->cycles_posted <= 0) {
        p->awake = 0;
    }
    return p->cycles_posted;
}

static int32_t ls_unused_cycles(struct mLockstep* l, int id) {
    (void) l;
    return pl[id].cycles_posted;
}

static void ls_unload(struct mLockstep* l, int id) {
    (void) l;
    if (id) {
        pl[id].cycles_posted = 0;
        struct Player* m = &pl[0];
        m->wait_mask &= ~(1u << id);
        if (!m->wait_mask && m->awake < 1) {
            m->awake = 1;
        }
    } else {
        for (int i = 1; i < n_cons; ++i) {
            struct Player* p = &pl[i];
            p->cycles_posted += lockstep.players[0]->eventDiff;
            if (p->awake < 1) {
                nodes[i].nextEvent += p->cycles_posted;
                p->awake = 1;
            }
        }
    }
}

/* ---- logging: silence mGBA ---- */
static void quiet_log(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
    (void) logger; (void) category; (void) level; (void) format; (void) args;
}
static struct mLogger quiet = { .log = quiet_log };

/* ---- libretro plumbing ---- */
EXPORT unsigned retro_api_version(void) { return RETRO_API_VERSION; }

static struct retro_subsystem_memory_info mem_info[MAXN][2];
static struct retro_subsystem_rom_info rom_info[3][MAXN];
static struct retro_subsystem_info subs[4];
static const struct retro_variable vars[] = {
    { "mgbalink_audio_output", "Audio output; GBA #1|GBA #2|GBA #3|GBA #4" },
    { "mgbalink_rtc_epoch", "RTC start (unix seconds, same on every PC); 1767225600" },
    { NULL, NULL },
};

EXPORT void retro_set_environment(retro_environment_t e) {
    env_cb = e;
    for (int i = 0; i < MAXN; i++) {
        mem_info[i][0].extension = "srm"; mem_info[i][0].type = ((2 * (i + 1) - 1) << 8) | 0;
        mem_info[i][1].extension = "rtc"; mem_info[i][1].type = ((2 * (i + 1)) << 8) | 1;
    }
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < MAXN; i++) {
            rom_info[k][i].desc = "Game Boy Advance ROM";
            rom_info[k][i].valid_extensions = "gba";
            rom_info[k][i].need_fullpath = false;
            rom_info[k][i].block_extract = false;
            rom_info[k][i].required = true;
            rom_info[k][i].memory = mem_info[i];
            rom_info[k][i].num_memory = 2;
        }
    static const char* ids[3] = { "gba_link2", "gba_link3", "gba_link4" };
    for (int k = 0; k < 3; k++) {
        subs[k].desc = "GBA link cable";
        subs[k].ident = ids[k];
        subs[k].roms = rom_info[k];
        subs[k].num_roms = k + 2;
        subs[k].id = 0x102 + k;
    }
    env_cb(RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO, subs);
    env_cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*) vars);
}
EXPORT void retro_set_video_refresh(retro_video_refresh_t v) { video_cb = v; }
EXPORT void retro_set_audio_sample(retro_audio_sample_t a) { audio1_cb = a; }
EXPORT void retro_set_audio_sample_batch(retro_audio_sample_batch_t a) { audio_cb = a; }
EXPORT void retro_set_input_poll(retro_input_poll_t p) { poll_cb = p; }
EXPORT void retro_set_input_state(retro_input_state_t i) { input_cb = i; }

EXPORT void retro_get_system_info(struct retro_system_info* info) {
    memset(info, 0, sizeof(*info));
    info->library_name = "mGBA Link";
    info->library_version = "0.10.5-link";
    info->valid_extensions = "gba";
    info->need_fullpath = false;
    info->block_extract = false;
}

EXPORT void retro_get_system_av_info(struct retro_system_av_info* info) {
    memset(info, 0, sizeof(*info));
    int n = n_cons > 0 ? n_cons : 2;
    info->geometry.base_width = W * n;
    info->geometry.base_height = H;
    info->geometry.max_width = W * MAXN;
    info->geometry.max_height = H;
    info->geometry.aspect_ratio = (float) (W * n) / H;
    info->timing.fps = 16777216.0 / 280896.0;
    info->timing.sample_rate = SAMPLE_RATE;
}

EXPORT void retro_init(void) {
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
    env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
    mLogSetDefaultLogger(&quiet);
}

EXPORT void retro_deinit(void) {}
EXPORT void retro_set_controller_port_device(unsigned port, unsigned device) { (void) port; (void) device; }

static void do_deferred_setup(void) {
    if (!deferred_setup) return;
    for (int i = 0; i < n_cons; i++) {
        struct VFile* save = VFileFromMemory(sav[i], SIZE_CART_FLASH1M);
        if (!cores[i]->loadSave(cores[i], save)) save->close(save);
    }
    deferred_setup = false;
}

EXPORT void retro_unload_game(void) {
    for (int i = 0; i < n_cons; i++) {
        if (cores[i]) {
            mCoreConfigDeinit(&cores[i]->config);
            cores[i]->deinit(cores[i]);
            cores[i] = NULL;
        }
        free(vbuf[i]); vbuf[i] = NULL;
        free(sav[i]); sav[i] = NULL;
    }
    free(out_frame); out_frame = NULL;
    free(abuf); abuf = NULL; abuf_cap = 0;
    n_cons = 0;
}

static void reset_all(void) {
    memset(pl, 0, sizeof(pl));
    for (int i = 0; i < n_cons; i++) pl[i].awake = 1;
    for (int i = 0; i < n_cons; i++) cores[i]->reset(cores[i]);
    /* reset runs the nodes' load() for the default (normal) SIO mode; start every console awake with no posted cycles */
    for (int i = 0; i < n_cons; i++) { pl[i].awake = 1; pl[i].cycles_posted = 0; pl[i].wait_mask = 0; }
}

EXPORT bool retro_load_game_special(unsigned game_type, const struct retro_game_info* info, size_t num_info) {
    if (game_type < 0x102 || game_type > 0x104 || num_info != (size_t) (game_type - 0x100) || n_cons) return false;
    n_cons = (int) num_info;
    dbg = getenv("MGBALINK_DEBUG") != NULL;

    struct retro_variable v = { "mgbalink_audio_output", NULL };
    audio_sel = 0;
    if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &v) && v.value && strncmp(v.value, "GBA #", 5) == 0) {
        audio_sel = v.value[5] - '1';
        if (audio_sel < 0 || audio_sel >= n_cons) audio_sel = 0;
    }
    int64_t epoch = 1767225600;
    struct retro_variable ev = { "mgbalink_rtc_epoch", NULL };
    if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &ev) && ev.value && *ev.value) epoch = strtoll(ev.value, NULL, 10);

    mLockstepInit(&lockstep.d);
    GBASIOLockstepInit(&lockstep);
    lockstep.d.lock = ls_lock;
    lockstep.d.unlock = ls_unlock;
    lockstep.d.signal = ls_signal;
    lockstep.d.wait = ls_wait;
    lockstep.d.addCycles = ls_add_cycles;
    lockstep.d.useCycles = ls_use_cycles;
    lockstep.d.unusedCycles = ls_unused_cycles;
    lockstep.d.unload = ls_unload;
    lockstep.d.context = NULL;

    static struct mRTCGenericSource rtcs[MAXN];
    for (int i = 0; i < n_cons; i++) {
        void* data = malloc(info[i].size);
        if (!info[i].data || !data) { free(data); return false; }
        memcpy(data, info[i].data, info[i].size);
        struct VFile* rom = VFileFromMemory(data, info[i].size);   /* leaked on purpose: the core keeps pointing into it */
        struct mCore* c = mCoreFindVF(rom);
        if (!c) { rom->close(rom); return false; }
        mCoreInitConfig(c, NULL);
        c->init(c);
        cores[i] = c;
        vbuf[i] = (color_t*) malloc(STRIDE * H * sizeof(color_t));
        memset(vbuf[i], 0xFF, STRIDE * H * sizeof(color_t));
        c->setVideoBuffer(c, vbuf[i], STRIDE);
        sav[i] = (uint8_t*) malloc(SIZE_CART_FLASH1M);
        memset(sav[i], 0xFF, SIZE_CART_FLASH1M);
        c->setAudioBufferSize(c, 2048);
        blip_set_rates(c->getAudioChannel(c, 0), c->frequency(c), SAMPLE_RATE);
        blip_set_rates(c->getAudioChannel(c, 1), c->frequency(c), SAMPLE_RATE);

        mCoreConfigSetDefaultValue(&c->config, "idleOptimization", "remove");
        mCoreConfigSetDefaultIntValue(&c->config, "useBios", 0);       /* HLE BIOS: no file dependency, identical on every PC */
        mCoreConfigSetDefaultIntValue(&c->config, "skipBios", 0);
        mCoreConfigSetDefaultIntValue(&c->config, "frameskip", 0);
        mCoreConfigSetDefaultIntValue(&c->config, "allowOpposingDirections", 0);
        mCoreLoadConfig(c);

        /* deterministic clock: advances with emulated frames only */
        mRTCGenericSourceInit(&rtcs[i], c);
        rtcs[i].override = RTC_FAKE_EPOCH;
        rtcs[i].value = epoch * 1000;
        mCoreSetRTC(c, &rtcs[i].d);

        c->loadROM(c, rom);

        GBASIOLockstepNodeCreate(&nodes[i]);
        GBASIOLockstepAttachNode(&lockstep, &nodes[i]);
        struct GBA* gba = (struct GBA*) c->board;
        GBASIOSetDriver(&gba->sio, &nodes[i].d, SIO_MULTI);
        GBASIOSetDriver(&gba->sio, &nodes[i].d, SIO_NORMAL_32);
    }
    out_frame = (uint16_t*) calloc((size_t) W * n_cons * H, sizeof(uint16_t));
    reset_all();
    deferred_setup = true;
    frame_no = 0;
    return true;
}

EXPORT bool retro_load_game(const struct retro_game_info* game) { (void) game; return false; }

EXPORT void retro_reset(void) { reset_all(); }

/* One emulated frame for every console. Consoles are stepped in index order, never past their frame boundary unless
 * every unfinished console is asleep (waiting on one that already finished), so the schedule depends only on emulation. */
static void run_all(void) {
    uint32_t start[MAXN];
    for (int i = 0; i < n_cons; i++) start[i] = (uint32_t) cores[i]->frameCounter(cores[i]);
    for (int guard = 0; guard < 4000000; guard++) {
        bool done[MAXN], all_done = true;
        for (int i = 0; i < n_cons; i++) {
            done[i] = (uint32_t) cores[i]->frameCounter(cores[i]) != start[i];
            if (!done[i]) all_done = false;
        }
        if (all_done) return;
        bool ran = false;
        for (int i = 0; i < n_cons; i++)
            if (!done[i] && pl[i].awake) { cores[i]->runLoop(cores[i]); ran = true; }
        if (ran) continue;
        for (int i = 0; i < n_cons; i++)
            if (done[i] && pl[i].awake) { cores[i]->runLoop(cores[i]); ran = true; }
        if (!ran) {   /* everyone asleep: cannot happen with a correct lockstep; kick the master rather than hang */
            for (int i = 0; i < n_cons; i++) pl[i].awake = 1;
        }
    }
}

EXPORT void retro_run(void) {
    do_deferred_setup();
    poll_cb();
    for (int i = 0; i < n_cons; i++) {
        uint16_t keys = 0;
        int16_t mask = input_cb((unsigned) i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
        for (int k = 0; k < 10; k++) keys |= (uint16_t) (((mask >> keymap[k]) & 1) << k);
        cores[i]->setKeys(cores[i], keys);
    }
    run_all();
    frame_no++;
    if (dbg && (frame_no % 100 == 0)) {
        fprintf(stderr, "[mgba_link] frame %llu waits=%llu phase=%d attMulti=%d", frame_no, n_wait, (int) lockstep.d.transferActive, lockstep.attachedMulti);
        for (int i = 0; i < n_cons; i++) { struct GBA* g = (struct GBA*) cores[i]->board; fprintf(stderr, " | c%d mode=%d siocnt=%04X multi1=%04X", i, (int) g->sio.mode, g->sio.siocnt, g->memory.io[REG_SIOMULTI1 >> 1]); }
        fputc(10, stderr);
    }

    for (int c = 0; c < n_cons; c++)
        for (int y = 0; y < H; y++)
            memcpy(out_frame + (size_t) y * W * n_cons + (size_t) c * W, vbuf[c] + (size_t) y * STRIDE, W * sizeof(uint16_t));
    video_cb(out_frame, W * n_cons, H, (size_t) W * n_cons * sizeof(uint16_t));

    for (int c = 0; c < n_cons; c++) {
        blip_t* l = cores[c]->getAudioChannel(cores[c], 0);
        blip_t* r = cores[c]->getAudioChannel(cores[c], 1);
        int avail = blip_samples_avail(l);
        if (avail <= 0) continue;
        if (c != audio_sel) {   /* drain so the blip buffer never overflows */
            if (abuf_cap < (size_t) avail * 2) { abuf_cap = (size_t) avail * 2; abuf = (int16_t*) realloc(abuf, abuf_cap * sizeof(int16_t)); }
            blip_read_samples(l, abuf, avail, true);
            blip_read_samples(r, abuf + 1, avail, true);
            continue;
        }
        if (abuf_cap < (size_t) avail * 2) { abuf_cap = (size_t) avail * 2; abuf = (int16_t*) realloc(abuf, abuf_cap * sizeof(int16_t)); }
        int got = blip_read_samples(l, abuf, avail, true);
        blip_read_samples(r, abuf + 1, avail, true);
        if (got > 0 && audio_cb) audio_cb(abuf, (size_t) got);
    }
}

/* ---- state: every console + the lockstep bookkeeping, so a state hash covers the whole simulation ---- */
static size_t extra_size(void) { return (size_t) (4 + n_cons * 16 + MAXN * 6 + 4 * 4 + MAXN * 24 + MAXN * 24) + 64; }

static size_t core_state_size(void) { return cores[0]->stateSize(cores[0]); }

EXPORT size_t retro_serialize_size(void) {
    if (!n_cons) return 0;
    do_deferred_setup();
    return (core_state_size() + SIZE_CART_FLASH1M + (size_t) STRIDE * H * sizeof(color_t) + sizeof(struct GBAVideoSoftwareRenderer)) * n_cons + extra_size();
}

static uint8_t* put32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); return p + 4; }

EXPORT bool retro_serialize(void* data, size_t size) {
    if (!n_cons || size < retro_serialize_size()) return false;
    uint8_t* p = (uint8_t*) data;
    size_t cs = core_state_size();
    for (int i = 0; i < n_cons; i++) {
        if (!cores[i]->saveState(cores[i], p)) return false;
        p += cs;
        memcpy(p, sav[i], SIZE_CART_FLASH1M);   /* cart save (flash/SRAM) is not in the core state, but games write it mid-run: rollback must restore it */
        p += SIZE_CART_FLASH1M;
        /* a console can run a few scanlines of the NEXT frame before the frame is handed out: those rows live in the video buffer,
         * so it is part of the state (otherwise a rollback re-presents rows drawn by the discarded future) */
        memcpy(p, vbuf[i], (size_t) STRIDE * H * sizeof(color_t));
        p += (size_t) STRIDE * H * sizeof(color_t);
        /* the software renderer keeps per-scanline caches / latched IO outside the savestate; mGBA only rebuilds them from registers */
        memcpy(p, ((struct GBA*) cores[i]->board)->video.renderer, sizeof(struct GBAVideoSoftwareRenderer));
        p += sizeof(struct GBAVideoSoftwareRenderer);
    }
    memset(p, 0, extra_size());
    p = put32(p, (uint32_t) lockstep.d.transferActive);
    p = put32(p, (uint32_t) lockstep.d.transferCycles);
    p = put32(p, (uint32_t) lockstep.attachedMulti);
    p = put32(p, (uint32_t) lockstep.attachedNormal);
    for (int i = 0; i < MAXN; i++) { p = put32(p, lockstep.multiRecv[i]); p = put32(p, lockstep.normalRecv[i]); }
    for (int i = 0; i < n_cons; i++) {
        p = put32(p, (uint32_t) nodes[i].nextEvent);
        p = put32(p, (uint32_t) nodes[i].eventDiff);
        p = put32(p, (uint32_t) (nodes[i].normalSO | (nodes[i].transferFinished << 1) | ((unsigned) nodes[i].mode << 8)));
        p = put32(p, (uint32_t) pl[i].cycles_posted);
        p = put32(p, (uint32_t) pl[i].awake);
        p = put32(p, pl[i].wait_mask);
    }
    /* cart RTC bookkeeping that mGBA leaves out of its state: the offset set when the game writes the clock, and the last latch */
    for (int i = 0; i < n_cons; i++) {
        struct GBA* g = (struct GBA*) cores[i]->board;
        int64_t off = (int64_t) g->memory.hw.rtc.offset, ll = (int64_t) g->memory.hw.rtc.lastLatch;
        memcpy(p, &off, 8); p += 8;
        memcpy(p, &ll, 8); p += 8;
        /* the lockstep node's timing event: loadState clears the core's event queue, so it must be re-armed or the link clock stalls after a rollback */
        int sch = mTimingIsScheduled(&g->timing, &nodes[i].event);
        p = put32(p, (uint32_t) sch);
        p = put32(p, (uint32_t) (sch ? nodes[i].event.when - mTimingCurrentTime(&g->timing) : 0));
    }
    return true;
}

static const uint8_t* get32(const uint8_t* p, uint32_t* v) { memcpy(v, p, 4); return p + 4; }

EXPORT bool retro_unserialize(const void* data, size_t size) {
    if (!n_cons || size < retro_serialize_size()) return false;
    do_deferred_setup();
    const uint8_t* p = (const uint8_t*) data;
    size_t cs = core_state_size();
    for (int i = 0; i < n_cons; i++) {
        if (!cores[i]->loadState(cores[i], p)) return false;
        p += cs;
        memcpy(sav[i], p, SIZE_CART_FLASH1M);
        p += SIZE_CART_FLASH1M;
        memcpy(vbuf[i], p, (size_t) STRIDE * H * sizeof(color_t));
        p += (size_t) STRIDE * H * sizeof(color_t);
        memcpy(((struct GBA*) cores[i]->board)->video.renderer, p, sizeof(struct GBAVideoSoftwareRenderer));
        p += sizeof(struct GBAVideoSoftwareRenderer);
    }
    uint32_t v;
    p = get32(p, &v); lockstep.d.transferActive = (enum mLockstepPhase) v;
    p = get32(p, &v); lockstep.d.transferCycles = (int32_t) v;
    p = get32(p, &v); lockstep.attachedMulti = (int) v;
    p = get32(p, &v); lockstep.attachedNormal = (int) v;
    for (int i = 0; i < MAXN; i++) { p = get32(p, &v); lockstep.multiRecv[i] = (uint16_t) v; p = get32(p, &v); lockstep.normalRecv[i] = v; }
    for (int i = 0; i < n_cons; i++) {
        p = get32(p, &v); nodes[i].nextEvent = (int32_t) v;
        p = get32(p, &v); nodes[i].eventDiff = (int32_t) v;
        p = get32(p, &v); nodes[i].normalSO = v & 1; nodes[i].transferFinished = (v >> 1) & 1; nodes[i].mode = (enum GBASIOMode) ((v >> 8) & 0xFF);
        p = get32(p, &v); pl[i].cycles_posted = (int32_t) v;
        p = get32(p, &v); pl[i].awake = (int) v;
        p = get32(p, &v); pl[i].wait_mask = v;
    }
    for (int i = 0; i < n_cons; i++) {
        struct GBA* g = (struct GBA*) cores[i]->board;
        int64_t off, ll;
        memcpy(&off, p, 8); p += 8;
        memcpy(&ll, p, 8); p += 8;
        g->memory.hw.rtc.offset = (time_t) off;
        g->memory.hw.rtc.lastLatch = (time_t) ll;
        uint32_t sch, dt;
        p = get32(p, &sch); p = get32(p, &dt);
        if (mTimingIsScheduled(&g->timing, &nodes[i].event)) mTimingDeschedule(&g->timing, &nodes[i].event);
        if (sch) mTimingSchedule(&g->timing, &nodes[i].event, (int32_t) dt);
    }
    return true;
}

EXPORT void retro_cheat_reset(void) {}
EXPORT void retro_cheat_set(unsigned index, bool enabled, const char* code) { (void) index; (void) enabled; (void) code; }
EXPORT unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

static int mem_console(unsigned id, unsigned* type) {
    *type = id & 0xFF;
    unsigned g = id >> 8;
    int inst = g ? (int) ((g - 1) / 2) : 0;
    return inst < n_cons ? inst : -1;
}

EXPORT void* retro_get_memory_data(unsigned id) {
    unsigned t;
    int c = mem_console(id, &t);
    if (c < 0 || t != RETRO_MEMORY_SAVE_RAM) return NULL;
    return sav[c];
}

EXPORT size_t retro_get_memory_size(unsigned id) {
    unsigned t;
    int c = mem_console(id, &t);
    if (c < 0 || t != RETRO_MEMORY_SAVE_RAM) return 0;
    struct GBA* gba = (struct GBA*) cores[c]->board;
    if (gba->memory.savedata.type == SAVEDATA_AUTODETECT || gba->memory.savedata.type == SAVEDATA_FORCE_NONE) return SIZE_CART_FLASH1M;
    return GBASavedataSize(&gba->memory.savedata);
}

EXPORT bool retro_unused_placeholder(void) { return false; }
EXPORT void retro_unused_placeholder2(void) { (void) audio1_cb; }
