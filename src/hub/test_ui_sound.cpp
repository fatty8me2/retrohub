/* Menu sounds: every UI sfx and the ambient loop render to non-silent, finite, bounded buffers (never played).
 */
#include "boot_synth.h"
#include <cmath>
#include <cstdio>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL %s: ", #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static void check(const char* name, const std::vector<float>& b, double max_sec, double min_sec) {
    double sec = b.size() / 2.0 / bootsynth::kSampleRate;
    float peak = 0; bool bad = false; double sq = 0;
    for (float v : b) { if (!std::isfinite(v)) bad = true; peak = std::fmax(peak, std::fabs(v)); sq += double(v) * v; }
    CHECK(!bad, "%s NaN/inf", name);
    CHECK(b.size() % 2 == 0, "%s odd sample count", name);
    CHECK(peak > 0.01f, "%s silent (peak %.4f)", name, peak);
    CHECK(peak <= 0.95f, "%s clips (peak %.3f)", name, peak);
    CHECK(sec >= min_sec && sec <= max_sec, "%s length %.3fs outside %.2f-%.2f", name, sec, min_sec, max_sec);
    CHECK(std::fabs(b.front()) < 0.05f && std::fabs(b.back()) < 0.01f, "%s does not start/end near zero", name);
    CHECK(std::sqrt(sq / b.size()) < 0.2, "%s too loud for a menu sound", name);
}

int main() {
    using bootsynth::Ui;
    const char* names[] = {"move", "select", "back", "error", "popup", "chime"};
    for (int i = 0; i < (int)Ui::Count; i++) {
        auto a = bootsynth::render_ui((Ui)i), b = bootsynth::render_ui((Ui)i);
        check(names[i], a, bootsynth::kUiMaxSeconds, 0.05);
        CHECK(a == b, "%s not deterministic", names[i]);
    }
    check("ambient", bootsynth::render_ambient(), bootsynth::kAmbientSeconds + 0.01, bootsynth::kAmbientSeconds - 0.01);
    std::printf(fails ? "hub_uisound_test: %d FAILED\n" : "hub_uisound_test: all passed\n", fails);
    return fails ? 1 : 0;
}
