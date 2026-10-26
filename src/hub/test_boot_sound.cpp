/* Renders every startup sound to memory and checks it; writes WAVs to qa/out/boot_sounds/ for review (never played). */
#include "boot_synth.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL %s: ", #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
    using hub::System;
    const System all[] = {System::N64, System::GB, System::GBC, System::GBA, System::NDS, System::N3DS,
                          System::GC, System::PS1, System::PS2, System::WiiU, System::NES, System::SNES};
    std::filesystem::path dir = std::filesystem::path(BOOT_SOUND_OUT_DIR);
    std::filesystem::create_directories(dir);
    for (System s : all) {
        std::string name = hub::system_short(s);
        auto a = bootsynth::render(s), b = bootsynth::render(s);
        double sec = a.size() / 2.0 / bootsynth::kSampleRate;
        float peak = 0; double sq = 0; bool bad = false;
        for (float v : a) { if (!std::isfinite(v)) bad = true; peak = std::fmax(peak, std::fabs(v)); sq += double(v) * v; }
        double rms = std::sqrt(sq / (a.size() ? a.size() : 1));
        CHECK(!bad, "%s has NaN/inf", name.c_str());
        CHECK(sec >= 1.0 && sec <= 3.0, "%s length %.2fs", name.c_str(), sec);
        CHECK(peak <= 0.95f, "%s clips (peak %.3f)", name.c_str(), peak);
        CHECK(peak >= 0.5f && rms > 0.01, "%s too quiet (peak %.3f rms %.4f)", name.c_str(), peak, rms);
        CHECK(std::fabs(a[a.size() - 1]) < 0.01f && std::fabs(a[a.size() - 2]) < 0.01f, "%s tail not silent", name.c_str());
        CHECK(a == b, "%s not deterministic", name.c_str());
        CHECK(bootsynth::write_wav((dir / (name + ".wav")).string().c_str(), a), "%s wav write", name.c_str());
        std::printf("%-5s %.2fs peak %.2f rms %.3f\n", name.c_str(), sec, peak, rms);
    }
    CHECK(bootsynth::render(System::Unknown).empty(), "unknown has no sound");
    std::printf(fails ? "FAILED (%d)\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
