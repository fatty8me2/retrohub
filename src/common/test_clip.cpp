#include "clip.h"
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* A moving box on a gradient: 256x224 BGRA, so a decoded clip is visibly non-black and changes frame to frame. */
static void make_frame(std::vector<uint8_t>* px, int w, int h, int n) {
    px->assign((size_t)w * h * 4, 0);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t* p = &(*px)[((size_t)y * w + x) * 4];
            p[0] = (uint8_t)(40 + y / 2); p[1] = (uint8_t)(60 + x / 3); p[2] = 90;
            if (x > (n * 3) % (w - 40) && x < (n * 3) % (w - 40) + 40 && y > 80 && y < 140) { p[0] = 40; p[1] = 220; p[2] = 240; }
        }
}

int main() {
    CHECK(clip::sample_every(60, 20) == 3);
    CHECK(clip::sample_every(59.7275, 20) == 3);
    CHECK(clip::sample_every(30, 20) == 2 || clip::sample_every(30, 20) == 1);
    int dw, dh;
    clip::scaled_size(640, 480, 320, &dw, &dh);
    CHECK(dw == 320 && dh == 240);
    clip::scaled_size(160, 144, 320, &dw, &dh);   // never upscaled
    CHECK(dw == 160 && dh == 144);
    clip::scaled_size(321, 241, 320, &dw, &dh);
    CHECK(dw % 2 == 0 && dh % 2 == 0);

    {   // caps: time and memory
        clip::Config c;
        CHECK(clip::capacity_frames(c, 320, 240) == 600);                 // 30 s * 20 fps fits in 80 MB (115 KB each)
        c.cap_bytes = 1024 * 1024;
        CHECK(clip::capacity_frames(c, 320, 240) == 1024 * 1024 / (320 * 240 * 3 / 2));
        clip::Ring r(c);
        std::vector<uint8_t> f;
        make_frame(&f, 256, 224, 0);
        for (int i = 0; i < 3000; i++) r.offer(f.data(), 256, 224, 256 * 4, 60.0);
        CHECK(r.frames() == clip::capacity_frames(c, 256, 224));
        CHECK(r.bytes() <= c.cap_bytes);
        r.offer(f.data(), 128, 112, 128 * 4, 60.0);   // size change drops the stale footage
        CHECK(r.frames() <= 1);
    }

    {   // round trip: 12 s of 60 fps game -> clip -> decode
        std::string dir = (std::filesystem::temp_directory_path() / "clip_test_out").string();
        std::filesystem::remove_all(dir);
        clip::Recorder rec;
        rec.set_dir(dir);
        std::string msg;
        CHECK(!rec.request_save(std::time(nullptr), &msg));   // nothing yet
        std::vector<uint8_t> f;
        for (int i = 0; i < 720; i++) { make_frame(&f, 256, 224, i); rec.offer(f.data(), 256, 224, 256 * 4, 60.0); }
        CHECK(rec.ring().frames() == 240);
        CHECK(rec.request_save(std::time(nullptr), &msg));
        CHECK(!rec.request_save(std::time(nullptr), &msg));   // busy
        bool ok = false, got = false;
        for (int i = 0; i < 600 && !got; i++) { got = rec.poll(&ok, &msg); if (!got) Sleep(50); }
        std::printf("save: %d %s\n", (int)ok, msg.c_str());
        CHECK(got && ok);
        if (got && ok) {
            clip::Info info;
            std::string err;
            bool pr = clip::probe_mp4(msg, &info, &err);
            std::printf("probe: %d w=%d h=%d frames=%d secs=%.2f luma=%.1f bytes=%llu %s\n", (int)pr, info.w, info.h, info.frames, info.seconds,
                        info.mean_luma, (unsigned long long)info.bytes, err.c_str());
            CHECK(pr);
            CHECK(info.w == 256 && info.h == 224);
            CHECK(info.frames >= 230 && info.frames <= 241);
            CHECK(info.seconds > 11.0 && info.seconds < 13.0);
            CHECK(info.mean_luma > 20.0);
            CHECK(std::filesystem::exists(std::filesystem::u8path(msg + ".jpg")));
        }
        if (std::getenv("CLIP_TEST_KEEP")) std::printf("kept: %s\n", dir.c_str());
        else std::filesystem::remove_all(dir);
    }
    std::printf(fails ? "clip_test: %d FAILED\n" : "clip_test: all passed\n", fails);
    return fails ? 1 : 0;
}
